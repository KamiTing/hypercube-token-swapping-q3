#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <queue>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <omp.h>

using namespace std;

// ============================================================
// Basic settings
// ============================================================

static constexpr int DIM = 3;                 // Q3
static constexpr int NODE_COUNT = 1 << DIM;   // 8 nodes
static constexpr int EDGE_COUNT = DIM * (1 << (DIM - 1)); // Q3 = 12 edges
static constexpr int MAX_STEP_BUCKET = 64;

static constexpr bool RUN_BFS = true;
static constexpr bool RUN_ASTAR = true;
static constexpr bool RUN_BEAM = true;
static constexpr bool RUN_BATCHER = true;

static constexpr int BEAM_WIDTH = 100;
static constexpr int MAX_DEPTH = 30;

// ============================================================
// State encoding
// ============================================================
//
// state[node] = packet
//
// Q3 has 8 nodes, packet id is 0~7.
// Each packet is stored in 4 bits.
// A complete state uses 8 * 4 = 32 bits in uint64_t.
// ============================================================

uint64_t encode_state(const array<int, NODE_COUNT>& a) {
    uint64_t s = 0;

    for (int i = 0; i < NODE_COUNT; ++i) {
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
    array<int, NODE_COUNT> a{};

    for (int i = 0; i < NODE_COUNT; ++i) {
        a[i] = i;
    }

    return encode_state(a);
}

bool is_solved(uint64_t state) {
    return state == target_state();
}

// ============================================================
// Hypercube tools
// ============================================================

struct Edge {
    int u;
    int v;
    int bit;
};

vector<Edge> generate_hypercube_edges() {
    vector<Edge> edges;

    for (int u = 0; u < NODE_COUNT; ++u) {
        for (int bit = 0; bit < DIM; ++bit) {
            int v = u ^ (1 << bit);

            // Avoid duplicated undirected edges.
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

// ============================================================
// Distance precomputation
// ============================================================

array<array<int, NODE_COUNT>, NODE_COUNT> build_hamming_table() {
    array<array<int, NODE_COUNT>, NODE_COUNT> dist{};

    for (int i = 0; i < NODE_COUNT; ++i) {
        for (int j = 0; j < NODE_COUNT; ++j) {
            dist[i][j] = __builtin_popcount(static_cast<unsigned>(i ^ j));
        }
    }

    return dist;
}

static const auto HDIST = build_hamming_table();

int total_distance(uint64_t state) {
    int total = 0;

    for (int node = 0; node < NODE_COUNT; ++node) {
        int packet = get_packet(state, node);
        total += HDIST[node][packet];
    }

    return total;
}

int misplaced_count(uint64_t state) {
    int count = 0;

    for (int node = 0; node < NODE_COUNT; ++node) {
        if (get_packet(state, node) != node) {
            ++count;
        }
    }

    return count;
}

int max_packet_distance(uint64_t state) {
    int best = 0;

    for (int node = 0; node < NODE_COUNT; ++node) {
        int packet = get_packet(state, node);
        best = max(best, HDIST[node][packet]);
    }

    return best;
}

// ============================================================
// Direction preference for Beam Search
// ============================================================

array<int, DIM> global_direction_preference(uint64_t state) {
    array<int, DIM> score{};
    score.fill(0);

    for (int node = 0; node < NODE_COUNT; ++node) {
        int packet = get_packet(state, node);
        int dist = HDIST[node][packet];

        if (dist == 0) {
            continue;
        }

        int diff = node ^ packet;

        for (int bit = 0; bit < DIM; ++bit) {
            if (diff & (1 << bit)) {
                score[bit] += dist;
            }
        }
    }

    return score;
}

// ============================================================
// BFS exact shortest swap count
// ============================================================

int bfs_min_steps(uint64_t initial_state, const vector<Edge>& edges) {
    const uint64_t target = target_state();

    if (initial_state == target) {
        return 0;
    }

    queue<pair<uint64_t, int>> q;
    unordered_set<uint64_t> visited;

    visited.reserve(50000);
    visited.max_load_factor(0.7f);

    q.push({initial_state, 0});
    visited.insert(initial_state);

    while (!q.empty()) {
        auto [state, depth] = q.front();
        q.pop();

        for (const auto& e : edges) {
            uint64_t next_state = swap_nodes(state, e.u, e.v);

            if (visited.find(next_state) != visited.end()) {
                continue;
            }

            if (next_state == target) {
                return depth + 1;
            }

            visited.insert(next_state);
            q.push({next_state, depth + 1});
        }
    }

    return -1;
}

// ============================================================
// A* exact shortest swap count
// ============================================================

int astar_heuristic(uint64_t state) {
    // One swap can reduce total Hamming distance by at most 2.
    // Therefore ceil(total_distance / 2) is an admissible lower bound.
    return (total_distance(state) + 1) / 2;
}

struct AStarNode {
    int f;
    int h;
    int g;
    uint64_t state;

    bool operator<(const AStarNode& other) const {
        // priority_queue is max-heap by default, so reverse comparison.
        if (f != other.f) {
            return f > other.f;
        }

        if (h != other.h) {
            return h > other.h;
        }

        return g > other.g;
    }
};

int astar_min_steps(uint64_t initial_state, const vector<Edge>& edges) {
    const uint64_t target = target_state();

    if (initial_state == target) {
        return 0;
    }

    priority_queue<AStarNode> pq;
    unordered_map<uint64_t, int> best_g;

    best_g.reserve(50000);
    best_g.max_load_factor(0.7f);

    int h0 = astar_heuristic(initial_state);

    pq.push({h0, h0, 0, initial_state});
    best_g[initial_state] = 0;

    while (!pq.empty()) {
        AStarNode cur = pq.top();
        pq.pop();

        auto it = best_g.find(cur.state);
        if (it == best_g.end() || cur.g != it->second) {
            continue;
        }

        if (cur.state == target) {
            return cur.g;
        }

        for (const auto& e : edges) {
            uint64_t next_state = swap_nodes(cur.state, e.u, e.v);
            int next_g = cur.g + 1;

            auto old = best_g.find(next_state);
            if (old != best_g.end() && next_g >= old->second) {
                continue;
            }

            best_g[next_state] = next_g;

            int next_h = astar_heuristic(next_state);
            int next_f = next_g + next_h;

            pq.push({next_f, next_h, next_g, next_state});
        }
    }

    return -1;
}

// ============================================================
// Beam Search heuristic
// ============================================================

struct BeamItem {
    uint64_t state;
    int depth;
    int last_edge_id;
    array<unsigned char, EDGE_COUNT> used_edges;
};

struct BeamScore {
    int total_dist;
    int misplaced;
    int max_dist;
    int repeat_penalty;
    int neg_improvement;
    int neg_local_improvement;
    int neg_dir_score;
    int neg_touched_max_dist;
    int depth;

    bool operator<(const BeamScore& other) const {
        return tie(
            total_dist,
            misplaced,
            max_dist,
            repeat_penalty,
            neg_improvement,
            neg_local_improvement,
            neg_dir_score,
            neg_touched_max_dist,
            depth
        ) < tie(
            other.total_dist,
            other.misplaced,
            other.max_dist,
            other.repeat_penalty,
            other.neg_improvement,
            other.neg_local_improvement,
            other.neg_dir_score,
            other.neg_touched_max_dist,
            other.depth
        );
    }
};

struct BeamCandidate {
    BeamScore score;
    BeamItem item;

    bool operator<(const BeamCandidate& other) const {
        return score < other.score;
    }
};

int beam_search_steps(
    uint64_t initial_state,
    const vector<Edge>& edges,
    int beam_width = BEAM_WIDTH,
    int max_depth = MAX_DEPTH
) {
    const uint64_t target = target_state();

    if (initial_state == target) {
        return 0;
    }

    BeamItem start;
    start.state = initial_state;
    start.depth = 0;
    start.last_edge_id = -1;
    start.used_edges.fill(0);

    vector<BeamItem> beam;
    beam.push_back(start);

    unordered_map<uint64_t, int> visited_best_depth;
    visited_best_depth.reserve(50000);
    visited_best_depth.max_load_factor(0.7f);
    visited_best_depth[initial_state] = 0;

    for (int depth = 1; depth <= max_depth; ++depth) {
        vector<BeamCandidate> candidates;
        candidates.reserve(static_cast<size_t>(beam.size()) * edges.size());

        for (const auto& item : beam) {
            uint64_t state = item.state;

            array<int, DIM> dir_score = global_direction_preference(state);
            int old_total = total_distance(state);

            for (int eid = 0; eid < static_cast<int>(edges.size()); ++eid) {
                const Edge& e = edges[eid];

                // Prevent immediate undoing the same edge.
                if (item.last_edge_id == eid) {
                    continue;
                }

                uint64_t next_state = swap_nodes(state, e.u, e.v);

                auto old_depth = visited_best_depth.find(next_state);
                if (old_depth != visited_best_depth.end() && old_depth->second <= depth) {
                    continue;
                }

                int new_total = total_distance(next_state);
                int improvement = old_total - new_total;

                int packet_u = get_packet(state, e.u);
                int packet_v = get_packet(state, e.v);

                int old_du = HDIST[e.u][packet_u];
                int old_dv = HDIST[e.v][packet_v];
                int new_du = HDIST[e.v][packet_u];
                int new_dv = HDIST[e.u][packet_v];

                int local_improvement = (old_du + old_dv) - (new_du + new_dv);
                int touched_max_dist = max(old_du, old_dv);

                BeamItem next_item;
                next_item.state = next_state;
                next_item.depth = depth;
                next_item.last_edge_id = eid;
                next_item.used_edges = item.used_edges;
                next_item.used_edges[eid]++;

                int repeat_penalty = 0;
                for (unsigned char c : next_item.used_edges) {
                    if (c > 1) {
                        repeat_penalty += static_cast<int>(c) - 1;
                    }
                }

                BeamScore score;
                score.total_dist = new_total;
                score.misplaced = misplaced_count(next_state);
                score.max_dist = max_packet_distance(next_state);
                score.repeat_penalty = repeat_penalty;
                score.neg_improvement = -improvement;
                score.neg_local_improvement = -local_improvement;
                score.neg_dir_score = -dir_score[e.bit];
                score.neg_touched_max_dist = -touched_max_dist;
                score.depth = depth;

                candidates.push_back({score, next_item});

                visited_best_depth[next_state] = depth;

                if (next_state == target) {
                    return depth;
                }
            }
        }

        if (candidates.empty()) {
            break;
        }

        sort(candidates.begin(), candidates.end());

        beam.clear();

        int keep = min(beam_width, static_cast<int>(candidates.size()));
        beam.reserve(keep);

        for (int i = 0; i < keep; ++i) {
            beam.push_back(candidates[i].item);
        }
    }

    return -1;
}

// ============================================================
// Batcher's merge / sorting-network baseline
// ============================================================
//
// This baseline uses a Batcher-style compare-exchange sorting network.
// The comparator partner is defined as i XOR j.
// Since j is always a power of two, i and i XOR j differ in exactly one bit.
// Therefore, every actual swap is a valid Q3 hypercube edge.
//
// This method is deterministic and does not search for the minimum swap count.
// BFS remains the true table for optimal shortest swap counts.
// ============================================================

struct BatcherResult {
    int compare_count;
    int swap_count;
    int round_count;
    bool success;
};

BatcherResult batcher_merge_sort_baseline(uint64_t initial_state) {
    uint64_t state = initial_state;

    int compare_count = 0;
    int swap_count = 0;
    int round_count = 0;

    const int N = NODE_COUNT;

    // Batcher-style bitonic merge/sorting network.
    // k controls the size of the merging sequence.
    // j controls the compare-exchange distance.
    for (int k = 2; k <= N; k <<= 1) {
        for (int j = k >> 1; j > 0; j >>= 1) {
            round_count++;

            for (int i = 0; i < N; ++i) {
                int partner = i ^ j;

                // Avoid duplicate comparator.
                if (partner <= i) {
                    continue;
                }

                // Since partner = i XOR j and j is a power of two,
                // this comparator is always a legal hypercube edge.
                if (!is_hypercube_edge(i, partner)) {
                    continue;
                }

                bool ascending = ((i & k) == 0);

                int pi = get_packet(state, i);
                int pj = get_packet(state, partner);

                compare_count++;

                bool need_swap = false;

                if (ascending) {
                    need_swap = (pi > pj);
                } else {
                    need_swap = (pi < pj);
                }

                if (need_swap) {
                    state = swap_nodes(state, i, partner);
                    swap_count++;
                }
            }
        }
    }

    return {
        compare_count,
        swap_count,
        round_count,
        is_solved(state)
    };
}

// ============================================================
// Generate all Q3 permutations
// ============================================================

vector<uint64_t> generate_all_states() {
    vector<uint64_t> states;

    array<int, NODE_COUNT> a{};
    iota(a.begin(), a.end(), 0);

    do {
        states.push_back(encode_state(a));
    } while (next_permutation(a.begin(), a.end()));

    return states;
}

// ============================================================
// Utility functions
// ============================================================

double safe_div(double a, double b) {
    return (b == 0.0) ? 0.0 : (a / b);
}

double percent(long long part, long long total) {
    if (total == 0) {
        return 0.0;
    }

    return 100.0 * static_cast<double>(part) / static_cast<double>(total);
}

// ============================================================
// CSV output
// ============================================================

void write_distribution_csv(
    const array<long long, MAX_STEP_BUCKET>& bfs_counter,
    const array<long long, MAX_STEP_BUCKET>& astar_counter,
    const array<long long, MAX_STEP_BUCKET>& beam_counter,
    const array<long long, MAX_STEP_BUCKET>& batcher_counter,
    const string& filename
) {
    ofstream fout(filename);

    fout << "steps,bfs_count,astar_count,beam_count,batcher_count\n";

    for (int step = 0; step < MAX_STEP_BUCKET; ++step) {
        if (bfs_counter[step] == 0 &&
            astar_counter[step] == 0 &&
            beam_counter[step] == 0 &&
            batcher_counter[step] == 0) {
            continue;
        }

        fout << step << ","
             << bfs_counter[step] << ","
             << astar_counter[step] << ","
             << beam_counter[step] << ","
             << batcher_counter[step] << "\n";
    }

    fout.close();
}

// ============================================================
// Text report output
// ============================================================

void write_text_report(
    const string& filename,
    int total_cases,
    int edge_count,
    int max_threads,
    double total_elapsed,
    double total_bfs_time,
    double total_astar_time,
    double total_beam_time,
    double total_batcher_time,
    long long astar_match_bfs,
    long long astar_mismatch,
    long long beam_success,
    long long beam_failed,
    long long beam_match_bfs,
    long long beam_not_optimal,
    long long batcher_success,
    long long batcher_failed,
    long long batcher_match_bfs,
    long long batcher_not_optimal,
    long long total_batcher_compares,
    long long total_batcher_swaps,
    long long total_batcher_rounds,
    const array<long long, MAX_STEP_BUCKET>& bfs_counter,
    const array<long long, MAX_STEP_BUCKET>& astar_counter,
    const array<long long, MAX_STEP_BUCKET>& beam_counter,
    const array<long long, MAX_STEP_BUCKET>& batcher_counter
) {
    ofstream fout(filename);

    fout << fixed << setprecision(6);

    fout << "========================================\n";
    fout << "Q3 Hypercube Full Permutation Test Report\n";
    fout << "========================================\n\n";

    fout << "[Experiment Settings]\n";
    fout << "Hypercube dimension              : " << DIM << "\n";
    fout << "Number of nodes                  : " << NODE_COUNT << "\n";
    fout << "Total states                     : " << total_cases << "\n";
    fout << "Hypercube edges                  : " << edge_count << "\n";
    fout << "OpenMP max threads               : " << max_threads << "\n";
    fout << "RUN_BFS                          : " << RUN_BFS << "\n";
    fout << "RUN_ASTAR                        : " << RUN_ASTAR << "\n";
    fout << "RUN_BEAM                         : " << RUN_BEAM << "\n";
    fout << "RUN_BATCHER                      : " << RUN_BATCHER << "\n";
    fout << "Beam width                       : " << BEAM_WIDTH << "\n";
    fout << "Max depth                        : " << MAX_DEPTH << "\n";
    fout << "Total elapsed wall time          : " << total_elapsed << " sec\n\n";

    if constexpr (RUN_BFS) {
        fout << "[BFS True Table]\n";
        fout << "Total BFS accumulated time       : " << total_bfs_time << " sec\n";
        fout << "Average BFS time per state       : " << total_bfs_time / total_cases << " sec\n\n";
    }

    if constexpr (RUN_ASTAR) {
        fout << "[A*]\n";
        fout << "Total A* accumulated time        : " << total_astar_time << " sec\n";
        fout << "Average A* time per state        : " << total_astar_time / total_cases << " sec\n";

        if constexpr (RUN_BFS) {
            fout << "A* match BFS count               : " << astar_match_bfs << "\n";
            fout << "A* match BFS rate                : " << percent(astar_match_bfs, total_cases) << "%\n";
            fout << "A* mismatch count                : " << astar_mismatch << "\n";
        }

        fout << "\n";
    }

    if constexpr (RUN_BEAM) {
        fout << "[Beam Search]\n";
        fout << "Total Beam accumulated time      : " << total_beam_time << " sec\n";
        fout << "Average Beam time per state      : " << total_beam_time / total_cases << " sec\n";
        fout << "Beam success count               : " << beam_success << "\n";
        fout << "Beam success rate                : " << percent(beam_success, total_cases) << "%\n";
        fout << "Beam failed count                : " << beam_failed << "\n";

        if constexpr (RUN_BFS) {
            fout << "Beam optimal vs BFS count        : " << beam_match_bfs << "\n";
            fout << "Beam optimal vs BFS rate         : " << percent(beam_match_bfs, total_cases) << "%\n";
            fout << "Beam not optimal count           : " << beam_not_optimal << "\n";
        }

        fout << "\n";
    }

    if constexpr (RUN_BATCHER) {
        fout << "[Batcher's Merge Sort Baseline]\n";
        fout << "Total Batcher accumulated time   : " << total_batcher_time << " sec\n";
        fout << "Average Batcher time per state   : " << total_batcher_time / total_cases << " sec\n";
        fout << "Batcher success count            : " << batcher_success << "\n";
        fout << "Batcher success rate             : " << percent(batcher_success, total_cases) << "%\n";
        fout << "Batcher failed count             : " << batcher_failed << "\n";
        fout << "Average Batcher compares         : " << safe_div(total_batcher_compares, total_cases) << "\n";
        fout << "Average Batcher swaps            : " << safe_div(total_batcher_swaps, total_cases) << "\n";
        fout << "Average Batcher rounds           : " << safe_div(total_batcher_rounds, total_cases) << "\n";

        if constexpr (RUN_BFS) {
            fout << "Batcher optimal vs BFS count     : " << batcher_match_bfs << "\n";
            fout << "Batcher optimal vs BFS rate      : " << percent(batcher_match_bfs, total_cases) << "%\n";
            fout << "Batcher not optimal count        : " << batcher_not_optimal << "\n";
        }

        fout << "\n";
    }

    fout << "[Speedup]\n";

    if constexpr (RUN_BFS && RUN_ASTAR) {
        fout << "A* speedup over BFS              : "
             << safe_div(total_bfs_time, total_astar_time)
             << "x\n";
    }

    if constexpr (RUN_BFS && RUN_BEAM) {
        fout << "Beam speedup over BFS            : "
             << safe_div(total_bfs_time, total_beam_time)
             << "x\n";
    }

    if constexpr (RUN_ASTAR && RUN_BEAM) {
        fout << "A* speedup over Beam             : "
             << safe_div(total_beam_time, total_astar_time)
             << "x\n";
    }

    if constexpr (RUN_BFS && RUN_BATCHER) {
        fout << "Batcher speedup over BFS         : "
             << safe_div(total_bfs_time, total_batcher_time)
             << "x\n";
    }

    if constexpr (RUN_ASTAR && RUN_BATCHER) {
        fout << "A* speedup over Batcher          : "
             << safe_div(total_batcher_time, total_astar_time)
             << "x\n";
    }

    if constexpr (RUN_BEAM && RUN_BATCHER) {
        fout << "Batcher speedup over Beam        : "
             << safe_div(total_beam_time, total_batcher_time)
             << "x\n";
    }

    fout << "\n";

    fout << "========================================\n";
    fout << "Step Distribution, BFS exact baseline\n";
    fout << "========================================\n";

    for (int step = 0; step < MAX_STEP_BUCKET; ++step) {
        long long count = bfs_counter[step];

        if (count == 0) {
            continue;
        }

        fout << "Steps = " << setw(2) << step
             << " | Count = " << setw(6) << count << "\n";
    }

    fout << "\n";

    fout << "[Step Distribution Table]\n";
    fout << "steps,bfs_count,astar_count,beam_count,batcher_count\n";

    for (int step = 0; step < MAX_STEP_BUCKET; ++step) {
        if (bfs_counter[step] == 0 &&
            astar_counter[step] == 0 &&
            beam_counter[step] == 0 &&
            batcher_counter[step] == 0) {
            continue;
        }

        fout << step << ","
             << bfs_counter[step] << ","
             << astar_counter[step] << ","
             << beam_counter[step] << ","
             << batcher_counter[step] << "\n";
    }

    fout << "\n";

    fout << "[Conclusion]\n";
    fout << "This experiment exhaustively tested all 8! = 40320 permutations of Q3 hypercube token swapping.\n";
    fout << "BFS is used as the true table for optimal shortest swap counts.\n";
    fout << "Batcher's merge sort baseline follows a fixed compare-exchange sorting-network schedule and does not search for the minimum swap count.\n";

    if constexpr (RUN_BFS && RUN_ASTAR) {
        if (astar_mismatch == 0) {
            fout << "A* matched the BFS true table on all states.\n";
        } else {
            fout << "A* produced mismatches against BFS, so further validation is required.\n";
        }
    }

    if constexpr (RUN_BFS && RUN_BEAM) {
        if (beam_not_optimal == 0 && beam_failed == 0) {
            fout << "Beam Search with the selected beam width matched the BFS true table on all states in this Q3 experiment.\n";
        } else {
            fout << "Beam Search did not match the BFS true table on all states, which is expected because Beam Search is heuristic.\n";
        }
    }

    if constexpr (RUN_BFS && RUN_BATCHER) {
        fout << "Batcher baseline optimal count compared with BFS: "
             << batcher_match_bfs << " / " << total_cases << ".\n";
    }

    fout.close();
}

// ============================================================
// ASCII distribution output
// ============================================================

void print_ascii_distribution(const array<long long, MAX_STEP_BUCKET>& counter) {
    long long max_count = 0;

    for (long long c : counter) {
        max_count = max(max_count, c);
    }

    cout << "\n========================================\n";
    cout << "Step Distribution, BFS exact baseline\n";
    cout << "========================================\n";

    for (int step = 0; step < MAX_STEP_BUCKET; ++step) {
        long long count = counter[step];

        if (count == 0) {
            continue;
        }

        int bar_len = 0;
        if (max_count > 0) {
            bar_len = static_cast<int>(50.0 * count / max_count);
        }

        cout << "Steps = " << setw(2) << step
             << " | Count = " << setw(6) << count
             << " | ";

        for (int i = 0; i < bar_len; ++i) {
            cout << '#';
        }

        cout << "\n";
    }
}

// ============================================================
// Main full test with OpenMP
// ============================================================

int main() {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    const vector<Edge> edges = generate_hypercube_edges();
    const vector<uint64_t> states = generate_all_states();

    const int total_cases = static_cast<int>(states.size());

    cout << "========================================\n";
    cout << "Q3 Hypercube Full Permutation Test\n";
    cout << "========================================\n";
    cout << "Total states : " << total_cases << "\n";
    cout << "Edges        : " << edges.size() << "\n";
    cout << "OpenMP max threads : " << omp_get_max_threads() << "\n";
    cout << "RUN_BFS      : " << RUN_BFS << "\n";
    cout << "RUN_ASTAR    : " << RUN_ASTAR << "\n";
    cout << "RUN_BEAM     : " << RUN_BEAM << "\n";
    cout << "RUN_BATCHER  : " << RUN_BATCHER << "\n";
    cout << "Beam width   : " << BEAM_WIDTH << "\n";
    cout << "Max depth    : " << MAX_DEPTH << "\n\n";

    array<long long, MAX_STEP_BUCKET> bfs_counter{};
    array<long long, MAX_STEP_BUCKET> astar_counter{};
    array<long long, MAX_STEP_BUCKET> beam_counter{};
    array<long long, MAX_STEP_BUCKET> batcher_counter{};

    long long astar_match_bfs = 0;
    long long astar_mismatch = 0;

    long long beam_success = 0;
    long long beam_match_bfs = 0;
    long long beam_not_optimal = 0;
    long long beam_failed = 0;

    long long batcher_success = 0;
    long long batcher_failed = 0;
    long long batcher_match_bfs = 0;
    long long batcher_not_optimal = 0;

    long long total_batcher_compares = 0;
    long long total_batcher_swaps = 0;
    long long total_batcher_rounds = 0;

    double total_bfs_time = 0.0;
    double total_astar_time = 0.0;
    double total_beam_time = 0.0;
    double total_batcher_time = 0.0;

    double start_all = omp_get_wtime();

    int progress_counter = 0;

#pragma omp parallel
    {
        array<long long, MAX_STEP_BUCKET> local_bfs_counter{};
        array<long long, MAX_STEP_BUCKET> local_astar_counter{};
        array<long long, MAX_STEP_BUCKET> local_beam_counter{};
        array<long long, MAX_STEP_BUCKET> local_batcher_counter{};

        long long local_astar_match_bfs = 0;
        long long local_astar_mismatch = 0;

        long long local_beam_success = 0;
        long long local_beam_match_bfs = 0;
        long long local_beam_not_optimal = 0;
        long long local_beam_failed = 0;

        long long local_batcher_success = 0;
        long long local_batcher_failed = 0;
        long long local_batcher_match_bfs = 0;
        long long local_batcher_not_optimal = 0;

        long long local_total_batcher_compares = 0;
        long long local_total_batcher_swaps = 0;
        long long local_total_batcher_rounds = 0;

        double local_bfs_time = 0.0;
        double local_astar_time = 0.0;
        double local_beam_time = 0.0;
        double local_batcher_time = 0.0;

#pragma omp for schedule(dynamic, 64)
        for (int i = 0; i < total_cases; ++i) {
            uint64_t state = states[i];

            int bfs_steps = -1;
            int astar_steps = -1;
            int beam_steps = -1;

            BatcherResult batcher_result{0, 0, 0, false};

            if constexpr (RUN_BFS) {
                double t0 = omp_get_wtime();
                bfs_steps = bfs_min_steps(state, edges);
                double t1 = omp_get_wtime();

                local_bfs_time += (t1 - t0);

                if (bfs_steps >= 0 && bfs_steps < MAX_STEP_BUCKET) {
                    local_bfs_counter[bfs_steps]++;
                }
            }

            if constexpr (RUN_ASTAR) {
                double t0 = omp_get_wtime();
                astar_steps = astar_min_steps(state, edges);
                double t1 = omp_get_wtime();

                local_astar_time += (t1 - t0);

                if (astar_steps >= 0 && astar_steps < MAX_STEP_BUCKET) {
                    local_astar_counter[astar_steps]++;
                }
            }

            if constexpr (RUN_BEAM) {
                double t0 = omp_get_wtime();
                beam_steps = beam_search_steps(state, edges, BEAM_WIDTH, MAX_DEPTH);
                double t1 = omp_get_wtime();

                local_beam_time += (t1 - t0);

                if (beam_steps >= 0) {
                    local_beam_success++;

                    if (beam_steps < MAX_STEP_BUCKET) {
                        local_beam_counter[beam_steps]++;
                    }
                } else {
                    local_beam_failed++;
                }
            }

            if constexpr (RUN_BATCHER) {
                double t0 = omp_get_wtime();
                batcher_result = batcher_merge_sort_baseline(state);
                double t1 = omp_get_wtime();

                local_batcher_time += (t1 - t0);

                local_total_batcher_compares += batcher_result.compare_count;
                local_total_batcher_swaps += batcher_result.swap_count;
                local_total_batcher_rounds += batcher_result.round_count;

                if (batcher_result.success) {
                    local_batcher_success++;

                    if (batcher_result.swap_count >= 0 &&
                        batcher_result.swap_count < MAX_STEP_BUCKET) {
                        local_batcher_counter[batcher_result.swap_count]++;
                    }
                } else {
                    local_batcher_failed++;
                }
            }

            if constexpr (RUN_BFS && RUN_ASTAR) {
                if (astar_steps == bfs_steps) {
                    local_astar_match_bfs++;
                } else {
                    local_astar_mismatch++;
                }
            }

            if constexpr (RUN_BFS && RUN_BEAM) {
                if (beam_steps < 0) {
                    // Already counted as failed.
                } else if (beam_steps == bfs_steps) {
                    local_beam_match_bfs++;
                } else {
                    local_beam_not_optimal++;
                }
            }

            if constexpr (RUN_BFS && RUN_BATCHER) {
                if (!batcher_result.success) {
                    // Already counted as failed.
                } else if (batcher_result.swap_count == bfs_steps) {
                    local_batcher_match_bfs++;
                } else {
                    local_batcher_not_optimal++;
                }
            }

#pragma omp atomic
            progress_counter++;

            if (progress_counter % 5000 == 0) {
#pragma omp critical
                {
                    cout << "Checked " << progress_counter << "/"
                         << total_cases << " states...\n";
                }
            }
        }

#pragma omp critical
        {
            for (int step = 0; step < MAX_STEP_BUCKET; ++step) {
                bfs_counter[step] += local_bfs_counter[step];
                astar_counter[step] += local_astar_counter[step];
                beam_counter[step] += local_beam_counter[step];
                batcher_counter[step] += local_batcher_counter[step];
            }

            astar_match_bfs += local_astar_match_bfs;
            astar_mismatch += local_astar_mismatch;

            beam_success += local_beam_success;
            beam_match_bfs += local_beam_match_bfs;
            beam_not_optimal += local_beam_not_optimal;
            beam_failed += local_beam_failed;

            batcher_success += local_batcher_success;
            batcher_failed += local_batcher_failed;
            batcher_match_bfs += local_batcher_match_bfs;
            batcher_not_optimal += local_batcher_not_optimal;

            total_batcher_compares += local_total_batcher_compares;
            total_batcher_swaps += local_total_batcher_swaps;
            total_batcher_rounds += local_total_batcher_rounds;

            total_bfs_time += local_bfs_time;
            total_astar_time += local_astar_time;
            total_beam_time += local_beam_time;
            total_batcher_time += local_batcher_time;
        }
    }

    double end_all = omp_get_wtime();
    double total_elapsed = end_all - start_all;

    cout << "\n========================================\n";
    cout << "Full Test Result\n";
    cout << "========================================\n";
    cout << fixed << setprecision(6);
    cout << "Total elapsed wall time     : " << total_elapsed << " sec\n";

    if constexpr (RUN_BFS) {
        cout << "\n[BFS True Table]\n";
        cout << "Total BFS accumulated time  : " << total_bfs_time << " sec\n";
        cout << "Average BFS time per state  : " << total_bfs_time / total_cases << " sec\n";
    }

    if constexpr (RUN_ASTAR) {
        cout << "\n[A*]\n";
        cout << "Total A* accumulated time   : " << total_astar_time << " sec\n";
        cout << "Average A* time per state   : " << total_astar_time / total_cases << " sec\n";

        if constexpr (RUN_BFS) {
            cout << "A* match BFS count          : " << astar_match_bfs << "\n";
            cout << "A* match BFS rate           : "
                 << 100.0 * astar_match_bfs / total_cases << "%\n";
            cout << "A* mismatch count           : " << astar_mismatch << "\n";
        }
    }

    if constexpr (RUN_BEAM) {
        cout << "\n[Beam Search]\n";
        cout << "Total Beam accumulated time : " << total_beam_time << " sec\n";
        cout << "Average Beam time per state : " << total_beam_time / total_cases << " sec\n";
        cout << "Beam success count          : " << beam_success << "\n";
        cout << "Beam success rate           : "
             << 100.0 * beam_success / total_cases << "%\n";
        cout << "Beam failed count           : " << beam_failed << "\n";

        if constexpr (RUN_BFS) {
            cout << "Beam optimal vs BFS count   : " << beam_match_bfs << "\n";
            cout << "Beam optimal vs BFS rate    : "
                 << 100.0 * beam_match_bfs / total_cases << "%\n";
            cout << "Beam not optimal count      : " << beam_not_optimal << "\n";
        }
    }

    if constexpr (RUN_BATCHER) {
        cout << "\n[Batcher's Merge Sort Baseline]\n";
        cout << "Total Batcher accumulated time : " << total_batcher_time << " sec\n";
        cout << "Average Batcher time per state : " << total_batcher_time / total_cases << " sec\n";
        cout << "Batcher success count          : " << batcher_success << "\n";
        cout << "Batcher success rate           : "
             << 100.0 * batcher_success / total_cases << "%\n";
        cout << "Batcher failed count           : " << batcher_failed << "\n";
        cout << "Average Batcher compares       : "
             << static_cast<double>(total_batcher_compares) / total_cases << "\n";
        cout << "Average Batcher swaps          : "
             << static_cast<double>(total_batcher_swaps) / total_cases << "\n";
        cout << "Average Batcher rounds         : "
             << static_cast<double>(total_batcher_rounds) / total_cases << "\n";

        if constexpr (RUN_BFS) {
            cout << "Batcher optimal vs BFS count   : " << batcher_match_bfs << "\n";
            cout << "Batcher optimal vs BFS rate    : "
                 << 100.0 * batcher_match_bfs / total_cases << "%\n";
            cout << "Batcher not optimal count      : " << batcher_not_optimal << "\n";
        }
    }

    cout << "\n[Speedup]\n";

    if constexpr (RUN_BFS && RUN_ASTAR) {
        if (total_astar_time > 0.0) {
            cout << "A* speedup over BFS        : "
                 << total_bfs_time / total_astar_time
                 << "x\n";
        }
    }

    if constexpr (RUN_BFS && RUN_BEAM) {
        if (total_beam_time > 0.0) {
            cout << "Beam speedup over BFS      : "
                 << total_bfs_time / total_beam_time
                 << "x\n";
        }
    }

    if constexpr (RUN_ASTAR && RUN_BEAM) {
        if (total_astar_time > 0.0) {
            cout << "A* speedup over Beam       : "
                 << total_beam_time / total_astar_time
                 << "x\n";
        }
    }

    if constexpr (RUN_BFS && RUN_BATCHER) {
        if (total_batcher_time > 0.0) {
            cout << "Batcher speedup over BFS   : "
                 << total_bfs_time / total_batcher_time
                 << "x\n";
        }
    }

    if constexpr (RUN_ASTAR && RUN_BATCHER) {
        if (total_astar_time > 0.0) {
            cout << "A* speedup over Batcher    : "
                 << total_batcher_time / total_astar_time
                 << "x\n";
        }
    }

    if constexpr (RUN_BEAM && RUN_BATCHER) {
        if (total_batcher_time > 0.0) {
            cout << "Batcher speedup over Beam  : "
                 << total_beam_time / total_batcher_time
                 << "x\n";
        }
    }

    print_ascii_distribution(bfs_counter);

    write_distribution_csv(
        bfs_counter,
        astar_counter,
        beam_counter,
        batcher_counter,
        "step_distribution.csv"
    );

    write_text_report(
        "hypercube_report.txt",
        total_cases,
        static_cast<int>(edges.size()),
        omp_get_max_threads(),
        total_elapsed,
        total_bfs_time,
        total_astar_time,
        total_beam_time,
        total_batcher_time,
        astar_match_bfs,
        astar_mismatch,
        beam_success,
        beam_failed,
        beam_match_bfs,
        beam_not_optimal,
        batcher_success,
        batcher_failed,
        batcher_match_bfs,
        batcher_not_optimal,
        total_batcher_compares,
        total_batcher_swaps,
        total_batcher_rounds,
        bfs_counter,
        astar_counter,
        beam_counter,
        batcher_counter
    );

    cout << "\nCSV written to: step_distribution.csv\n";
    cout << "Text report written to: hypercube_report.txt\n";

    return 0;
}