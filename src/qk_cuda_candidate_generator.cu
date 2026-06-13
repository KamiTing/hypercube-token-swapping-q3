#include "qk/cuda_candidate_generator.h"

#include <cub/device/device_topk.cuh>
#include <cuda_runtime.h>
#include <cuda/__execution/determinism.h>
#include <cuda/__execution/output_ordering.h>
#include <cuda/__execution/require.h>
#include <thrust/device_ptr.h>
#include <thrust/sort.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std;

namespace qk {
namespace {

struct DeviceFingerprint128 {
    uint64_t low;
    uint64_t high;
};

struct DeviceEdge {
    int u;
    int v;
    int bit;
};

struct DeviceCandidate {
    int total_dist;
    int misplaced;
    int max_dist;
    int parent_index;
    int edge_id;
    int parent_path_node;
    DeviceFingerprint128 fingerprint;
    long long order;
    int valid;
};

struct DeviceCandidateLess {
    __host__ __device__ bool operator()(const DeviceCandidate& a, const DeviceCandidate& b) const {
        if (a.valid != b.valid) {
            return a.valid > b.valid;
        }
        if (!a.valid) {
            return false;
        }
        if (a.total_dist != b.total_dist) {
            return a.total_dist < b.total_dist;
        }
        if (a.max_dist != b.max_dist) {
            return a.max_dist < b.max_dist;
        }
        if (a.misplaced != b.misplaced) {
            return a.misplaced < b.misplaced;
        }
        if (a.edge_id != b.edge_id) {
            return a.edge_id < b.edge_id;
        }
        if (a.fingerprint.high != b.fingerprint.high) {
            return a.fingerprint.high < b.fingerprint.high;
        }
        if (a.fingerprint.low != b.fingerprint.low) {
            return a.fingerprint.low < b.fingerprint.low;
        }
        return a.order < b.order;
    }
};

enum class DeviceTopKKeyMode {
    Greedy = 0,
    MaxDistance = 1,
    Misplaced = 2,
    Perturb = 3,
    EdgeBit = 4
};

__host__ __device__ uint64_t splitmix64_device(uint64_t value) {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31);
}

__host__ __device__ DeviceFingerprint128 fingerprint_component_device(int position, int token) {
    uint64_t pair = (static_cast<uint64_t>(static_cast<uint32_t>(position)) << 32) |
                    static_cast<uint32_t>(token);
    return {
        splitmix64_device(pair ^ 0x243F6A8885A308D3ULL),
        splitmix64_device(pair ^ 0x13198A2E03707344ULL)
    };
}

__host__ __device__ DeviceFingerprint128 fingerprint_after_swap_device(
    DeviceFingerprint128 fingerprint,
    int u,
    int v,
    int token_u,
    int token_v
) {
    DeviceFingerprint128 old_u = fingerprint_component_device(u, token_u);
    DeviceFingerprint128 old_v = fingerprint_component_device(v, token_v);
    DeviceFingerprint128 new_u = fingerprint_component_device(u, token_v);
    DeviceFingerprint128 new_v = fingerprint_component_device(v, token_u);
    fingerprint.low ^= old_u.low ^ old_v.low ^ new_u.low ^ new_v.low;
    fingerprint.high ^= old_u.high ^ old_v.high ^ new_u.high ^ new_v.high;
    return fingerprint;
}

__host__ __device__ uint64_t fingerprint_hash_device(DeviceFingerprint128 fingerprint) {
    return splitmix64_device(fingerprint.low ^ (fingerprint.high + 0x9E3779B97F4A7C15ULL));
}

__device__ int hdist_device(int a, int b) {
    return __popc(static_cast<unsigned>(a ^ b));
}

__device__ bool fingerprint_equal(DeviceFingerprint128 a, DeviceFingerprint128 b) {
    return a.low == b.low && a.high == b.high;
}

__device__ bool recent_contains(
    DeviceFingerprint128 fingerprint,
    const DeviceFingerprint128* table,
    const unsigned int* used,
    size_t table_mask
) {
    if (table_mask == 0) {
        return false;
    }
    size_t slot = static_cast<size_t>(fingerprint_hash_device(fingerprint)) & table_mask;
    for (size_t probe = 0; probe <= table_mask; ++probe) {
        size_t index = (slot + probe) & table_mask;
        if (used[index] == 0) {
            return false;
        }
        if (used[index] != 2) {
            continue;
        }
        if (fingerprint_equal(table[index], fingerprint)) {
            return true;
        }
    }
    return false;
}

__global__ void generate_candidates_kernel(
    const uint16_t* states,
    const int* last_edges,
    const int* path_nodes,
    const DeviceFingerprint128* parent_fingerprints,
    const int* parent_total_dist,
    const int* parent_misplaced,
    const int* parent_histograms,
    const DeviceEdge* edges,
    int parent_count,
    int edge_count,
    int node_count,
    long long candidate_order_base,
    const DeviceFingerprint128* recent_table,
    const unsigned int* recent_used,
    size_t recent_mask,
    size_t logical_offset,
    size_t chunk_count,
    DeviceCandidate* candidates,
    unsigned long long* valid_count
) {
    size_t logical = static_cast<size_t>(parent_count) * static_cast<size_t>(edge_count);
    size_t local_index = static_cast<size_t>(blockIdx.x) * static_cast<size_t>(blockDim.x) +
                         static_cast<size_t>(threadIdx.x);
    if (local_index >= chunk_count) {
        return;
    }
    size_t index = logical_offset + local_index;
    if (index >= logical) {
        return;
    }

    int parent_index = static_cast<int>(index / static_cast<size_t>(edge_count));
    int edge_id = static_cast<int>(index % static_cast<size_t>(edge_count));
    DeviceCandidate out{};
    out.total_dist = INT_MAX;
    out.misplaced = INT_MAX;
    out.max_dist = INT_MAX;
    out.parent_index = parent_index;
    out.edge_id = edge_id;
    out.parent_path_node = path_nodes[parent_index];
    out.order = candidate_order_base +
                static_cast<long long>(parent_index) * static_cast<long long>(edge_count) +
                static_cast<long long>(edge_id) +
                1LL;
    out.valid = 0;

    if (edge_id == last_edges[parent_index]) {
        candidates[local_index] = out;
        return;
    }

    DeviceEdge edge = edges[edge_id];
    size_t state_base = static_cast<size_t>(parent_index) * static_cast<size_t>(node_count);
    int token_u = static_cast<int>(states[state_base + static_cast<size_t>(edge.u)]);
    int token_v = static_cast<int>(states[state_base + static_cast<size_t>(edge.v)]);

    int old_u_dist = hdist_device(edge.u, token_u);
    int old_v_dist = hdist_device(edge.v, token_v);
    int new_u_dist = hdist_device(edge.u, token_v);
    int new_v_dist = hdist_device(edge.v, token_u);

    int histogram[16];
    const int* parent_hist = parent_histograms + parent_index * 16;
    #pragma unroll
    for (int i = 0; i < 16; ++i) {
        histogram[i] = parent_hist[i];
    }
    histogram[old_u_dist]--;
    histogram[old_v_dist]--;
    histogram[new_u_dist]++;
    histogram[new_v_dist]++;
    int max_dist = 0;
    #pragma unroll
    for (int d = 15; d >= 0; --d) {
        if (histogram[d] > 0) {
            max_dist = d;
            break;
        }
    }

    DeviceFingerprint128 fingerprint = fingerprint_after_swap_device(
        parent_fingerprints[parent_index],
        edge.u,
        edge.v,
        token_u,
        token_v
    );
    if (recent_contains(fingerprint, recent_table, recent_used, recent_mask)) {
        candidates[local_index] = out;
        return;
    }

    out.total_dist =
        parent_total_dist[parent_index] -
        old_u_dist -
        old_v_dist +
        new_u_dist +
        new_v_dist;
    out.misplaced =
        parent_misplaced[parent_index] -
        (token_u != edge.u) -
        (token_v != edge.v) +
        (token_v != edge.u) +
        (token_u != edge.v);
    out.max_dist = max_dist;
    out.fingerprint = fingerprint;
    out.valid = 1;
    atomicAdd(valid_count, 1ULL);
    candidates[local_index] = out;
}

__host__ __device__ unsigned long long candidate_prefix_key_device(const DeviceCandidate& candidate) {
    constexpr unsigned long long total_mask = (1ULL << 22) - 1ULL;
    constexpr unsigned long long max_mask = (1ULL << 5) - 1ULL;
    constexpr unsigned long long misplaced_mask = (1ULL << 17) - 1ULL;
    constexpr unsigned long long edge_mask = (1ULL << 19) - 1ULL;

    unsigned long long invalid = candidate.valid ? 0ULL : 1ULL;
    unsigned long long total = static_cast<unsigned long long>(
        candidate.total_dist > 0 ? candidate.total_dist : 0
    ) & total_mask;
    unsigned long long max_dist = static_cast<unsigned long long>(
        candidate.max_dist > 0 ? candidate.max_dist : 0
    ) & max_mask;
    unsigned long long misplaced = static_cast<unsigned long long>(
        candidate.misplaced > 0 ? candidate.misplaced : 0
    ) & misplaced_mask;
    unsigned long long edge = static_cast<unsigned long long>(
        candidate.edge_id > 0 ? candidate.edge_id : 0
    ) & edge_mask;
    return (invalid << 63) | (total << 41) | (max_dist << 36) | (misplaced << 19) | edge;
}

__host__ __device__ unsigned long long pack_limited_key_device(
    int invalid,
    unsigned long long primary,
    unsigned long long secondary,
    unsigned long long tertiary,
    unsigned long long edge
) {
    constexpr unsigned long long primary_mask = (1ULL << 20) - 1ULL;
    constexpr unsigned long long secondary_mask = (1ULL << 20) - 1ULL;
    constexpr unsigned long long tertiary_mask = (1ULL << 5) - 1ULL;
    constexpr unsigned long long edge_mask = (1ULL << 18) - 1ULL;
    return (static_cast<unsigned long long>(invalid ? 1 : 0) << 63) |
           ((primary & primary_mask) << 43) |
           ((secondary & secondary_mask) << 23) |
           ((tertiary & tertiary_mask) << 18) |
           (edge & edge_mask);
}

__host__ __device__ unsigned long long perturbation_key_device(
    const DeviceCandidate& candidate,
    int depth
) {
    uint64_t key =
        candidate.fingerprint.low ^
        (candidate.fingerprint.high + 0x9E3779B97F4A7C15ULL) ^
        (static_cast<uint64_t>(depth) * 0xBF58476D1CE4E5B9ULL) ^
        (static_cast<uint64_t>(candidate.edge_id + 1) * 0x94D049BB133111EBULL) ^
        static_cast<uint64_t>(candidate.order);
    return splitmix64_device(key);
}

__host__ __device__ unsigned long long candidate_diverse_key_device(
    const DeviceCandidate& candidate,
    const DeviceEdge* edges,
    int mode,
    int target_bit,
    int depth
) {
    if (!candidate.valid) {
        return ~0ULL;
    }
    unsigned long long total = static_cast<unsigned long long>(
        candidate.total_dist > 0 ? candidate.total_dist : 0
    );
    unsigned long long max_dist = static_cast<unsigned long long>(
        candidate.max_dist > 0 ? candidate.max_dist : 0
    );
    unsigned long long misplaced = static_cast<unsigned long long>(
        candidate.misplaced > 0 ? candidate.misplaced : 0
    );
    unsigned long long edge = static_cast<unsigned long long>(
        candidate.edge_id > 0 ? candidate.edge_id : 0
    );

    switch (static_cast<DeviceTopKKeyMode>(mode)) {
        case DeviceTopKKeyMode::Greedy:
            return candidate_prefix_key_device(candidate);
        case DeviceTopKKeyMode::MaxDistance:
            return pack_limited_key_device(0, max_dist, total, misplaced, edge);
        case DeviceTopKKeyMode::Misplaced:
            return pack_limited_key_device(0, misplaced, total, max_dist, edge);
        case DeviceTopKKeyMode::Perturb:
            return perturbation_key_device(candidate, depth);
        case DeviceTopKKeyMode::EdgeBit: {
            int edge_bit = -1;
            if (candidate.edge_id >= 0) {
                edge_bit = edges[candidate.edge_id].bit;
            }
            if (edge_bit != target_bit) {
                return ~0ULL;
            }
            return candidate_prefix_key_device(candidate);
        }
    }
    return ~0ULL;
}

__global__ void pack_candidate_prefix_keys_kernel(
    const DeviceCandidate* candidates,
    size_t count,
    unsigned long long* keys
) {
    size_t index = static_cast<size_t>(blockIdx.x) * static_cast<size_t>(blockDim.x) +
                   static_cast<size_t>(threadIdx.x);
    if (index >= count) {
        return;
    }
    keys[index] = candidate_prefix_key_device(candidates[index]);
}

__global__ void pack_candidate_diverse_keys_kernel(
    const DeviceCandidate* candidates,
    const DeviceEdge* edges,
    size_t count,
    int mode,
    int target_bit,
    int depth,
    unsigned long long* keys
) {
    size_t index = static_cast<size_t>(blockIdx.x) * static_cast<size_t>(blockDim.x) +
                   static_cast<size_t>(threadIdx.x);
    if (index >= count) {
        return;
    }
    keys[index] = candidate_diverse_key_device(
        candidates[index],
        edges,
        mode,
        target_bit,
        depth
    );
}

__global__ void pack_threshold_fingerprint_high_keys_kernel(
    const DeviceCandidate* candidates,
    const unsigned long long* prefix_keys,
    size_t count,
    unsigned long long threshold,
    unsigned long long* high_keys
) {
    size_t index = static_cast<size_t>(blockIdx.x) * static_cast<size_t>(blockDim.x) +
                   static_cast<size_t>(threadIdx.x);
    if (index >= count) {
        return;
    }
    const DeviceCandidate& candidate = candidates[index];
    if (candidate.valid && prefix_keys[index] == threshold) {
        high_keys[index] = candidate.fingerprint.high;
    } else {
        high_keys[index] = ~0ULL;
    }
}

__global__ void count_prefix_threshold_kernel(
    const unsigned long long* keys,
    size_t count,
    unsigned long long threshold,
    unsigned long long* counts
) {
    size_t index = static_cast<size_t>(blockIdx.x) * static_cast<size_t>(blockDim.x) +
                   static_cast<size_t>(threadIdx.x);
    if (index >= count) {
        return;
    }
    unsigned long long key = keys[index];
    if (key < threshold) {
        atomicAdd(&counts[0], 1ULL);
    } else if (key == threshold) {
        atomicAdd(&counts[1], 1ULL);
    }
}

__global__ void count_refined_threshold_kernel(
    const DeviceCandidate* candidates,
    const unsigned long long* prefix_keys,
    size_t count,
    unsigned long long prefix_threshold,
    unsigned long long high_threshold,
    unsigned long long* counts
) {
    size_t index = static_cast<size_t>(blockIdx.x) * static_cast<size_t>(blockDim.x) +
                   static_cast<size_t>(threadIdx.x);
    if (index >= count) {
        return;
    }
    const DeviceCandidate& candidate = candidates[index];
    if (!candidate.valid) {
        return;
    }
    unsigned long long prefix_key = prefix_keys[index];
    if (prefix_key < prefix_threshold) {
        atomicAdd(&counts[0], 1ULL);
    } else if (prefix_key == prefix_threshold) {
        if (candidate.fingerprint.high < high_threshold) {
            atomicAdd(&counts[1], 1ULL);
        } else if (candidate.fingerprint.high == high_threshold) {
            atomicAdd(&counts[2], 1ULL);
        }
    }
}

__global__ void select_prefix_threshold_kernel(
    const DeviceCandidate* candidates,
    const unsigned long long* keys,
    size_t count,
    unsigned long long threshold,
    DeviceCandidate* selected,
    unsigned long long* selected_count
) {
    size_t index = static_cast<size_t>(blockIdx.x) * static_cast<size_t>(blockDim.x) +
                   static_cast<size_t>(threadIdx.x);
    if (index >= count) {
        return;
    }
    if (keys[index] <= threshold) {
        unsigned long long out_index = atomicAdd(selected_count, 1ULL);
        selected[out_index] = candidates[index];
    }
}

__global__ void select_refined_threshold_kernel(
    const DeviceCandidate* candidates,
    const unsigned long long* prefix_keys,
    size_t count,
    unsigned long long prefix_threshold,
    unsigned long long high_threshold,
    DeviceCandidate* selected,
    unsigned long long* selected_count
) {
    size_t index = static_cast<size_t>(blockIdx.x) * static_cast<size_t>(blockDim.x) +
                   static_cast<size_t>(threadIdx.x);
    if (index >= count) {
        return;
    }
    const DeviceCandidate& candidate = candidates[index];
    if (!candidate.valid) {
        return;
    }
    unsigned long long prefix_key = prefix_keys[index];
    bool keep =
        prefix_key < prefix_threshold ||
        (
            prefix_key == prefix_threshold &&
            candidate.fingerprint.high <= high_threshold
        );
    if (keep) {
        unsigned long long out_index = atomicAdd(selected_count, 1ULL);
        selected[out_index] = candidate;
    }
}

__global__ void insert_recent_fingerprints_kernel(
    const DeviceFingerprint128* fingerprints,
    size_t count,
    DeviceFingerprint128* table,
    unsigned int* used,
    size_t table_mask
) {
    size_t index = static_cast<size_t>(blockIdx.x) * static_cast<size_t>(blockDim.x) +
                   static_cast<size_t>(threadIdx.x);
    if (index >= count || table_mask == 0) {
        return;
    }

    DeviceFingerprint128 fingerprint = fingerprints[index];
    size_t slot = static_cast<size_t>(fingerprint_hash_device(fingerprint)) & table_mask;
    for (size_t probe = 0; probe <= table_mask; ++probe) {
        size_t table_index = (slot + probe) & table_mask;
        unsigned int state = atomicCAS(&used[table_index], 0U, 1U);
        if (state == 0U) {
            table[table_index] = fingerprint;
            __threadfence();
            used[table_index] = 2U;
            return;
        }
        if (state == 2U && fingerprint_equal(table[table_index], fingerprint)) {
            return;
        }
    }
}

void check_cuda(cudaError_t error, const char* what) {
    if (error != cudaSuccess) {
        throw runtime_error(string(what) + ": " + cudaGetErrorString(error));
    }
}

size_t next_power_of_two(size_t value) {
    size_t result = 1;
    while (result < value) {
        result <<= 1;
    }
    return result;
}

vector<DeviceFingerprint128> make_recent_table(
    const vector<Fingerprint128>& recent_fingerprints,
    vector<unsigned int>& used
) {
    if (recent_fingerprints.empty()) {
        used.clear();
        return {};
    }
    size_t table_size = next_power_of_two(max<size_t>(recent_fingerprints.size() * 2, 2));
    vector<DeviceFingerprint128> table(table_size, {0, 0});
    used.assign(table_size, 0);
    size_t mask = table_size - 1;
    for (const Fingerprint128& fingerprint : recent_fingerprints) {
        DeviceFingerprint128 fp{fingerprint.low, fingerprint.high};
        size_t slot = static_cast<size_t>(fingerprint_hash_device(fp)) & mask;
        while (used[slot]) {
            if (table[slot].low == fp.low && table[slot].high == fp.high) {
                break;
            }
            slot = (slot + 1) & mask;
        }
        table[slot] = fp;
        used[slot] = 2;
    }
    return table;
}

size_t choose_candidate_chunk_size(size_t logical_candidates) {
    constexpr size_t min_chunk_candidates = 16ULL * 1024ULL * 1024ULL;
    constexpr size_t max_chunk_candidates = 64ULL * 1024ULL * 1024ULL;
    constexpr size_t reserve_bytes = 2ULL * 1024ULL * 1024ULL * 1024ULL;
    if (const char* override_value = getenv("QK_CUDA_CHUNK_CANDIDATES")) {
        char* end = nullptr;
        unsigned long long parsed = strtoull(override_value, &end, 10);
        if (end != override_value && parsed > 0) {
            return max<size_t>(1, min(logical_candidates, static_cast<size_t>(parsed)));
        }
    }

    size_t free_bytes = 0;
    size_t total_bytes = 0;
    cudaError_t error = cudaMemGetInfo(&free_bytes, &total_bytes);
    if (error != cudaSuccess || free_bytes <= reserve_bytes) {
        return min(logical_candidates, min_chunk_candidates);
    }

    // thrust::sort needs temporary storage too, so leave a conservative multiple
    // of the candidate buffer rather than filling all free VRAM.
    size_t usable_bytes = free_bytes - reserve_bytes;
    size_t by_memory = usable_bytes / max<size_t>(sizeof(DeviceCandidate) * 4ULL, 1);
    size_t chunk = max(min_chunk_candidates, by_memory);
    chunk = min(chunk, max_chunk_candidates);
    return max<size_t>(1, min(logical_candidates, chunk));
}

void append_and_trim_top(
    vector<DeviceCandidate>& merged_top,
    vector<DeviceCandidate>& chunk_top,
    size_t keep_limit
) {
    if (chunk_top.empty()) {
        return;
    }
    merged_top.insert(
        merged_top.end(),
        make_move_iterator(chunk_top.begin()),
        make_move_iterator(chunk_top.end())
    );
    if (merged_top.size() > keep_limit * 2ULL) {
        sort(merged_top.begin(), merged_top.end(), DeviceCandidateLess{});
        if (merged_top.size() > keep_limit) {
            merged_top.resize(keep_limit);
        }
    }
}

template <typename T>
void ensure_device_capacity(T*& pointer, size_t& capacity, size_t needed, const char* label) {
    if (needed == 0 || capacity >= needed) {
        return;
    }
    cudaFree(pointer);
    pointer = nullptr;
    check_cuda(cudaMalloc(&pointer, needed * sizeof(T)), label);
    capacity = needed;
}

template <typename T>
void release_device_buffer(T*& pointer, size_t& capacity) {
    cudaFree(pointer);
    pointer = nullptr;
    capacity = 0;
}

struct CudaLayerContext {
    uint16_t* d_states = nullptr;
    int* d_last_edges = nullptr;
    int* d_path_nodes = nullptr;
    DeviceFingerprint128* d_parent_fingerprints = nullptr;
    int* d_total_dist = nullptr;
    int* d_misplaced = nullptr;
    int* d_histograms = nullptr;
    DeviceEdge* d_edges = nullptr;
    DeviceFingerprint128* d_recent_table = nullptr;
    unsigned int* d_recent_used = nullptr;
    DeviceFingerprint128* d_recent_delta = nullptr;
    DeviceCandidate* d_candidates = nullptr;
    unsigned long long* d_candidate_prefix_keys = nullptr;
    unsigned long long* d_candidate_secondary_keys = nullptr;
    unsigned long long* d_topk_prefix_keys = nullptr;
    DeviceCandidate* d_topk_candidates = nullptr;
    unsigned long long* d_topk_counts = nullptr;
    void* d_topk_temp_storage = nullptr;
    unsigned long long* d_valid_count = nullptr;

