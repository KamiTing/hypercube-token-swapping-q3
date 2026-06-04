#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <queue>
#include <random>
#include <sstream>
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

enum class HeuristicKind {
    Basic,
    Strong
};

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

int max_packet_distance(uint64_t s) {
    int best = 0;
    for (int node = 0; node < NODES; ++node) {
        best = max(best, hdist(node, get_packet(s, node)));
    }
    return best;
}

int cycle_lower_bound(uint64_t s) {
    array<bool, NODES> seen{};
    int cycles = 0;

    for (int i = 0; i < NODES; ++i) {
        if (seen[i]) continue;

        cycles++;
        int cur = i;
        while (!seen[cur]) {
            seen[cur] = true;
            cur = get_packet(s, cur);
        }
    }

    return NODES - cycles;
}

int permutation_parity(uint64_t s) {
    int parity = 0;
    for (int i = 0; i < NODES; ++i) {
        int pi = get_packet(s, i);
        for (int j = i + 1; j < NODES; ++j) {
            if (pi > get_packet(s, j)) parity ^= 1;
        }
    }
    return parity;
}

int basic_astar_h(uint64_t s) { return (total_distance(s) + 1) / 2; }

int parity_adjust(uint64_t s, int lower_bound) {
    int parity = permutation_parity(s);
    if ((lower_bound & 1) != parity) lower_bound++;
    return lower_bound;
}

int strong_astar_h(uint64_t s) {
    int h1 = basic_astar_h(s);
    int h2 = max_packet_distance(s);
    int h3 = cycle_lower_bound(s);
    return parity_adjust(s, max({h1, h2, h3}));
}

int astar_h(uint64_t s, HeuristicKind kind) {
    return kind == HeuristicKind::Strong ? strong_astar_h(s) : basic_astar_h(s);
}

string state_hex(uint64_t s) {
    ostringstream out;
    out << "0x" << uppercase << hex << setw(16) << setfill('0') << s;
    return out.str();
}

struct SolveResult { int steps; long long expanded; double sec; };

struct SwapStep {
    int u;
    int v;
};

struct ParentInfo {
    uint64_t prev;
    SwapStep swap;
};

vector<SwapStep> reconstruct_path(
    uint64_t init,
    uint64_t goal,
    const unordered_map<uint64_t, ParentInfo>& parent
) {
    vector<SwapStep> path;
    if (init == goal) return path;
    if (parent.find(goal) == parent.end()) return path;

    uint64_t cur = goal;
    while (cur != init) {
        const auto p = parent.at(cur);
        path.push_back(p.swap);
        cur = p.prev;
    }
    reverse(path.begin(), path.end());
    return path;
}

struct PathSolveResult {
    int steps = -1;
    long long expanded = 0;
    double sec = 0.0;
    vector<SwapStep> swaps;
};

struct ANode {
    int f, h, g;
    uint64_t s;
    bool operator<(const ANode& o) const {
        if (f != o.f) return f > o.f;
        if (h != o.h) return h > o.h;
        return g > o.g;
    }
};

