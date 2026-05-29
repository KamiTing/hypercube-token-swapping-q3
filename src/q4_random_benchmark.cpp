#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <queue>
#include <random>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace std;

namespace q4 {
constexpr int DIM = 4;
constexpr int NODES = 1 << DIM;
constexpr int EDGE_COUNT = DIM * (1 << (DIM - 1));

struct Edge { int u; int v; int bit; };

uint64_t encode(const array<int, NODES>& a) {
    uint64_t s = 0;
    for (int i = 0; i < NODES; ++i) s |= (static_cast<uint64_t>(a[i]) << (4 * i));
    return s;
}

int get_packet(uint64_t s, int node) { return static_cast<int>((s >> (4 * node)) & 0xFULL); }

uint64_t set_packet(uint64_t s, int node, int packet) {
    uint64_t mask = 0xFULL << (4 * node);
    s &= ~mask;
    s |= (static_cast<uint64_t>(packet) << (4 * node));
    return s;
}

uint64_t swap_nodes(uint64_t s, int u, int v) {
    int pu = get_packet(s, u), pv = get_packet(s, v);
    s = set_packet(s, u, pv);
    s = set_packet(s, v, pu);
    return s;
}

uint64_t target_state() {
    array<int, NODES> a{};
    for (int i = 0; i < NODES; ++i) a[i] = i;
    return encode(a);
}

vector<Edge> edges() {
    vector<Edge> out;
    for (int u = 0; u < NODES; ++u) {
        for (int bit = 0; bit < DIM; ++bit) {
            int v = u ^ (1 << bit);
            if (u < v) out.push_back({u, v, bit});
        }
    }
    return out;
}

int hdist(int a, int b) { return __builtin_popcount(static_cast<unsigned>(a ^ b)); }

int total_distance(uint64_t s) {
    int total = 0;
    for (int node = 0; node < NODES; ++node) total += hdist(node, get_packet(s, node));
    return total;
}

int astar_h(uint64_t s) { return (total_distance(s) + 1) / 2; }

struct SolveResult { int steps; long long expanded; double sec; };

struct ANode {
    int f, h, g;
    uint64_t s;
    bool operator<(const ANode& o) const {
        if (f != o.f) return f > o.f;
        if (h != o.h) return h > o.h;
        return g > o.g;
    }
};

SolveResult astar_min_steps(uint64_t init, const vector<Edge>& es, size_t node_cap) {
    auto t0 = chrono::steady_clock::now();
    uint64_t target = target_state();
    if (init == target) return {0, 0, 0.0};

    priority_queue<ANode> pq;
    unordered_map<uint64_t, int> best;
    best.reserve(300000);
    best.max_load_factor(0.7f);

    int h0 = astar_h(init);
    pq.push({h0, h0, 0, init});
    best[init] = 0;

    long long expanded = 0;

    while (!pq.empty()) {
        ANode cur = pq.top(); pq.pop();
        auto it = best.find(cur.s);
        if (it == best.end() || it->second != cur.g) continue;

        expanded++;
        if (best.size() > node_cap) {
            auto t1 = chrono::steady_clock::now();
            return {-1, expanded, chrono::duration<double>(t1 - t0).count()};
        }

        if (cur.s == target) {
            auto t1 = chrono::steady_clock::now();
            return {cur.g, expanded, chrono::duration<double>(t1 - t0).count()};
        }

        for (const auto& e : es) {
            uint64_t ns = swap_nodes(cur.s, e.u, e.v);
            int ng = cur.g + 1;
            auto old = best.find(ns);
            if (old != best.end() && ng >= old->second) continue;
            best[ns] = ng;
            int nh = astar_h(ns);
            pq.push({ng + nh, nh, ng, ns});
        }
    }

    auto t1 = chrono::steady_clock::now();
    return {-1, expanded, chrono::duration<double>(t1 - t0).count()};
}

struct BeamItem { uint64_t s; int d; int last_e; };

SolveResult beam_steps(uint64_t init, const vector<Edge>& es, int beam_width, int max_depth) {
    auto t0 = chrono::steady_clock::now();
    uint64_t target = target_state();
    if (init == target) return {0, 0, 0.0};

    vector<BeamItem> beam = {{init, 0, -1}};
    unordered_map<uint64_t, int> best_depth;
    best_depth.reserve(300000);
    best_depth.max_load_factor(0.7f);
    best_depth[init] = 0;

    long long expanded = 0;

    for (int depth = 1; depth <= max_depth; ++depth) {
        vector<tuple<int, int, uint64_t, int>> cand;
        cand.reserve(static_cast<size_t>(beam.size()) * es.size());

        for (const auto& item : beam) {
            int old_total = total_distance(item.s);
            for (int eid = 0; eid < static_cast<int>(es.size()); ++eid) {
                if (eid == item.last_e) continue;
                const auto& e = es[eid];
                uint64_t ns = swap_nodes(item.s, e.u, e.v);

                auto it = best_depth.find(ns);
                if (it != best_depth.end() && it->second <= depth) continue;
                best_depth[ns] = depth;

                int new_total = total_distance(ns);
                int misplaced = 0;
                for (int n = 0; n < NODES; ++n) misplaced += (get_packet(ns, n) != n);

                cand.push_back({new_total, misplaced, ns, eid});
                expanded++;

                if (ns == target) {
                    auto t1 = chrono::steady_clock::now();
                    return {depth, expanded, chrono::duration<double>(t1 - t0).count()};
                }
            }
        }

        if (cand.empty()) break;

        sort(cand.begin(), cand.end(), [](const auto& a, const auto& b) {
            return tie(get<0>(a), get<1>(a)) < tie(get<0>(b), get<1>(b));
        });

        beam.clear();
        int keep = min(beam_width, static_cast<int>(cand.size()));
        beam.reserve(keep);
        for (int i = 0; i < keep; ++i) beam.push_back({get<2>(cand[i]), depth, get<3>(cand[i])});
    }

    auto t1 = chrono::steady_clock::now();
    return {-1, expanded, chrono::duration<double>(t1 - t0).count()};
}

struct BatcherResult {
    bool success;
    int swaps;
    int compares;
    int rounds;
    double sec;
};

BatcherResult batcher_baseline(uint64_t init) {
    auto t0 = chrono::steady_clock::now();
    uint64_t s = init;
    int compares = 0;
    int swaps = 0;
    int rounds = 0;

    for (int k = 2; k <= NODES; k <<= 1) {
        for (int j = k >> 1; j > 0; j >>= 1) {
            rounds++;
            for (int i = 0; i < NODES; ++i) {
                int p = i ^ j;
                if (p <= i) continue;
                if (__builtin_popcount(static_cast<unsigned>(i ^ p)) != 1) continue;
                bool asc = ((i & k) == 0);
                int pi = get_packet(s, i), pp = get_packet(s, p);
                bool sw = asc ? (pi > pp) : (pi < pp);
                compares++;
                if (sw) {
                    s = swap_nodes(s, i, p);
                    swaps++;
                }
            }
        }
    }

    auto t1 = chrono::steady_clock::now();
    return {
        s == target_state(),
        swaps,
        compares,
        rounds,
        chrono::duration<double>(t1 - t0).count()
    };
}

uint64_t make_random_state(mt19937_64& rng, int scramble_steps, const vector<Edge>& es) {
    uint64_t s = target_state();
    uniform_int_distribution<int> pick(0, static_cast<int>(es.size()) - 1);
    for (int i = 0; i < scramble_steps; ++i) {
        const auto& e = es[pick(rng)];
        s = swap_nodes(s, e.u, e.v);
    }
    return s;
}

} // namespace q4