    size_t states_capacity = 0;
    size_t parent_capacity = 0;
    size_t histogram_capacity = 0;
    size_t edge_capacity = 0;
    size_t recent_table_capacity = 0;
    size_t recent_used_capacity = 0;
    size_t recent_delta_capacity = 0;
    size_t candidate_capacity = 0;
    size_t candidate_prefix_key_capacity = 0;
    size_t candidate_secondary_key_capacity = 0;
    size_t topk_prefix_key_capacity = 0;
    size_t topk_candidate_capacity = 0;
    size_t topk_count_capacity = 0;
    size_t topk_temp_storage_capacity = 0;
    size_t valid_count_capacity = 0;
    size_t recent_active_slots = 0;

    int cached_node_count = -1;
    size_t cached_edge_count = 0;
    uint64_t recent_version = numeric_limits<uint64_t>::max();
    bool recent_ready = false;

    cudaEvent_t event_start = nullptr;
    cudaEvent_t event_stop = nullptr;

    ~CudaLayerContext() {
        release_device_buffer(d_states, states_capacity);
        release_device_buffer(d_last_edges, parent_capacity);
        // The following buffers share parent_capacity. Reset manually after freeing.
        cudaFree(d_path_nodes);
        cudaFree(d_parent_fingerprints);
        cudaFree(d_total_dist);
        cudaFree(d_misplaced);
        d_path_nodes = nullptr;
        d_parent_fingerprints = nullptr;
        d_total_dist = nullptr;
        d_misplaced = nullptr;
        release_device_buffer(d_histograms, histogram_capacity);
        release_device_buffer(d_edges, edge_capacity);
        release_device_buffer(d_recent_table, recent_table_capacity);
        release_device_buffer(d_recent_used, recent_used_capacity);
        release_device_buffer(d_recent_delta, recent_delta_capacity);
        release_device_buffer(d_candidates, candidate_capacity);
        release_device_buffer(d_candidate_prefix_keys, candidate_prefix_key_capacity);
        release_device_buffer(d_candidate_secondary_keys, candidate_secondary_key_capacity);
        release_device_buffer(d_topk_prefix_keys, topk_prefix_key_capacity);
        release_device_buffer(d_topk_candidates, topk_candidate_capacity);
        release_device_buffer(d_topk_counts, topk_count_capacity);
        cudaFree(d_topk_temp_storage);
        d_topk_temp_storage = nullptr;
        topk_temp_storage_capacity = 0;
        release_device_buffer(d_valid_count, valid_count_capacity);
        if (event_start != nullptr) {
            cudaEventDestroy(event_start);
        }
        if (event_stop != nullptr) {
            cudaEventDestroy(event_stop);
        }
    }