PathSolveResult astar_min_steps(
    uint64_t init,
    const vector<Edge>& es,
    size_t node_cap,
    HeuristicKind kind,
    bool record_path = false
) {
    auto t0 = chrono::steady_clock::now();
    uint64_t target = target_state();
    if (init == target) return {0, 0, 0.0, {}};

    priority_queue<ANode> pq;
    unordered_map<uint64_t, int> best;
    best.reserve(300000);
    best.max_load_factor(0.7f);

    unordered_map<uint64_t, ParentInfo> parent;
    if (record_path) {
        parent.reserve(300000);
        parent.max_load_factor(0.7f);
        parent[init] = {init, {-1, -1}};
    }

    int h0 = astar_h(init, kind);
    pq.push({h0, h0, 0, init});
    best[init] = 0;

    long long expanded = 0;

    while (!pq.empty()) {
        ANode cur = pq.top(); pq.pop();
        auto it = best.find(cur.s);
        if (it == best.end() || it->second != cur.g) continue;

        expanded++;
        if (node_cap > 0 && best.size() > node_cap) {
            auto t1 = chrono::steady_clock::now();
            return {-1, expanded, chrono::duration<double>(t1 - t0).count(), {}};
        }

        if (cur.s == target) {
            auto t1 = chrono::steady_clock::now();
            vector<SwapStep> path = record_path ? reconstruct_path(init, target, parent) : vector<SwapStep>{};
            return {cur.g, expanded, chrono::duration<double>(t1 - t0).count(), path};
        }

        for (const auto& e : es) {
            uint64_t ns = swap_nodes(cur.s, e.u, e.v);
            int ng = cur.g + 1;
            auto old = best.find(ns);
            if (old != best.end() && ng >= old->second) continue;
            best[ns] = ng;
            if (record_path) {
                parent[ns] = {cur.s, {e.u, e.v}};
            }
            int nh = astar_h(ns, kind);
            pq.push({ng + nh, nh, ng, ns});
        }
    }

    auto t1 = chrono::steady_clock::now();
    return {-1, expanded, chrono::duration<double>(t1 - t0).count(), {}};
}

struct BeamItem {
    uint64_t s;
    int d;
    int last_e;
    vector<SwapStep> swaps;
};

struct BeamCandidate {
    int total_dist;
    int misplaced;
    uint64_t state;
    int edge_id;
    vector<SwapStep> swaps;
};

PathSolveResult beam_steps(
    uint64_t init,
    const vector<Edge>& es,
    int beam_width,
    int max_depth,
    bool record_path = false
) {
    auto t0 = chrono::steady_clock::now();
    uint64_t target = target_state();
    if (init == target) return {0, 0, 0.0, {}};

    vector<BeamItem> beam = {{init, 0, -1, {}}};
    unordered_map<uint64_t, int> best_depth;
    best_depth.reserve(300000);
    best_depth.max_load_factor(0.7f);
    best_depth[init] = 0;

    long long expanded = 0;

    for (int depth = 1; depth <= max_depth; ++depth) {
        vector<BeamCandidate> cand;
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

                vector<SwapStep> next_path;
                if (record_path) {
                    next_path = item.swaps;
                    next_path.push_back({e.u, e.v});
                }

                cand.push_back({new_total, misplaced, ns, eid, next_path});
                expanded++;

                if (ns == target) {
                    auto t1 = chrono::steady_clock::now();
                    return {depth, expanded, chrono::duration<double>(t1 - t0).count(), next_path};
                }
            }
        }

        if (cand.empty()) break;

        sort(cand.begin(), cand.end(), [](const auto& a, const auto& b) {
            return tie(a.total_dist, a.misplaced) < tie(b.total_dist, b.misplaced);
        });

        beam.clear();
        int keep = min(beam_width, static_cast<int>(cand.size()));
        beam.reserve(keep);
        for (int i = 0; i < keep; ++i) {
            beam.push_back({cand[i].state, depth, cand[i].edge_id, cand[i].swaps});
        }
    }

    auto t1 = chrono::steady_clock::now();
    return {-1, expanded, chrono::duration<double>(t1 - t0).count(), {}};
}

struct BatcherResult {
    bool success;
    int swaps;
    int compares;
    int rounds;
    double sec;
    vector<SwapStep> path;
};

BatcherResult batcher_baseline(uint64_t init, bool record_path = false) {
    auto t0 = chrono::steady_clock::now();
    uint64_t s = init;
    int compares = 0;
    int swaps = 0;
    int rounds = 0;
    vector<SwapStep> path;

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
                    if (record_path) {
                        path.push_back({i, p});
                    }
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
        chrono::duration<double>(t1 - t0).count(),
        path
    };
}

uint64_t make_random_state(mt19937_64& rng) {
    array<int, NODES> a{};
    iota(a.begin(), a.end(), 0);
    shuffle(a.begin(), a.end(), rng);
    return encode(a);
}

string csv_escape(const string& value) {
    string out = "\"";
    for (char ch : value) {
        if (ch == '"') {
            out += "\"\"";
        } else {
            out += ch;
        }
    }
    out += "\"";
    return out;
}