int main(int argc, char** argv) {
    using namespace q4;

    int samples = 100;
    int scramble_steps = 20;
    int beam_width = 32;
    int max_depth = 24;
    unsigned long long seed = 42;
    size_t astar_cap = 500000;

    if (argc > 1) samples = stoi(argv[1]);
    if (argc > 2) scramble_steps = stoi(argv[2]);
    if (argc > 3) beam_width = stoi(argv[3]);
    if (argc > 4) max_depth = stoi(argv[4]);
    if (argc > 5) seed = stoull(argv[5]);

    filesystem::create_directories("output");

    const auto es = edges();
    mt19937_64 rng(seed);

    ofstream csv("output/q4_random_benchmark.csv");
    csv << "id,scramble,astar_steps,beam_steps,batcher_swaps,batcher_ok,astar_sec,beam_sec,batcher_sec\n";

    cout << "Q4 random benchmark (A*/Beam vs Batcher baseline)\n";
    cout << "samples=" << samples
         << ", scramble_steps=" << scramble_steps
         << ", beam_width=" << beam_width
         << ", max_depth=" << max_depth
         << ", seed=" << seed << "\n";

    int astar_not_worse_than_batcher = 0;
    int beam_not_worse_than_batcher = 0;
    int astar_failed = 0;
    int beam_failed = 0;
    int batcher_failed = 0;

    double sum_astar = 0.0, sum_beam = 0.0, sum_batcher = 0.0;
    long long sum_batcher_swaps = 0;

    for (int i = 0; i < samples; ++i) {
        uint64_t s = make_random_state(rng, scramble_steps, es);
        auto rastar = astar_min_steps(s, es, astar_cap);
        auto rbeam = beam_steps(s, es, beam_width, max_depth);
        auto rbatch = batcher_baseline(s);

        if (rastar.steps < 0) astar_failed++;
        if (rbeam.steps < 0) beam_failed++;
        if (!rbatch.success) batcher_failed++;

        if (rastar.steps >= 0 && rbatch.success && rastar.steps <= rbatch.swaps) {
            astar_not_worse_than_batcher++;
        }
        if (rbeam.steps >= 0 && rbatch.success && rbeam.steps <= rbatch.swaps) {
            beam_not_worse_than_batcher++;
        }

        sum_astar += rastar.sec;
        sum_beam += rbeam.sec;
        sum_batcher += rbatch.sec;
        sum_batcher_swaps += rbatch.swaps;

        csv << i << "," << scramble_steps << ","
            << rastar.steps << "," << rbeam.steps << "," << rbatch.swaps << ","
            << (rbatch.success ? 1 : 0) << ","
            << fixed << setprecision(6)
            << rastar.sec << "," << rbeam.sec << "," << rbatch.sec << "\n";
    }

    cout << "A* failures=" << astar_failed << "/" << samples << "\n";
    cout << "Beam failures=" << beam_failed << "/" << samples << "\n";
    cout << "Batcher failures=" << batcher_failed << "/" << samples << "\n";
    cout << "A* <= Batcher swaps: " << astar_not_worse_than_batcher << "/" << samples << "\n";
    cout << "Beam <= Batcher swaps: " << beam_not_worse_than_batcher << "/" << samples << "\n";

    cout << fixed << setprecision(6)
         << "avg astar sec=" << (sum_astar / samples)
         << ", avg beam sec=" << (sum_beam / samples)
         << ", avg batcher sec=" << (sum_batcher / samples)
         << ", avg batcher swaps=" << static_cast<double>(sum_batcher_swaps) / samples << "\n";

    cout << "CSV: output/q4_random_benchmark.csv\n";
    return 0;
}