    void ensure_events() {
        if (event_start == nullptr) {
            check_cuda(cudaEventCreate(&event_start), "cudaEventCreate start");
        }
        if (event_stop == nullptr) {
            check_cuda(cudaEventCreate(&event_stop), "cudaEventCreate stop");
        }
    }

    template <typename F>
    double time_gpu(F&& fn, const char* sync_label) {
        ensure_events();
        check_cuda(cudaEventRecord(event_start), "cudaEventRecord start");
        fn();
        check_cuda(cudaEventRecord(event_stop), "cudaEventRecord stop");
        check_cuda(cudaEventSynchronize(event_stop), sync_label);
        float ms = 0.0f;
        check_cuda(cudaEventElapsedTime(&ms, event_start, event_stop), "cudaEventElapsedTime");
        return static_cast<double>(ms) / 1000.0;
    }

    void ensure_parent_buffers(size_t parent_count, int node_count) {
        ensure_device_capacity(d_states, states_capacity, parent_count * static_cast<size_t>(node_count), "cudaMalloc states");
        if (parent_capacity < parent_count) {
            cudaFree(d_last_edges);
            cudaFree(d_path_nodes);
            cudaFree(d_parent_fingerprints);
            cudaFree(d_total_dist);
            cudaFree(d_misplaced);
            d_last_edges = nullptr;
            d_path_nodes = nullptr;
            d_parent_fingerprints = nullptr;
            d_total_dist = nullptr;
            d_misplaced = nullptr;
            check_cuda(cudaMalloc(&d_last_edges, parent_count * sizeof(int)), "cudaMalloc last_edges");
            check_cuda(cudaMalloc(&d_path_nodes, parent_count * sizeof(int)), "cudaMalloc path_nodes");
            check_cuda(cudaMalloc(&d_parent_fingerprints, parent_count * sizeof(DeviceFingerprint128)), "cudaMalloc fingerprints");
            check_cuda(cudaMalloc(&d_total_dist, parent_count * sizeof(int)), "cudaMalloc total_dist");
            check_cuda(cudaMalloc(&d_misplaced, parent_count * sizeof(int)), "cudaMalloc misplaced");
            parent_capacity = parent_count;
        }
        ensure_device_capacity(d_histograms, histogram_capacity, parent_count * 16ULL, "cudaMalloc histograms");
    }

