#pragma once

#include "qk/common.h"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace qk {

struct CudaLayerParent {
    State state;
    int last_edge = -1;
    int path_node = -1;
    Fingerprint128 fingerprint;
    int total_dist = 0;
    int misplaced = 0;
    std::array<int, 16> distance_hist{};
};

struct CudaLayerCandidate {
    int total_dist = 0;
    int misplaced = 0;
    int max_dist = 0;
    int parent_index = -1;
    int edge_id = -1;
    int parent_path_node = -1;
    Fingerprint128 fingerprint;
    long long order = 0;
};

struct CudaLayerTiming {
    int chunks = 0;
    std::size_t parent_count = 0;
    std::size_t edge_count = 0;
    std::size_t logical_candidates = 0;
    std::size_t chunk_candidates = 0;
    std::size_t recent_fingerprints = 0;
    std::size_t recent_table_slots = 0;
    std::size_t topk_tile_candidates = 0;
    int topk_mode = 0;
    int topk_tiles = 0;
    long long valid_candidates = 0;
    double host_prepare_sec = 0.0;
    double device_alloc_sec = 0.0;
    double h2d_sec = 0.0;
    double recent_update_sec = 0.0;
    double kernel_sec = 0.0;
    double sort_sec = 0.0;
    double d2h_sec = 0.0;
    double cpu_merge_sec = 0.0;
    double topk_select_sec = 0.0;
    double retained_select_sec = 0.0;
    double materialize_sec = 0.0;
    double total_sec = 0.0;
    double outer_parent_prepare_sec = 0.0;
    double outer_recent_snapshot_sec = 0.0;
    double outer_cuda_call_sec = 0.0;
    double outer_candidate_convert_sec = 0.0;
    double outer_progress_write_sec = 0.0;
    double outer_recent_add_sec = 0.0;
    double outer_layer_sec = 0.0;
};

struct CudaLayerGenerationResult {
    bool used_cuda = false;
    bool solved = false;
    long long layer_candidates = 0;
    std::vector<CudaLayerCandidate> candidates;
    std::string fallback_reason;
    CudaLayerTiming timing;
};

bool cuda_candidate_backend_available();
std::string cuda_candidate_backend_unavailable_reason();

CudaLayerGenerationResult generate_layer_only_candidates_cuda(
    const std::vector<CudaLayerParent>& parents,
    const std::vector<Edge>& edges,
    int node_count,
    int depth,
    int candidate_keep_limit,
    long long candidate_order_base,
    const std::vector<Fingerprint128>& recent_fingerprints,
    std::size_t recent_fingerprint_count,
    std::uint64_t recent_version,
    const std::vector<Fingerprint128>& recent_delta_fingerprints,
    bool recent_delta_has_eviction,
    bool force_recent_rebuild,
    bool require_packed_tie_key,
    CudaTopKMode topk_mode,
    int retained_limit,
    double perturbation_ratio,
    BeamSelectionPolicy selection_policy
);

} // namespace qk
