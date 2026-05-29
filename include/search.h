#pragma once

#include <cstdint>
#include <vector>

#include "config.h"
#include "hypercube.h"

int bfs_min_steps(uint64_t initial_state, const std::vector<Edge>& edges);
int astar_heuristic(uint64_t state);
int astar_min_steps(uint64_t initial_state, const std::vector<Edge>& edges);
int beam_search_steps(
    uint64_t initial_state,
    const std::vector<Edge>& edges,
    int beam_width = cfg::BEAM_WIDTH,
    int max_depth = cfg::MAX_DEPTH
);