    void ensure_valid_count() {
        ensure_device_capacity(d_valid_count, valid_count_capacity, 1, "cudaMalloc valid_count");
    }

    void ensure_topk_buffers(size_t candidate_count, size_t output_count) {
        ensure_device_capacity(
            d_candidate_prefix_keys,
            candidate_prefix_key_capacity,
            candidate_count,
            "cudaMalloc candidate prefix keys"
        );
        ensure_device_capacity(
            d_topk_prefix_keys,
            topk_prefix_key_capacity,
            output_count,
            "cudaMalloc topk prefix keys"
        );
        ensure_device_capacity(
            d_topk_candidates,
            topk_candidate_capacity,
            output_count,
            "cudaMalloc topk candidates"
        );
        ensure_device_capacity(d_topk_counts, topk_count_capacity, 3, "cudaMalloc topk counts");
    }

    void ensure_secondary_key_buffer(size_t candidate_count) {
        ensure_device_capacity(
            d_candidate_secondary_keys,
            candidate_secondary_key_capacity,
            candidate_count,
            "cudaMalloc candidate secondary keys"
        );
    }

    void ensure_topk_temp_storage(size_t needed) {
        if (needed == 0 || topk_temp_storage_capacity >= needed) {
            return;
        }
        cudaFree(d_topk_temp_storage);
        d_topk_temp_storage = nullptr;
        check_cuda(cudaMalloc(&d_topk_temp_storage, needed), "cudaMalloc topk temp storage");
        topk_temp_storage_capacity = needed;
    }
};

thread_local CudaLayerContext cuda_layer_context;

double seconds_since(Clock::time_point start) {
    return chrono::duration<double>(Clock::now() - start).count();
}

int hypercube_dim_from_node_count(int node_count) {
    if (node_count <= 0) {
        return -1;
    }
    int dim = 0;
    int value = 1;
    while (value < node_count && dim < 30) {
        value <<= 1;
        ++dim;
    }
    return value == node_count ? dim : -1;
}

bool cub_prefix_key_supported(int node_count, size_t edge_count) {
    constexpr unsigned long long total_limit = (1ULL << 22) - 1ULL;
    constexpr unsigned long long max_dist_limit = (1ULL << 5) - 1ULL;
    constexpr unsigned long long misplaced_limit = (1ULL << 17) - 1ULL;
    constexpr unsigned long long edge_limit = (1ULL << 19) - 1ULL;

    int dim = hypercube_dim_from_node_count(node_count);
    if (dim < 0) {
        return false;
    }
    unsigned long long max_total =
        static_cast<unsigned long long>(node_count) * static_cast<unsigned long long>(dim);
    return max_total <= total_limit &&
           static_cast<unsigned long long>(dim) <= max_dist_limit &&
           static_cast<unsigned long long>(node_count) <= misplaced_limit &&
           edge_count > 0 &&
           static_cast<unsigned long long>(edge_count - 1) <= edge_limit;
}

size_t choose_cub_tie_cap(size_t keep_limit) {
    size_t default_cap = max<size_t>(keep_limit * 8ULL, 1ULL << 20);
    if (const char* override_value = getenv("QK_CUDA_TOPK_TIE_CAP")) {
        char* end = nullptr;
        unsigned long long parsed = strtoull(override_value, &end, 10);
        if (end != override_value && parsed > 0) {
            return max<size_t>(keep_limit, static_cast<size_t>(parsed));
        }
    }
    return default_cap;
}

size_t choose_topk_tile_size(size_t current_chunk, size_t keep_limit) {
    if (current_chunk == 0) {
        return 0;
    }
    if (const char* override_value = getenv("QK_CUDA_TOPK_TILE_CANDIDATES")) {
        char* end = nullptr;
        unsigned long long parsed = strtoull(override_value, &end, 10);
        if (end != override_value && parsed > 0) {
            return max<size_t>(1, min(current_chunk, static_cast<size_t>(parsed)));
        }
    }
    size_t tile = max<size_t>(keep_limit * 8ULL, 256ULL * 1024ULL);
    tile = min(tile, 4ULL * 1024ULL * 1024ULL);
    return max<size_t>(1, min(current_chunk, tile));
}

vector<DeviceCandidate> copy_device_top(
    CudaLayerContext& ctx,
    DeviceCandidate* begin_pointer,
    size_t copy_count,
    CudaLayerTiming& timing
) {
    vector<DeviceCandidate> h_top(copy_count);
    if (copy_count > 0) {
        timing.d2h_sec += ctx.time_gpu([&]() {
            check_cuda(cudaMemcpy(
                h_top.data(),
                begin_pointer,
                copy_count * sizeof(DeviceCandidate),
                cudaMemcpyDeviceToHost
            ), "cudaMemcpy top candidates");
        }, "cudaMemcpy top candidates sync");
    }
    return h_top;
}

vector<DeviceCandidate> select_top_full_sort(
    CudaLayerContext& ctx,
    size_t current_chunk,
    size_t keep_limit,
    size_t valid_count,
    CudaLayerTiming& timing
) {
    timing.topk_mode = static_cast<int>(CudaTopKMode::FullSort);
    timing.topk_tiles += 1;
    timing.topk_tile_candidates = max(timing.topk_tile_candidates, current_chunk);
    auto topk_start = Clock::now();
    timing.sort_sec += ctx.time_gpu([&]() {
        thrust::device_ptr<DeviceCandidate> begin(ctx.d_candidates);
        thrust::device_ptr<DeviceCandidate> end(ctx.d_candidates + current_chunk);
        thrust::sort(begin, end, DeviceCandidateLess{});
    }, "thrust sort sync");

    size_t copy_count = min<size_t>(
        keep_limit,
        min<size_t>(valid_count, current_chunk)
    );
    vector<DeviceCandidate> top = copy_device_top(ctx, ctx.d_candidates, copy_count, timing);
    timing.topk_select_sec += seconds_since(topk_start);
    return top;
}

vector<DeviceCandidate> select_top_tiled(
    CudaLayerContext& ctx,
    size_t current_chunk,
    size_t keep_limit,
    CudaLayerTiming& timing
) {
    timing.topk_mode = static_cast<int>(CudaTopKMode::Tiled);
    size_t tile_size = choose_topk_tile_size(current_chunk, keep_limit);
    timing.topk_tile_candidates = max(timing.topk_tile_candidates, tile_size);
    vector<DeviceCandidate> chunk_top;
    chunk_top.reserve(keep_limit);

    auto topk_start = Clock::now();
    for (size_t tile_offset = 0; tile_offset < current_chunk; tile_offset += tile_size) {
        size_t tile_count = min(tile_size, current_chunk - tile_offset);
        timing.topk_tiles += 1;
        timing.sort_sec += ctx.time_gpu([&]() {
            thrust::device_ptr<DeviceCandidate> begin(ctx.d_candidates + tile_offset);
            thrust::device_ptr<DeviceCandidate> end(ctx.d_candidates + tile_offset + tile_count);
            thrust::sort(begin, end, DeviceCandidateLess{});
        }, "thrust tiled sort sync");

        size_t copy_count = min(keep_limit, tile_count);
        vector<DeviceCandidate> tile_top = copy_device_top(
            ctx,
            ctx.d_candidates + tile_offset,
            copy_count,
            timing
        );
        auto merge_start = Clock::now();
        append_and_trim_top(chunk_top, tile_top, keep_limit);
        timing.cpu_merge_sec += seconds_since(merge_start);
    }

    auto merge_start = Clock::now();
    sort(chunk_top.begin(), chunk_top.end(), DeviceCandidateLess{});
    if (chunk_top.size() > keep_limit) {
        chunk_top.resize(keep_limit);
    }
    timing.cpu_merge_sec += seconds_since(merge_start);
    timing.topk_select_sec += seconds_since(topk_start);
    return chunk_top;
}

vector<DeviceCandidate> select_top_cub(
    CudaLayerContext& ctx,
    size_t current_chunk,
    size_t keep_limit,
    size_t valid_count,
    int node_count,
    size_t edge_count,
    CudaLayerTiming& timing
) {
    auto fallback_full_sort = [&]() {
        vector<DeviceCandidate> top = select_top_full_sort(
            ctx,
            current_chunk,
            keep_limit,
            valid_count,
            timing
        );
        timing.topk_mode = static_cast<int>(CudaTopKMode::CubFallbackFullSort);
        return top;
    };

    if (valid_count <= keep_limit || !cub_prefix_key_supported(node_count, edge_count)) {
        return fallback_full_sort();
    }

    timing.topk_mode = static_cast<int>(CudaTopKMode::Cub);
    timing.topk_tiles += 1;
    timing.topk_tile_candidates = max(timing.topk_tile_candidates, current_chunk);

    size_t k = min(keep_limit, current_chunk);
    auto topk_start = Clock::now();
    ctx.ensure_topk_buffers(current_chunk, k);

    constexpr int threads_per_block = 256;
    int blocks = static_cast<int>((current_chunk + threads_per_block - 1) / threads_per_block);
    timing.kernel_sec += ctx.time_gpu([&]() {
        pack_candidate_prefix_keys_kernel<<<blocks, threads_per_block>>>(
            ctx.d_candidates,
            current_chunk,
            ctx.d_candidate_prefix_keys
        );
        check_cuda(cudaGetLastError(), "pack_candidate_prefix_keys_kernel launch");
    }, "pack_candidate_prefix_keys_kernel sync");

    auto env = cuda::execution::require(
        cuda::execution::determinism::not_guaranteed,
        cuda::execution::output_ordering::unsorted
    );

    size_t temp_storage_bytes = 0;
    check_cuda(cub::DeviceTopK::MinPairs(
        nullptr,
        temp_storage_bytes,
        ctx.d_candidate_prefix_keys,
        ctx.d_topk_prefix_keys,
        ctx.d_candidates,
        ctx.d_topk_candidates,
        current_chunk,
        k,
        env
    ), "cub::DeviceTopK::MinPairs temp query");
    ctx.ensure_topk_temp_storage(temp_storage_bytes);

    timing.sort_sec += ctx.time_gpu([&]() {
        size_t available_temp_storage = ctx.topk_temp_storage_capacity;
        check_cuda(cub::DeviceTopK::MinPairs(
            ctx.d_topk_temp_storage,
            available_temp_storage,
            ctx.d_candidate_prefix_keys,
            ctx.d_topk_prefix_keys,
            ctx.d_candidates,
            ctx.d_topk_candidates,
            current_chunk,
            k,
            env
        ), "cub::DeviceTopK::MinPairs");
    }, "cub::DeviceTopK::MinPairs sync");

    vector<unsigned long long> h_topk_keys(k);
    timing.d2h_sec += ctx.time_gpu([&]() {
        check_cuda(cudaMemcpy(
            h_topk_keys.data(),
            ctx.d_topk_prefix_keys,
            k * sizeof(unsigned long long),
            cudaMemcpyDeviceToHost
        ), "cudaMemcpy cub topk keys");
    }, "cudaMemcpy cub topk keys sync");
    unsigned long long threshold = *max_element(h_topk_keys.begin(), h_topk_keys.end());

    unsigned long long zero_counts[3] = {0, 0, 0};
    timing.h2d_sec += ctx.time_gpu([&]() {
        check_cuda(cudaMemcpy(
            ctx.d_topk_counts,
            zero_counts,
            sizeof(zero_counts),
            cudaMemcpyHostToDevice
        ), "cudaMemcpy topk counts zero");
    }, "cudaMemcpy topk counts zero sync");
    timing.kernel_sec += ctx.time_gpu([&]() {
        count_prefix_threshold_kernel<<<blocks, threads_per_block>>>(
            ctx.d_candidate_prefix_keys,
            current_chunk,
            threshold,
            ctx.d_topk_counts
        );
        check_cuda(cudaGetLastError(), "count_prefix_threshold_kernel launch");
    }, "count_prefix_threshold_kernel sync");

    unsigned long long h_counts[3] = {0, 0, 0};
    timing.d2h_sec += ctx.time_gpu([&]() {
        check_cuda(cudaMemcpy(
            h_counts,
            ctx.d_topk_counts,
            sizeof(h_counts),
            cudaMemcpyDeviceToHost
        ), "cudaMemcpy topk counts");
    }, "cudaMemcpy topk counts sync");

    size_t selected_count = static_cast<size_t>(h_counts[0] + h_counts[1]);
    size_t tie_cap = choose_cub_tie_cap(keep_limit);
    if (selected_count == 0) {
        return fallback_full_sort();
    }

    if (selected_count > tie_cap) {
        size_t less_count = static_cast<size_t>(h_counts[0]);
        if (less_count >= keep_limit) {
            return fallback_full_sort();
        }

        size_t remaining_from_tie = keep_limit - less_count;
        ctx.ensure_topk_buffers(current_chunk, max(k, remaining_from_tie));
        ctx.ensure_secondary_key_buffer(current_chunk);
        timing.kernel_sec += ctx.time_gpu([&]() {
            pack_threshold_fingerprint_high_keys_kernel<<<blocks, threads_per_block>>>(
                ctx.d_candidates,
                ctx.d_candidate_prefix_keys,
                current_chunk,
                threshold,
                ctx.d_candidate_secondary_keys
            );
            check_cuda(cudaGetLastError(), "pack_threshold_fingerprint_high_keys_kernel launch");
        }, "pack_threshold_fingerprint_high_keys_kernel sync");

        size_t secondary_temp_storage_bytes = 0;
        check_cuda(cub::DeviceTopK::MinPairs(
            nullptr,
            secondary_temp_storage_bytes,
            ctx.d_candidate_secondary_keys,
            ctx.d_topk_prefix_keys,
            ctx.d_candidates,
            ctx.d_topk_candidates,
            current_chunk,
            remaining_from_tie,
            env
        ), "cub::DeviceTopK::MinPairs secondary temp query");
        ctx.ensure_topk_temp_storage(secondary_temp_storage_bytes);

        timing.topk_tiles += 1;
        timing.sort_sec += ctx.time_gpu([&]() {
            size_t available_temp_storage = ctx.topk_temp_storage_capacity;
            check_cuda(cub::DeviceTopK::MinPairs(
                ctx.d_topk_temp_storage,
                available_temp_storage,
                ctx.d_candidate_secondary_keys,
                ctx.d_topk_prefix_keys,
                ctx.d_candidates,
                ctx.d_topk_candidates,
                current_chunk,
                remaining_from_tie,
                env
            ), "cub::DeviceTopK::MinPairs secondary");
        }, "cub::DeviceTopK::MinPairs secondary sync");

        vector<unsigned long long> h_secondary_keys(remaining_from_tie);
        timing.d2h_sec += ctx.time_gpu([&]() {
            check_cuda(cudaMemcpy(
                h_secondary_keys.data(),
                ctx.d_topk_prefix_keys,
                remaining_from_tie * sizeof(unsigned long long),
                cudaMemcpyDeviceToHost
            ), "cudaMemcpy cub secondary topk keys");
        }, "cudaMemcpy cub secondary topk keys sync");
        unsigned long long high_threshold =
            *max_element(h_secondary_keys.begin(), h_secondary_keys.end());

        timing.h2d_sec += ctx.time_gpu([&]() {
            check_cuda(cudaMemcpy(
                ctx.d_topk_counts,
                zero_counts,
                sizeof(zero_counts),
                cudaMemcpyHostToDevice
            ), "cudaMemcpy refined topk counts zero");
        }, "cudaMemcpy refined topk counts zero sync");
        timing.kernel_sec += ctx.time_gpu([&]() {
            count_refined_threshold_kernel<<<blocks, threads_per_block>>>(
                ctx.d_candidates,
                ctx.d_candidate_prefix_keys,
                current_chunk,
                threshold,
                high_threshold,
                ctx.d_topk_counts
            );
            check_cuda(cudaGetLastError(), "count_refined_threshold_kernel launch");
        }, "count_refined_threshold_kernel sync");

        unsigned long long h_refined_counts[3] = {0, 0, 0};
        timing.d2h_sec += ctx.time_gpu([&]() {
            check_cuda(cudaMemcpy(
                h_refined_counts,
                ctx.d_topk_counts,
                sizeof(h_refined_counts),
                cudaMemcpyDeviceToHost
            ), "cudaMemcpy refined topk counts");
        }, "cudaMemcpy refined topk counts sync");

        selected_count = static_cast<size_t>(
            h_refined_counts[0] + h_refined_counts[1] + h_refined_counts[2]
        );
        if (selected_count == 0 || selected_count > tie_cap) {
            return fallback_full_sort();
        }

        ctx.ensure_topk_buffers(current_chunk, max(k, selected_count));
        timing.h2d_sec += ctx.time_gpu([&]() {
            check_cuda(cudaMemset(
                ctx.d_topk_counts,
                0,
                sizeof(unsigned long long)
            ), "cudaMemset refined selected count");
        }, "cudaMemset refined selected count sync");
        timing.kernel_sec += ctx.time_gpu([&]() {
            select_refined_threshold_kernel<<<blocks, threads_per_block>>>(
                ctx.d_candidates,
                ctx.d_candidate_prefix_keys,
                current_chunk,
                threshold,
                high_threshold,
                ctx.d_topk_candidates,
                ctx.d_topk_counts
            );
            check_cuda(cudaGetLastError(), "select_refined_threshold_kernel launch");
        }, "select_refined_threshold_kernel sync");

        vector<DeviceCandidate> selected(selected_count);
        timing.d2h_sec += ctx.time_gpu([&]() {
            check_cuda(cudaMemcpy(
                selected.data(),
                ctx.d_topk_candidates,
                selected_count * sizeof(DeviceCandidate),
                cudaMemcpyDeviceToHost
            ), "cudaMemcpy cub refined selected candidates");
        }, "cudaMemcpy cub refined selected candidates sync");

        auto merge_start = Clock::now();
        sort(selected.begin(), selected.end(), DeviceCandidateLess{});
        if (selected.size() > keep_limit) {
            selected.resize(keep_limit);
        }
        timing.cpu_merge_sec += seconds_since(merge_start);
        timing.topk_select_sec += seconds_since(topk_start);
        return selected;
    }

    ctx.ensure_topk_buffers(current_chunk, max(k, selected_count));
    timing.h2d_sec += ctx.time_gpu([&]() {
        check_cuda(cudaMemset(
            ctx.d_topk_counts + 2,
            0,
            sizeof(unsigned long long)
        ), "cudaMemset selected count");
    }, "cudaMemset selected count sync");
    timing.kernel_sec += ctx.time_gpu([&]() {
        select_prefix_threshold_kernel<<<blocks, threads_per_block>>>(
            ctx.d_candidates,
            ctx.d_candidate_prefix_keys,
            current_chunk,
            threshold,
            ctx.d_topk_candidates,
            ctx.d_topk_counts + 2
        );
        check_cuda(cudaGetLastError(), "select_prefix_threshold_kernel launch");
    }, "select_prefix_threshold_kernel sync");

    vector<DeviceCandidate> selected(selected_count);
    timing.d2h_sec += ctx.time_gpu([&]() {
        check_cuda(cudaMemcpy(
            selected.data(),
            ctx.d_topk_candidates,
            selected_count * sizeof(DeviceCandidate),
            cudaMemcpyDeviceToHost
        ), "cudaMemcpy cub selected candidates");
    }, "cudaMemcpy cub selected candidates sync");

    auto merge_start = Clock::now();
    sort(selected.begin(), selected.end(), DeviceCandidateLess{});
    if (selected.size() > keep_limit) {
        selected.resize(keep_limit);
    }
    timing.cpu_merge_sec += seconds_since(merge_start);
    timing.topk_select_sec += seconds_since(topk_start);
    return selected;
}

vector<DeviceCandidate> select_top_diverse_key_cub(
    CudaLayerContext& ctx,
    size_t current_chunk,
    size_t keep_limit,
    int depth,
    DeviceTopKKeyMode key_mode,
    int target_bit,
    CudaLayerTiming& timing
) {
    if (keep_limit == 0 || current_chunk == 0) {
        return {};
    }
    timing.topk_mode = static_cast<int>(CudaTopKMode::Cub);
    auto topk_start = Clock::now();
    size_t k = min(keep_limit, current_chunk);
    ctx.ensure_topk_buffers(current_chunk, k);

    constexpr int threads_per_block = 256;
    int blocks = static_cast<int>((current_chunk + threads_per_block - 1) / threads_per_block);
    timing.kernel_sec += ctx.time_gpu([&]() {
        pack_candidate_diverse_keys_kernel<<<blocks, threads_per_block>>>(
            ctx.d_candidates,
            ctx.d_edges,
            current_chunk,
            static_cast<int>(key_mode),
            target_bit,
            depth,
            ctx.d_candidate_prefix_keys
        );
        check_cuda(cudaGetLastError(), "pack_candidate_diverse_keys_kernel launch");
    }, "pack_candidate_diverse_keys_kernel sync");

    auto env = cuda::execution::require(
        cuda::execution::determinism::not_guaranteed,
        cuda::execution::output_ordering::unsorted
    );

    size_t temp_storage_bytes = 0;
    check_cuda(cub::DeviceTopK::MinPairs(
        nullptr,
        temp_storage_bytes,
        ctx.d_candidate_prefix_keys,
        ctx.d_topk_prefix_keys,
        ctx.d_candidates,
        ctx.d_topk_candidates,
        current_chunk,
        k,
        env
    ), "cub::DeviceTopK::MinPairs diverse temp query");
    ctx.ensure_topk_temp_storage(temp_storage_bytes);

    timing.topk_tiles += 1;
    timing.sort_sec += ctx.time_gpu([&]() {
        size_t available_temp_storage = ctx.topk_temp_storage_capacity;
        check_cuda(cub::DeviceTopK::MinPairs(
            ctx.d_topk_temp_storage,
            available_temp_storage,
            ctx.d_candidate_prefix_keys,
            ctx.d_topk_prefix_keys,
            ctx.d_candidates,
            ctx.d_topk_candidates,
            current_chunk,
            k,
            env
        ), "cub::DeviceTopK::MinPairs diverse");
    }, "cub::DeviceTopK::MinPairs diverse sync");

    vector<DeviceCandidate> selected = copy_device_top(
        ctx,
        ctx.d_topk_candidates,
        k,
        timing
    );
    timing.topk_select_sec += seconds_since(topk_start);
    return selected;
}

void append_candidate_pool(
    vector<DeviceCandidate>& merged,
    vector<DeviceCandidate>&& candidates
) {
    merged.insert(
        merged.end(),
        make_move_iterator(candidates.begin()),
        make_move_iterator(candidates.end())
    );
}

size_t oversampled_slots(int slots, size_t keep_limit, size_t minimum) {
    if (slots <= 0 || keep_limit == 0) {
        return 0;
    }
    size_t wanted = max<size_t>(static_cast<size_t>(slots) * 8ULL, minimum);
    return min(keep_limit, wanted);
}

vector<DeviceCandidate> select_diverse_gpu_pool(
    CudaLayerContext& ctx,
    size_t current_chunk,
    size_t keep_limit,
    int retained_limit,
    double perturbation_ratio,
    int depth,
    int dim,
    CudaLayerTiming& timing
) {
    if (retained_limit <= 0 || keep_limit == 0) {
        return {};
    }

    int perturb_slots = 0;
    if (perturbation_ratio > 0.0) {
        perturb_slots = static_cast<int>(ceil(static_cast<double>(retained_limit) * perturbation_ratio));
        perturb_slots = min(max(perturb_slots, 1), retained_limit);
    }
    int greedy_slots = retained_limit - perturb_slots;
    int max_dist_slots = greedy_slots * 20 / 100;
    int misplaced_slots = greedy_slots * 15 / 100;
    int plain_greedy_slots = greedy_slots * 50 / 100;
    if (plain_greedy_slots == 0 && greedy_slots > 0) {
        plain_greedy_slots = 1;
    }
    while (plain_greedy_slots + max_dist_slots + misplaced_slots > greedy_slots) {
        if (misplaced_slots > 0) {
            misplaced_slots--;
        } else if (max_dist_slots > 0) {
            max_dist_slots--;
        } else {
            plain_greedy_slots--;
        }
    }
    int edge_slots = greedy_slots - plain_greedy_slots - max_dist_slots - misplaced_slots;

    vector<DeviceCandidate> merged;
    merged.reserve(min<size_t>(keep_limit, static_cast<size_t>(retained_limit) * 8ULL));

    size_t greedy_keep = min(
        keep_limit,
        max<size_t>(
            static_cast<size_t>(retained_limit) * 2ULL,
            oversampled_slots(plain_greedy_slots, keep_limit, 512)
        )
    );
    append_candidate_pool(
        merged,
        select_top_diverse_key_cub(
            ctx,
            current_chunk,
            greedy_keep,
            depth,
            DeviceTopKKeyMode::Greedy,
            -1,
            timing
        )
    );

    append_candidate_pool(
        merged,
        select_top_diverse_key_cub(
            ctx,
            current_chunk,
            oversampled_slots(max_dist_slots, keep_limit, 512),
            depth,
            DeviceTopKKeyMode::MaxDistance,
            -1,
            timing
        )
    );

    append_candidate_pool(
        merged,
        select_top_diverse_key_cub(
            ctx,
            current_chunk,
            oversampled_slots(misplaced_slots, keep_limit, 512),
            depth,
            DeviceTopKKeyMode::Misplaced,
            -1,
            timing
        )
    );

    if (edge_slots > 0 && dim > 0) {
        int per_bit_slots = max(1, (edge_slots + dim - 1) / dim);
        size_t per_bit_keep = oversampled_slots(per_bit_slots, keep_limit, 64);
        for (int bit = 0; bit < dim; ++bit) {
            append_candidate_pool(
                merged,
                select_top_diverse_key_cub(
                    ctx,
                    current_chunk,
                    per_bit_keep,
                    depth,
                    DeviceTopKKeyMode::EdgeBit,
                    bit,
                    timing
                )
            );
        }
    }

    append_candidate_pool(
        merged,
        select_top_diverse_key_cub(
            ctx,
            current_chunk,
            oversampled_slots(perturb_slots, keep_limit, 512),
            depth,
            DeviceTopKKeyMode::Perturb,
            -1,
            timing
        )
    );

    return merged;
}

bool ensure_candidate_capacity(CudaLayerContext& ctx, size_t& chunk_candidates) {
    if (chunk_candidates == 0) {
        return false;
    }
    if (ctx.candidate_capacity >= chunk_candidates) {
        return true;
    }
    cudaFree(ctx.d_candidates);
    ctx.d_candidates = nullptr;
    ctx.candidate_capacity = 0;
    while (chunk_candidates > 0) {
        cudaError_t candidate_alloc = cudaMalloc(
            &ctx.d_candidates,
            chunk_candidates * sizeof(DeviceCandidate)
        );
        if (candidate_alloc == cudaSuccess) {
            ctx.candidate_capacity = chunk_candidates;
            return true;
        }
        cudaGetLastError();
        chunk_candidates /= 2;
    }
    return false;
}

void reset_recent_cache(CudaLayerContext& ctx) {
    ctx.recent_version = numeric_limits<uint64_t>::max();
    ctx.recent_ready = false;
    ctx.recent_active_slots = 0;
}

} // namespace

