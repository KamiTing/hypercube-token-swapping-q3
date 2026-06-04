#pragma once

#include <cstdint>
#include <vector>

#include "config.h"
#include "hypercube.h"

struct SwapStep {
    int u;
    int v;
};

struct PathResult {
    int steps = -1;
    std::vector<SwapStep> swaps;
};

int bfs_min_steps(uint64_t initial_state, const std::vector<Edge>& edges);
PathResult bfs_shortest_path(uint64_t initial_state, const std::vector<Edge>& edges);
int astar_heuristic(uint64_t state);
int astar_min_steps(uint64_t initial_state, const std::vector<Edge>& edges);
PathResult astar_shortest_path(uint64_t initial_state, const std::vector<Edge>& edges);
int strong_astar_heuristic(uint64_t state);
int strong_astar_min_steps(uint64_t initial_state, const std::vector<Edge>& edges);
PathResult strong_astar_shortest_path(uint64_t initial_state, const std::vector<Edge>& edges);
int beam_search_steps(
    uint64_t initial_state,
    const std::vector<Edge>& edges,
    int beam_width = cfg::BEAM_WIDTH,
    int max_depth = cfg::MAX_DEPTH
);
PathResult beam_search_path(
    uint64_t initial_state,
    const std::vector<Edge>& edges,
    int beam_width = cfg::BEAM_WIDTH,
    int max_depth = cfg::MAX_DEPTH
);
