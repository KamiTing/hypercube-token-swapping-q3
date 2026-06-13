#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace qk {

using Clock = std::chrono::steady_clock;
using State = std::vector<int>;

struct Edge {
    int u;
    int v;
    int bit;
};

struct SwapStep {
    int u;
    int v;
};

struct SpecialCase {
    std::string name;
    std::string note;
    State state;
};

struct PathResult {
    int steps = -1;
    long long expanded = 0;
    std::size_t reached_states = 0;
    double sec = 0.0;
    std::string status = "skipped";
    std::vector<SwapStep> swaps;
};

struct BatcherResult {
    bool success = false;
    int swaps = 0;
    int compares = 0;
    int rounds = 0;
    double sec = 0.0;
    std::vector<SwapStep> path;
};

enum class CandidateTraceMode {
    None = 0,
    RetainedOnly = 1,
    AllNewCandidates = 2
};

enum class BeamVisitedMode {
    ExactPacked = 0,
    Fingerprint128 = 1,
    Fingerprint128Disk = 2,
    LayerOnly = 3
};

enum class BeamCandidateBackend {
    Cpu = 0,
    Cuda = 1,
    Auto = 2
};

enum class BeamSelectionPolicy {
    Greedy = 0,
    Diverse = 1
};

enum class CudaTopKMode {
    FullSort = 0,
    Tiled = 1,
    Cub = 2,
    CubFallbackFullSort = 3
};

struct ProgressContext {
    bool enabled = false;
    int case_index = 0;
    int total_cases = 0;
    int dim = 0;
    std::string case_name;
};

struct PackedStateKey {
    std::array<uint64_t, 32> words{};

    bool operator==(const PackedStateKey& other) const;
};

struct PackedStateKeyHash {
    std::size_t operator()(const PackedStateKey& key) const;
};

struct Fingerprint128 {
    uint64_t low = 0;
    uint64_t high = 0;

    bool operator==(const Fingerprint128& other) const;
};

struct Fingerprint128Hash {
    std::size_t operator()(const Fingerprint128& key) const;
};

static_assert(sizeof(Fingerprint128) == 16, "Fingerprint128 must be exactly 16 bytes");

uint64_t splitmix64(uint64_t value);
bool fingerprint_less(const Fingerprint128& a, const Fingerprint128& b);
bool packed_key_less(const PackedStateKey& a, const PackedStateKey& b);

int node_count(int dim);
State target_state(int n);
std::string key_from_state(const State& s);
PackedStateKey packed_key_from_state(const State& s);
Fingerprint128 fingerprint_component(int position, int token);
Fingerprint128 fingerprint_from_state(const State& s);
Fingerprint128 fingerprint_after_swap(
    Fingerprint128 fingerprint,
    int u,
    int v,
    int token_u,
    int token_v
);

} // namespace qk