bool cuda_candidate_backend_available() {
    int count = 0;
    cudaError_t error = cudaGetDeviceCount(&count);
    return error == cudaSuccess && count > 0;
}

string cuda_candidate_backend_unavailable_reason() {
    int count = 0;
    cudaError_t error = cudaGetDeviceCount(&count);
    if (error != cudaSuccess) {
        return string("CUDA runtime is unavailable: ") + cudaGetErrorString(error);
    }
    if (count <= 0) {
        return "CUDA runtime found no devices.";
    }
    return {};
}

CudaLayerGenerationResult generate_layer_only_candidates_cuda(
    const vector<CudaLayerParent>& parents,
    const vector<Edge>& edges,
    int node_count,
    int depth,
    int candidate_keep_limit,
    long long candidate_order_base,
    const vector<Fingerprint128>& recent_fingerprints,
    size_t recent_fingerprint_count,
    uint64_t recent_version,
    const vector<Fingerprint128>& recent_delta_fingerprints,
    bool recent_delta_has_eviction,
    bool force_recent_rebuild,
    bool,
    CudaTopKMode topk_mode,
    int retained_limit,
    double perturbation_ratio,
    BeamSelectionPolicy selection_policy
) {
    if (parents.empty() || edges.empty() || candidate_keep_limit <= 0) {
        return {true, false, 0, {}, {}};
    }
    if (!cuda_candidate_backend_available()) {
        return {false, false, 0, {}, cuda_candidate_backend_unavailable_reason()};
    }
    if (node_count > numeric_limits<uint16_t>::max()) {
        return {
            false,
            false,
            0,
            {},
            "CUDA candidate backend currently supports node ids up to uint16_t."
        };
    }

    auto total_start = Clock::now();
    CudaLayerTiming timing;
    size_t parent_count = parents.size();
    size_t edge_count = edges.size();
    size_t logical_candidates = parent_count * edge_count;
    size_t chunk_candidates = choose_candidate_chunk_size(logical_candidates);
    size_t keep_limit = static_cast<size_t>(candidate_keep_limit);
    timing.parent_count = parent_count;
    timing.edge_count = edge_count;
    timing.logical_candidates = logical_candidates;
    timing.recent_fingerprints = recent_fingerprint_count;

    auto prepare_start = Clock::now();
    vector<uint16_t> h_states(parent_count * static_cast<size_t>(node_count));
    vector<int> h_last_edges(parent_count);
    vector<int> h_path_nodes(parent_count);
    vector<DeviceFingerprint128> h_parent_fingerprints(parent_count);
    vector<int> h_total_dist(parent_count);
    vector<int> h_misplaced(parent_count);
    vector<int> h_histograms(parent_count * 16);
    for (size_t i = 0; i < parent_count; ++i) {
        const CudaLayerParent& parent = parents[i];
        for (int node = 0; node < node_count; ++node) {
            h_states[i * static_cast<size_t>(node_count) + static_cast<size_t>(node)] =
                static_cast<uint16_t>(parent.state[static_cast<size_t>(node)]);
        }
        h_last_edges[i] = parent.last_edge;
        h_path_nodes[i] = parent.path_node;
        h_parent_fingerprints[i] = {parent.fingerprint.low, parent.fingerprint.high};
        h_total_dist[i] = parent.total_dist;
        h_misplaced[i] = parent.misplaced;
        for (int d = 0; d < 16; ++d) {
            h_histograms[i * 16 + static_cast<size_t>(d)] = parent.distance_hist[static_cast<size_t>(d)];
        }
    }

    CudaLayerContext& ctx = cuda_layer_context;
    bool upload_edges =
        ctx.cached_node_count != node_count ||
        ctx.cached_edge_count != edge_count;
    vector<DeviceEdge> h_edges;
    if (upload_edges) {
        h_edges.resize(edges.size());
        for (size_t i = 0; i < edges.size(); ++i) {
            h_edges[i] = {edges[i].u, edges[i].v, edges[i].bit};
        }
    }

    size_t recent_slots = recent_fingerprint_count == 0
        ? 0
        : next_power_of_two(max<size_t>(recent_fingerprint_count * 2, 2));
    bool recent_version_changed = ctx.recent_version != recent_version;
    bool rebuild_recent =
        recent_version_changed &&
        recent_fingerprint_count > 0 &&
        (force_recent_rebuild ||
         !ctx.recent_ready ||
         recent_delta_has_eviction ||
         recent_slots > ctx.recent_active_slots ||
         recent_delta_fingerprints.empty());
    bool insert_recent_delta =
        recent_version_changed &&
        recent_fingerprint_count > 0 &&
        !rebuild_recent &&
        !recent_delta_fingerprints.empty();

    vector<unsigned int> h_recent_used;
    vector<DeviceFingerprint128> h_recent_table;
    if (rebuild_recent) {
        if (recent_fingerprints.size() != recent_fingerprint_count) {
            throw runtime_error("CUDA recent fingerprint rebuild requires a complete host snapshot");
        }
        h_recent_table = make_recent_table(recent_fingerprints, h_recent_used);
        recent_slots = h_recent_table.size();
    }
    vector<DeviceFingerprint128> h_recent_delta;
    if (insert_recent_delta) {
        h_recent_delta.reserve(recent_delta_fingerprints.size());
        for (const Fingerprint128& fingerprint : recent_delta_fingerprints) {
            h_recent_delta.push_back({fingerprint.low, fingerprint.high});
        }
    }
    timing.host_prepare_sec = seconds_since(prepare_start);

    auto alloc_start = Clock::now();
    ctx.ensure_parent_buffers(parent_count, node_count);
    if (upload_edges) {
        ensure_device_capacity(ctx.d_edges, ctx.edge_capacity, edge_count, "cudaMalloc edges");
    }
    if (rebuild_recent) {
        ensure_device_capacity(ctx.d_recent_table, ctx.recent_table_capacity, recent_slots, "cudaMalloc recent_table");
        ensure_device_capacity(ctx.d_recent_used, ctx.recent_used_capacity, recent_slots, "cudaMalloc recent_used");
    }
    if (insert_recent_delta) {
        ensure_device_capacity(ctx.d_recent_delta, ctx.recent_delta_capacity, h_recent_delta.size(), "cudaMalloc recent_delta");
    }
    if (!ensure_candidate_capacity(ctx, chunk_candidates)) {
        throw runtime_error("cudaMalloc candidates: unable to allocate candidate chunk buffer");
    }
    ctx.ensure_valid_count();
    timing.device_alloc_sec = seconds_since(alloc_start);
    timing.chunk_candidates = chunk_candidates;

    timing.h2d_sec += ctx.time_gpu([&]() {
        check_cuda(cudaMemcpy(ctx.d_states, h_states.data(), h_states.size() * sizeof(uint16_t), cudaMemcpyHostToDevice), "cudaMemcpy states");
        check_cuda(cudaMemcpy(ctx.d_last_edges, h_last_edges.data(), h_last_edges.size() * sizeof(int), cudaMemcpyHostToDevice), "cudaMemcpy last_edges");
        check_cuda(cudaMemcpy(ctx.d_path_nodes, h_path_nodes.data(), h_path_nodes.size() * sizeof(int), cudaMemcpyHostToDevice), "cudaMemcpy path_nodes");
        check_cuda(cudaMemcpy(ctx.d_parent_fingerprints, h_parent_fingerprints.data(), h_parent_fingerprints.size() * sizeof(DeviceFingerprint128), cudaMemcpyHostToDevice), "cudaMemcpy fingerprints");
        check_cuda(cudaMemcpy(ctx.d_total_dist, h_total_dist.data(), h_total_dist.size() * sizeof(int), cudaMemcpyHostToDevice), "cudaMemcpy total_dist");
        check_cuda(cudaMemcpy(ctx.d_misplaced, h_misplaced.data(), h_misplaced.size() * sizeof(int), cudaMemcpyHostToDevice), "cudaMemcpy misplaced");
        check_cuda(cudaMemcpy(ctx.d_histograms, h_histograms.data(), h_histograms.size() * sizeof(int), cudaMemcpyHostToDevice), "cudaMemcpy histograms");
        if (upload_edges) {
            check_cuda(cudaMemcpy(ctx.d_edges, h_edges.data(), h_edges.size() * sizeof(DeviceEdge), cudaMemcpyHostToDevice), "cudaMemcpy edges");
        }
        if (rebuild_recent && recent_slots > 0) {
            check_cuda(cudaMemcpy(ctx.d_recent_table, h_recent_table.data(), h_recent_table.size() * sizeof(DeviceFingerprint128), cudaMemcpyHostToDevice), "cudaMemcpy recent_table");
            check_cuda(cudaMemcpy(ctx.d_recent_used, h_recent_used.data(), h_recent_used.size() * sizeof(unsigned int), cudaMemcpyHostToDevice), "cudaMemcpy recent_used");
        }
        if (insert_recent_delta && !h_recent_delta.empty()) {
            check_cuda(cudaMemcpy(ctx.d_recent_delta, h_recent_delta.data(), h_recent_delta.size() * sizeof(DeviceFingerprint128), cudaMemcpyHostToDevice), "cudaMemcpy recent_delta");
        }
    }, "cudaMemcpy H2D sync");

    if (upload_edges) {
        ctx.cached_node_count = node_count;
        ctx.cached_edge_count = edge_count;
    }

    if (recent_fingerprint_count == 0) {
        reset_recent_cache(ctx);
        ctx.recent_version = recent_version;
        ctx.recent_ready = true;
    } else if (rebuild_recent) {
        ctx.recent_active_slots = recent_slots;
        ctx.recent_version = recent_version;
        ctx.recent_ready = true;
    } else if (insert_recent_delta) {
        int threads = 256;
        int blocks = static_cast<int>((h_recent_delta.size() + static_cast<size_t>(threads) - 1) / static_cast<size_t>(threads));
        timing.recent_update_sec += ctx.time_gpu([&]() {
            insert_recent_fingerprints_kernel<<<blocks, threads>>>(
                ctx.d_recent_delta,
                h_recent_delta.size(),
                ctx.d_recent_table,
                ctx.d_recent_used,
                ctx.recent_active_slots - 1
            );
            check_cuda(cudaGetLastError(), "insert_recent_fingerprints_kernel launch");
        }, "insert_recent_fingerprints_kernel sync");
        ctx.recent_version = recent_version;
        ctx.recent_ready = true;
    } else if (!recent_version_changed && ctx.recent_ready) {
        // Reuse the resident table unchanged.
    } else if (recent_fingerprint_count > 0) {
        reset_recent_cache(ctx);
        throw runtime_error("CUDA recent fingerprint cache is not initialized");
    }
    timing.recent_table_slots = ctx.recent_active_slots;

    int threads = 256;
    size_t recent_mask = ctx.recent_active_slots == 0 ? 0 : (ctx.recent_active_slots - 1);

    unsigned long long total_valid_count = 0;
    vector<DeviceCandidate> merged_top;
    merged_top.reserve(keep_limit);

    for (size_t offset = 0; offset < logical_candidates; offset += chunk_candidates) {
        size_t current_chunk = min(chunk_candidates, logical_candidates - offset);
        timing.chunks++;

        unsigned long long zero = 0;
        timing.h2d_sec += ctx.time_gpu([&]() {
            check_cuda(cudaMemcpy(ctx.d_valid_count, &zero, sizeof(zero), cudaMemcpyHostToDevice), "cudaMemcpy valid_count");
        }, "cudaMemcpy valid_count sync");

        int blocks = static_cast<int>((current_chunk + static_cast<size_t>(threads) - 1) / static_cast<size_t>(threads));
        timing.kernel_sec += ctx.time_gpu([&]() {
            generate_candidates_kernel<<<blocks, threads>>>(
                ctx.d_states,
                ctx.d_last_edges,
                ctx.d_path_nodes,
                ctx.d_parent_fingerprints,
                ctx.d_total_dist,
                ctx.d_misplaced,
                ctx.d_histograms,
                ctx.d_edges,
                static_cast<int>(parent_count),
                static_cast<int>(edge_count),
                node_count,
                candidate_order_base,
                ctx.d_recent_table,
                ctx.d_recent_used,
                recent_mask,
                offset,
                current_chunk,
                ctx.d_candidates,
                ctx.d_valid_count
            );
            check_cuda(cudaGetLastError(), "generate_candidates_kernel launch");
        }, "generate_candidates_kernel sync");

        unsigned long long valid_count = 0;
        timing.d2h_sec += ctx.time_gpu([&]() {
            check_cuda(cudaMemcpy(&valid_count, ctx.d_valid_count, sizeof(valid_count), cudaMemcpyDeviceToHost), "cudaMemcpy valid_count back");
        }, "cudaMemcpy valid_count back sync");
        total_valid_count += valid_count;

        vector<DeviceCandidate> h_top;
        bool use_diverse_gpu_pool =
            topk_mode == CudaTopKMode::Cub &&
            selection_policy == BeamSelectionPolicy::Diverse &&
            retained_limit > 0 &&
            keep_limit > static_cast<size_t>(retained_limit);
        if (use_diverse_gpu_pool) {
            h_top = select_diverse_gpu_pool(
                ctx,
                current_chunk,
                keep_limit,
                retained_limit,
                perturbation_ratio,
                depth,
                hypercube_dim_from_node_count(node_count),
                timing
            );
        } else if (topk_mode == CudaTopKMode::Tiled) {
            h_top = select_top_tiled(ctx, current_chunk, keep_limit, timing);
        } else if (topk_mode == CudaTopKMode::Cub) {
            h_top = select_top_cub(
                ctx,
                current_chunk,
                keep_limit,
                static_cast<size_t>(valid_count),
                node_count,
                edge_count,
                timing
            );
        } else {
            h_top = select_top_full_sort(
                ctx,
                current_chunk,
                keep_limit,
                static_cast<size_t>(valid_count),
                timing
            );
        }
        auto merge_start = Clock::now();
        append_and_trim_top(merged_top, h_top, keep_limit);
        timing.cpu_merge_sec += seconds_since(merge_start);
    }

    auto merge_start = Clock::now();
    sort(merged_top.begin(), merged_top.end(), DeviceCandidateLess{});
    if (merged_top.size() > keep_limit) {
        merged_top.resize(keep_limit);
    }

    CudaLayerGenerationResult result;
    result.used_cuda = true;
    result.layer_candidates = static_cast<long long>(total_valid_count);
    result.candidates.reserve(merged_top.size());
    for (const DeviceCandidate& candidate : merged_top) {
        if (!candidate.valid) {
            continue;
        }
        CudaLayerCandidate host_candidate;
        host_candidate.total_dist = candidate.total_dist;
        host_candidate.misplaced = candidate.misplaced;
        host_candidate.max_dist = candidate.max_dist;
        host_candidate.parent_index = candidate.parent_index;
        host_candidate.edge_id = candidate.edge_id;
        host_candidate.parent_path_node = candidate.parent_path_node;
        host_candidate.fingerprint = {candidate.fingerprint.low, candidate.fingerprint.high};
        host_candidate.order = candidate.order;
        if (host_candidate.total_dist == 0) {
            result.solved = true;
        }
        result.candidates.push_back(host_candidate);
    }
    timing.cpu_merge_sec += seconds_since(merge_start);
    timing.valid_candidates = static_cast<long long>(total_valid_count);
    timing.total_sec = seconds_since(total_start);
    result.timing = timing;
    return result;
}

} // namespace qk
