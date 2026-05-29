#pragma once

namespace cfg {

constexpr int DIM = 3;
constexpr int NODE_COUNT = 1 << DIM;
constexpr int EDGE_COUNT = DIM * (1 << (DIM - 1));
constexpr int MAX_STEP_BUCKET = 64;

constexpr bool RUN_BFS = true;
constexpr bool RUN_ASTAR = true;
constexpr bool RUN_BEAM = true;
constexpr bool RUN_BATCHER = true;

constexpr int BEAM_WIDTH = 14;
constexpr int MAX_DEPTH = 12;

} // namespace cfg