string state_perm(uint64_t s) {
    ostringstream out;
    for (int node = 0; node < NODES; ++node) {
        if (node > 0) out << ' ';
        out << get_packet(s, node);
    }
    return out.str();
}

string path_string(const vector<SwapStep>& swaps) {
    ostringstream out;
    for (size_t i = 0; i < swaps.size(); ++i) {
        if (i > 0) out << ' ';
        out << swaps[i].u << "-" << swaps[i].v;
    }
    return out.str();
}

} // namespace q4

int main(int argc, char** argv) {
    using namespace q4;

    int samples = 100;
    int beam_width = 32;
    int max_depth = 24;
    unsigned long long seed = 42;
    size_t astar_cap = 0;
    bool parallel_methods = false;
    bool record_paths = true;
    filesystem::path output_dir = "output";

    if (argc > 1) samples = stoi(argv[1]);
    if (argc > 2) beam_width = stoi(argv[2]);
    if (argc > 3) max_depth = stoi(argv[3]);
    if (argc > 4) seed = stoull(argv[4]);
    if (argc > 5) astar_cap = stoull(argv[5]);
    if (argc > 6) parallel_methods = stoi(argv[6]) != 0;
    if (argc > 7) record_paths = stoi(argv[7]) != 0;
    if (argc > 8) output_dir = argv[8];

    filesystem::create_directories(output_dir);
    const filesystem::path benchmark_csv_path = output_dir / "q4_random_benchmark.csv";
    const filesystem::path routing_paths_csv_path = output_dir / "q4_random_routing_paths.csv";

    const auto es = edges();
    mt19937_64 rng(seed);

    ofstream csv(benchmark_csv_path);
    csv << "id,total_dist,state_hex,mode,"
        << "astar_steps,strong_astar_steps,beam_steps,batcher_swaps,batcher_ok,"
        << "astar_expanded,strong_astar_expanded,beam_expanded,"
        << "astar_sec,strong_astar_sec,beam_sec,batcher_sec,case_wall_sec\n";

    ofstream path_csv;
    if (record_paths) {
        path_csv.open(routing_paths_csv_path);
        path_csv << "id,total_dist,state_hex,state_perm,mode,"
                 << "astar_steps,astar_path,"
                 << "strong_astar_steps,strong_astar_path,"
                 << "beam_steps,beam_path,"
                 << "batcher_swaps,batcher_path,batcher_ok\n";
    }

    cout << "Q4 full-random benchmark (Basic A*/Strong A*/Beam vs Batcher baseline)\n";
    cout << "generator=random_permutation (uniform shuffle over all 16! token placements)\n";
    cout << "samples=" << samples
         << ", beam_width=" << beam_width
         << ", max_depth=" << max_depth
         << ", seed=" << seed
         << ", astar_cap=" << astar_cap
         << ", mode=" << (parallel_methods ? "parallel_methods" : "sequential_methods")
         << ", record_paths=" << (record_paths ? 1 : 0)
         << ", output_dir=" << output_dir.generic_string() << "\n";

    int astar_not_worse_than_batcher = 0;
    int strong_astar_not_worse_than_batcher = 0;
    int beam_not_worse_than_batcher = 0;
    int astar_failed = 0;
    int strong_astar_failed = 0;
    int beam_failed = 0;
    int batcher_failed = 0;
    int astar_strong_same_steps = 0;
    int strong_expanded_less_or_equal = 0;

    double sum_astar = 0.0, sum_strong_astar = 0.0, sum_beam = 0.0, sum_batcher = 0.0;
    double sum_case_wall = 0.0;
    long long sum_astar_expanded = 0, sum_strong_astar_expanded = 0, sum_beam_expanded = 0;
    long long sum_astar_steps = 0, sum_strong_astar_steps = 0, sum_beam_steps = 0, sum_batcher_swaps = 0;
    int astar_solved = 0, strong_astar_solved = 0, beam_solved = 0, batcher_solved = 0;

    auto bench_start = chrono::steady_clock::now();
    auto print_progress = [&](int done) {
        constexpr int width = 36;
        double ratio = samples > 0 ? static_cast<double>(done) / samples : 1.0;
        int filled = static_cast<int>(ratio * width);
        double elapsed = chrono::duration<double>(chrono::steady_clock::now() - bench_start).count();

        cout << "\r[";
        for (int j = 0; j < width; ++j) cout << (j < filled ? '#' : '.');
        cout << "] " << setw(5) << done << "/" << samples
             << " elapsed=" << fixed << setprecision(0) << elapsed << "s"
             << " astar_fail=" << astar_failed
             << " strong_fail=" << strong_astar_failed
             << " beam_fail=" << beam_failed
             << flush;
    };

    print_progress(0);

    for (int i = 0; i < samples; ++i) {
        uint64_t s = make_random_state(rng);
        int td = total_distance(s);

        auto case_start = chrono::steady_clock::now();

        PathSolveResult rastar;
        PathSolveResult rstrong;
        PathSolveResult rbeam;
        BatcherResult rbatch;

        if (parallel_methods) {
            auto fastar = async(launch::async, [&es, s, astar_cap]() {
                return astar_min_steps(s, es, astar_cap, HeuristicKind::Basic, false);
            });
            auto fstrong = async(launch::async, [&es, s, astar_cap, record_paths]() {
                return astar_min_steps(s, es, astar_cap, HeuristicKind::Strong, record_paths);
            });
            auto fbeam = async(launch::async, [&es, s, beam_width, max_depth, record_paths]() {
                return beam_steps(s, es, beam_width, max_depth, record_paths);
            });
            auto fbatch = async(launch::async, [s, record_paths]() {
                return batcher_baseline(s, record_paths);
            });

            rastar = fastar.get();
            rstrong = fstrong.get();
            rbeam = fbeam.get();
            rbatch = fbatch.get();
        } else {
            rastar = astar_min_steps(s, es, astar_cap, HeuristicKind::Basic, false);
            rstrong = astar_min_steps(s, es, astar_cap, HeuristicKind::Strong, record_paths);
            rbeam = beam_steps(s, es, beam_width, max_depth, record_paths);
            rbatch = batcher_baseline(s, record_paths);
        }

        double case_wall_sec = chrono::duration<double>(chrono::steady_clock::now() - case_start).count();

        if (rastar.steps < 0) astar_failed++;
        if (rstrong.steps < 0) strong_astar_failed++;
        if (rbeam.steps < 0) beam_failed++;
        if (!rbatch.success) batcher_failed++;

        if (rastar.steps >= 0) {
            astar_solved++;
            sum_astar_steps += rastar.steps;
            sum_astar_expanded += rastar.expanded;
        }
        if (rstrong.steps >= 0) {
            strong_astar_solved++;
            sum_strong_astar_steps += rstrong.steps;
            sum_strong_astar_expanded += rstrong.expanded;
        }
        if (rbeam.steps >= 0) {
            beam_solved++;
            sum_beam_steps += rbeam.steps;
            sum_beam_expanded += rbeam.expanded;
        }
        if (rbatch.success) {
            batcher_solved++;
            sum_batcher_swaps += rbatch.swaps;
        }

        if (rastar.steps >= 0 && rstrong.steps >= 0 && rastar.steps == rstrong.steps) {
            astar_strong_same_steps++;
        }
        if (rastar.steps >= 0 && rstrong.steps >= 0 && rstrong.expanded <= rastar.expanded) {
            strong_expanded_less_or_equal++;
        }
        if (rastar.steps >= 0 && rbatch.success && rastar.steps <= rbatch.swaps) {
            astar_not_worse_than_batcher++;
        }
        if (rstrong.steps >= 0 && rbatch.success && rstrong.steps <= rbatch.swaps) {
            strong_astar_not_worse_than_batcher++;
        }
        if (rbeam.steps >= 0 && rbatch.success && rbeam.steps <= rbatch.swaps) {
            beam_not_worse_than_batcher++;
        }

        sum_astar += rastar.sec;
        sum_strong_astar += rstrong.sec;
        sum_beam += rbeam.sec;
        sum_batcher += rbatch.sec;
        sum_case_wall += case_wall_sec;

        csv << i << "," << td << "," << state_hex(s) << ","
            << (parallel_methods ? "parallel_methods" : "sequential_methods") << ","
            << rastar.steps << "," << rstrong.steps << "," << rbeam.steps << "," << rbatch.swaps << ","
            << (rbatch.success ? 1 : 0) << ","
            << rastar.expanded << "," << rstrong.expanded << "," << rbeam.expanded << ","
            << fixed << setprecision(6)
            << rastar.sec << "," << rstrong.sec << "," << rbeam.sec << "," << rbatch.sec << ","
            << case_wall_sec << "\n";

        if (record_paths) {
            path_csv << i << "," << td << "," << state_hex(s) << ","
                     << csv_escape(state_perm(s)) << ","
                     << (parallel_methods ? "parallel_methods" : "sequential_methods") << ","
                     << rastar.steps << "," << csv_escape(path_string(rastar.swaps)) << ","
                     << rstrong.steps << "," << csv_escape(path_string(rstrong.swaps)) << ","
                     << rbeam.steps << "," << csv_escape(path_string(rbeam.swaps)) << ","
                     << rbatch.swaps << "," << csv_escape(path_string(rbatch.path)) << ","
                     << (rbatch.success ? 1 : 0) << "\n";
        }

        if ((i + 1) % 10 == 0 || i + 1 == samples) {
            csv.flush();
            if (record_paths) path_csv.flush();
            print_progress(i + 1);
        }
    }

    cout << "\n";
    cout << "Basic A* failures=" << astar_failed << "/" << samples << "\n";
    cout << "Strong A* failures=" << strong_astar_failed << "/" << samples << "\n";
    cout << "Beam failures=" << beam_failed << "/" << samples << "\n";
    cout << "Batcher failures=" << batcher_failed << "/" << samples << "\n";
    cout << "Basic A* and Strong A* same steps: " << astar_strong_same_steps << "/" << samples << "\n";
    cout << "Strong A* expanded <= Basic A*: " << strong_expanded_less_or_equal << "/" << samples << "\n";
    cout << "Basic A* <= Batcher swaps: " << astar_not_worse_than_batcher << "/" << samples << "\n";
    cout << "Strong A* <= Batcher swaps: " << strong_astar_not_worse_than_batcher << "/" << samples << "\n";
    cout << "Beam <= Batcher swaps: " << beam_not_worse_than_batcher << "/" << samples << "\n";

    cout << fixed << setprecision(6)
         << "avg basic astar sec=" << (sum_astar / samples)
         << ", avg strong astar sec=" << (sum_strong_astar / samples)
         << ", avg beam sec=" << (sum_beam / samples)
         << ", avg batcher sec=" << (sum_batcher / samples)
         << ", avg case wall sec=" << (sum_case_wall / samples)
         << "\n";

    cout << "avg solved steps: "
         << "Basic A*=" << (astar_solved ? static_cast<double>(sum_astar_steps) / astar_solved : 0.0)
         << ", Strong A*=" << (strong_astar_solved ? static_cast<double>(sum_strong_astar_steps) / strong_astar_solved : 0.0)
         << ", Beam=" << (beam_solved ? static_cast<double>(sum_beam_steps) / beam_solved : 0.0)
         << ", Batcher=" << (batcher_solved ? static_cast<double>(sum_batcher_swaps) / batcher_solved : 0.0)
         << "\n";

    cout << "avg expanded: "
         << "Basic A*=" << (astar_solved ? static_cast<double>(sum_astar_expanded) / astar_solved : 0.0)
         << ", Strong A*=" << (strong_astar_solved ? static_cast<double>(sum_strong_astar_expanded) / strong_astar_solved : 0.0)
         << ", Beam=" << (beam_solved ? static_cast<double>(sum_beam_expanded) / beam_solved : 0.0)
         << "\n";

    cout << "CSV: " << benchmark_csv_path.generic_string() << "\n";
    if (record_paths) {
        cout << "Routing paths CSV: " << routing_paths_csv_path.generic_string() << "\n";
    }
    return 0;
}
