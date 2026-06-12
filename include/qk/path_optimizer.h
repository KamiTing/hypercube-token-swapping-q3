#pragma once

#include "qk/common.h"

#include <cstddef>
#include <string>
#include <vector>

namespace qk {

struct PathOptimizerOptions {
    bool enabled = false;
    int max_window = 0;
    int min_window = 2;
    int window_stride = 0;
    int passes = 1;
    std::size_t node_cap = 200000;
    double segment_time_sec = 0.25;
    int worker_threads = 1;
    bool word_reduction = true;
};

struct PathOptimizerResult {
    std::string status = "skipped";
    int original_steps = -1;
    int optimized_steps = -1;
    int improvement = 0;
    int word_reductions = 0;
    int attempts = 0;
    int improved_segments = 0;
    long long expanded = 0;
    std::size_t reached_states = 0;
    double sec = 0.0;
    std::vector<SwapStep> swaps;
};

PathOptimizerResult optimize_path_shortcuts(
    const State& init,
    const std::vector<Edge>& es,
    const std::vector<SwapStep>& original_path,
    const PathOptimizerOptions& options
);

} // namespace qk
