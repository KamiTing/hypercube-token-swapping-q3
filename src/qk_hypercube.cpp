#include "qk/hypercube.h"

#include <algorithm>
#ifdef _MSC_VER
#include <intrin.h>
#endif

using namespace std;

namespace qk {

vector<Edge> edges(int dim) {
    vector<Edge> out;
    int n = node_count(dim);
    for (int u = 0; u < n; ++u) {
        for (int bit = 0; bit < dim; ++bit) {
            int v = u ^ (1 << bit);
            if (u < v) {
                out.push_back({u, v, bit});
            }
        }
    }
    return out;
}

int hdist(int a, int b) {
#ifdef _MSC_VER
    return static_cast<int>(__popcnt(static_cast<unsigned>(a ^ b)));
#else
    return __builtin_popcount(static_cast<unsigned>(a ^ b));
#endif
}

int total_distance(const State& s) {
    int total = 0;
    for (int node = 0; node < static_cast<int>(s.size()); ++node) {
        total += hdist(node, s[node]);
    }
    return total;
}

int misplaced_count(const State& s) {
    int total = 0;
    for (int node = 0; node < static_cast<int>(s.size()); ++node) {
        total += (s[node] != node);
    }
    return total;
}

int max_packet_distance(const State& s) {
    int best = 0;
    for (int node = 0; node < static_cast<int>(s.size()); ++node) {
        best = max(best, hdist(node, s[node]));
    }
    return best;
}

int cycle_lower_bound(const State& s) {
    vector<char> seen(s.size(), 0);
    int cycles = 0;

    for (int i = 0; i < static_cast<int>(s.size()); ++i) {
        if (seen[i]) {
            continue;
        }

        cycles++;
        int cur = i;
        while (!seen[cur]) {
            seen[cur] = 1;
            cur = s[cur];
        }
    }

    return static_cast<int>(s.size()) - cycles;
}

int permutation_parity(const State& s) {
    int parity = 0;
    for (int i = 0; i < static_cast<int>(s.size()); ++i) {
        for (int j = i + 1; j < static_cast<int>(s.size()); ++j) {
            if (s[i] > s[j]) {
                parity ^= 1;
            }
        }
    }
    return parity;
}

int basic_lower_bound(const State& s) {
    return (total_distance(s) + 1) / 2;
}

int parity_adjust(const State& s, int lower_bound) {
    int parity = permutation_parity(s);
    if ((lower_bound & 1) != parity) {
        lower_bound++;
    }
    return lower_bound;
}

int strong_lower_bound(const State& s) {
    return parity_adjust(s, max({basic_lower_bound(s), max_packet_distance(s), cycle_lower_bound(s)}));
}

bool path_reaches_goal(State s, const vector<SwapStep>& path, int dim) {
    int n = node_count(dim);
    for (const auto& step : path) {
        if (step.u < 0 || step.u >= n || step.v < 0 || step.v >= n) {
            return false;
        }
        if (hdist(step.u, step.v) != 1) {
            return false;
        }
        swap(s[step.u], s[step.v]);
    }
    return s == target_state(n);
}

} // namespace qk
