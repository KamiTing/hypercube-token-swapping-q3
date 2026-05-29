#pragma once

#include <cstdint>

struct BatcherResult {
    int compare_count;
    int swap_count;
    int round_count;
    bool success;
};

BatcherResult batcher_merge_sort_baseline(uint64_t initial_state);
