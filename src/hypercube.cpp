#include "hypercube.h"

#include <algorithm>
#include <numeric>

using namespace std;

uint64_t encode_state(const array<int, cfg::NODE_COUNT>& a) {
    uint64_t s = 0;
    for (int i = 0; i < cfg::NODE_COUNT; ++i) {
        s |= (static_cast<uint64_t>(a[i]) << (4 * i));
    }
    return s;
}

int get_packet(uint64_t state, int node) {
    return static_cast<int>((state >> (4 * node)) & 0xFULL);
}

uint64_t set_packet(uint64_t state, int node, int packet) {
    uint64_t mask = 0xFULL << (4 * node);
    state &= ~mask;
    state |= (static_cast<uint64_t>(packet) << (4 * node));
    return state;
}

uint64_t swap_nodes(uint64_t state, int u, int v) {
    int pu = get_packet(state, u);
    int pv = get_packet(state, v);
    state = set_packet(state, u, pv);
    state = set_packet(state, v, pu);
    return state;
}

uint64_t target_state() {
    array<int, cfg::NODE_COUNT> a{};
    for (int i = 0; i < cfg::NODE_COUNT; ++i) {
        a[i] = i;
    }
    return encode_state(a);
}

bool is_solved(uint64_t state) {
    return state == target_state();
}

vector<Edge> generate_hypercube_edges() {
    vector<Edge> edges;
    for (int u = 0; u < cfg::NODE_COUNT; ++u) {
        for (int bit = 0; bit < cfg::DIM; ++bit) {
            int v = u ^ (1 << bit);
            if (u < v) {
                edges.push_back({u, v, bit});
            }
        }
    }
    return edges;
}

bool is_hypercube_edge(int u, int v) {
    int x = u ^ v;
    return x != 0 && ((x & (x - 1)) == 0);
}

static array<array<int, cfg::NODE_COUNT>, cfg::NODE_COUNT> build_hamming_table() {
    array<array<int, cfg::NODE_COUNT>, cfg::NODE_COUNT> dist{};
    for (int i = 0; i < cfg::NODE_COUNT; ++i) {
        for (int j = 0; j < cfg::NODE_COUNT; ++j) {
            dist[i][j] = __builtin_popcount(static_cast<unsigned>(i ^ j));
        }
    }
    return dist;
}

const array<array<int, cfg::NODE_COUNT>, cfg::NODE_COUNT>& hdist_table() {
    static const auto table = build_hamming_table();
    return table;
}

int total_distance(uint64_t state) {
    const auto& h = hdist_table();
    int total = 0;
    for (int node = 0; node < cfg::NODE_COUNT; ++node) {
        int packet = get_packet(state, node);
        total += h[node][packet];
    }
    return total;
}

int misplaced_count(uint64_t state) {
    int count = 0;
    for (int node = 0; node < cfg::NODE_COUNT; ++node) {
        if (get_packet(state, node) != node) {
            ++count;
        }
    }
    return count;
}

int max_packet_distance(uint64_t state) {
    const auto& h = hdist_table();
    int best = 0;
    for (int node = 0; node < cfg::NODE_COUNT; ++node) {
        int packet = get_packet(state, node);
        best = max(best, h[node][packet]);
    }
    return best;
}

array<int, cfg::DIM> global_direction_preference(uint64_t state) {
    const auto& h = hdist_table();
    array<int, cfg::DIM> score{};
    score.fill(0);

    for (int node = 0; node < cfg::NODE_COUNT; ++node) {
        int packet = get_packet(state, node);
        int dist = h[node][packet];
        if (dist == 0) {
            continue;
        }
        int diff = node ^ packet;
        for (int bit = 0; bit < cfg::DIM; ++bit) {
            if (diff & (1 << bit)) {
                score[bit] += dist;
            }
        }
    }

    return score;
}

vector<uint64_t> generate_all_states() {
    vector<uint64_t> states;
    array<int, cfg::NODE_COUNT> a{};
    iota(a.begin(), a.end(), 0);

    do {
        states.push_back(encode_state(a));
    } while (next_permutation(a.begin(), a.end()));

    return states;
}
