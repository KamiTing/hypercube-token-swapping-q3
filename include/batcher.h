#pragma once

#include <cstdint>
#include <vector>

#include "search.h"

struct BatcherResult {
    int compare_count;
    int swap_count;
    int round_count;
    bool success;
};

struct BatcherPathResult {
    int compare_count = 0;
    int swap_count = 0;
    int round_count = 0;
    bool success = false;
    std::vector<SwapStep> swaps;
};

BatcherResult batcher_merge_sort_baseline(uint64_t initial_state);
BatcherPathResult batcher_merge_sort_path(uint64_t initial_state);
