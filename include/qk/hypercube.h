#pragma once

#include "qk/common.h"

#include <vector>

namespace qk {

std::vector<Edge> edges(int dim);
int hdist(int a, int b);
int total_distance(const State& s);
int misplaced_count(const State& s);
int max_packet_distance(const State& s);
int cycle_lower_bound(const State& s);
int permutation_parity(const State& s);
int basic_lower_bound(const State& s);
int parity_adjust(const State& s, int lower_bound);
int strong_lower_bound(const State& s);
bool path_reaches_goal(State s, const std::vector<SwapStep>& path, int dim);

} // namespace qk
