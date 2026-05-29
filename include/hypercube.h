#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "config.h"

struct Edge {
    int u;
    int v;
    int bit;
};

uint64_t encode_state(const std::array<int, cfg::NODE_COUNT>& a);
int get_packet(uint64_t state, int node);
uint64_t set_packet(uint64_t state, int node, int packet);
uint64_t swap_nodes(uint64_t state, int u, int v);
uint64_t target_state();
bool is_solved(uint64_t state);

std::vector<Edge> generate_hypercube_edges();
bool is_hypercube_edge(int u, int v);

const std::array<std::array<int, cfg::NODE_COUNT>, cfg::NODE_COUNT>& hdist_table();
int total_distance(uint64_t state);
int misplaced_count(uint64_t state);
int max_packet_distance(uint64_t state);
std::array<int, cfg::DIM> global_direction_preference(uint64_t state);

std::vector<uint64_t> generate_all_states();
