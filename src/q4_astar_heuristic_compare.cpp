#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <queue>
#include <random>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

using namespace std;

namespace q4 {
constexpr int DIM = 4;
constexpr int NODES = 1 << DIM;
using Clock = chrono::steady_clock;

struct Edge {
    int u;
    int v;
    int bit;
};

struct AStarResult {
    int steps;
    long long expanded;
    size_t reached_states;
    double sec;
    string status;
};

enum class HeuristicKind {
    Basic,
    Strong
};

uint64_t encode(const array<int, NODES>& a) {
    uint64_t s = 0;
    for (int i = 0; i < NODES; ++i) {
        s |= (static_cast<uint64_t>(a[i]) << (4 * i));
    }
    return s;
}

int get_packet(uint64_t s, int node) {
    return static_cast<int>((s >> (4 * node)) & 0xFULL);
}

uint64_t set_packet(uint64_t s, int node, int packet) {
    uint64_t mask = 0xFULL << (4 * node);
    s &= ~mask;
    s |= (static_cast<uint64_t>(packet) << (4 * node));
    return s;
}

uint64_t swap_nodes(uint64_t s, int u, int v) {
    int pu = get_packet(s, u);
    int pv = get_packet(s, v);
    s = set_packet(s, u, pv);
    s = set_packet(s, v, pu);
    return s;
}

uint64_t target_state() {
    array<int, NODES> a{};
    for (int i = 0; i < NODES; ++i) {
        a[i] = i;
    }
    return encode(a);
}

vector<Edge> edges() {
    vector<Edge> out;
    for (int u = 0; u < NODES; ++u) {
        for (int bit = 0; bit < DIM; ++bit) {
            int v = u ^ (1 << bit);
            if (u < v) {
                out.push_back({u, v, bit});
            }
        }
    }
    return out;
}

int hdist(int a, int b) {
    return __builtin_popcount(static_cast<unsigned>(a ^ b));
}

int total_distance(uint64_t s) {
    int total = 0;
    for (int node = 0; node < NODES; ++node) {
        total += hdist(node, get_packet(s, node));
    }
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
        if (seen[i]) {
            continue;
        }

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
            if (pi > get_packet(s, j)) {
                parity ^= 1;
            }
        }
    }
    return parity;
}

int basic_heuristic(uint64_t s) {
    return (total_distance(s) + 1) / 2;
}

int parity_adjust(uint64_t s, int lower_bound) {
    int parity = permutation_parity(s);
    if ((lower_bound & 1) != parity) {
        lower_bound++;
    }
    return lower_bound;
}

int strong_heuristic(uint64_t s) {
    int h1 = basic_heuristic(s);
    int h2 = max_packet_distance(s);
    int h3 = cycle_lower_bound(s);
    return parity_adjust(s, max({h1, h2, h3}));
}

int heuristic(uint64_t s, HeuristicKind kind) {
    if (kind == HeuristicKind::Basic) {
        return basic_heuristic(s);
    }
    return strong_heuristic(s);
}

uint64_t make_random_state(mt19937_64& rng) {
    array<int, NODES> a{};
    for (int i = 0; i < NODES; ++i) {
        a[i] = i;
    }
    shuffle(a.begin(), a.end(), rng);
    return encode(a);
}

uint64_t make_random_state_for_sample(unsigned long long seed, int sample_id) {
    constexpr unsigned long long mix_a = 0x9E3779B97F4A7C15ULL;
    constexpr unsigned long long mix_b = 0xBF58476D1CE4E5B9ULL;
    mt19937_64 rng(seed ^ (mix_a + static_cast<unsigned long long>(sample_id) * mix_b));
    return make_random_state(rng);
}

string state_hex(uint64_t s) {
    ostringstream out;
    out << "0x" << uppercase << hex << setw(16) << setfill('0') << s;
    return out.str();
}

struct ANode {
    int f;
    int h;
    int g;
    uint64_t s;

    bool operator<(const ANode& o) const {
        if (f != o.f) return f > o.f;
        if (h != o.h) return h > o.h;
        return g > o.g;
    }
};

AStarResult astar(uint64_t init, const vector<Edge>& es, HeuristicKind kind, size_t node_cap, Clock::time_point deadline) {
    auto t0 = Clock::now();
    uint64_t target = target_state();
    if (init == target) {
        return {0, 0, 1, 0.0, "solved"};
    }

    priority_queue<ANode> pq;
    unordered_map<uint64_t, int> best;
    size_t reserve_cap = node_cap == 0 ? 2000000 : min<size_t>(node_cap, 2000000);
    best.reserve(reserve_cap);
    best.max_load_factor(0.7f);

    int h0 = heuristic(init, kind);
    pq.push({h0, h0, 0, init});
    best[init] = 0;

    long long expanded = 0;
    while (!pq.empty()) {
        if ((expanded & 0x3FFF) == 0 && Clock::now() >= deadline) {
            auto t1 = Clock::now();
            return {-1, expanded, best.size(), chrono::duration<double>(t1 - t0).count(), "time_limit"};
        }

        ANode cur = pq.top();
        pq.pop();

        auto it = best.find(cur.s);
        if (it == best.end() || it->second != cur.g) {
            continue;
        }

        expanded++;
        if (node_cap > 0 && best.size() > node_cap) {
            auto t1 = Clock::now();
            return {-1, expanded, best.size(), chrono::duration<double>(t1 - t0).count(), "cap"};
        }

        if (cur.s == target) {
            auto t1 = Clock::now();
            return {cur.g, expanded, best.size(), chrono::duration<double>(t1 - t0).count(), "solved"};
        }

        for (const auto& e : es) {
            uint64_t ns = swap_nodes(cur.s, e.u, e.v);
            int ng = cur.g + 1;
            auto old = best.find(ns);
            if (old != best.end() && ng >= old->second) {
                continue;
            }
            best[ns] = ng;
            int nh = heuristic(ns, kind);
            pq.push({ng + nh, nh, ng, ns});
        }
    }

    auto t1 = Clock::now();
    return {-1, expanded, best.size(), chrono::duration<double>(t1 - t0).count(), "exhausted"};
}

void print_progress(double elapsed, double limit, int done, int target, int mismatches) {
    constexpr int width = 16;
    double ratio = limit > 0 ? min(1.0, elapsed / limit) : (target > 0 ? min(1.0, static_cast<double>(done) / target) : 1.0);
    int filled = static_cast<int>(ratio * width);

    cout << "\r[";
    for (int i = 0; i < width; ++i) {
        cout << (i < filled ? '#' : '.');
    }
    cout << "] cases " << done << "/" << target
         << " time " << fixed << setprecision(0) << elapsed << "/" << limit << "s"
         << " mismatch " << mismatches << flush;
}
} // namespace q4

int main(int argc, char** argv) {
    using namespace q4;

    int target_cases = 1000;
    unsigned long long seed = 42;
    size_t node_cap = 0;
    double time_limit_sec = 600.0;

    if (argc > 1) target_cases = stoi(argv[1]);
    if (argc > 2) seed = stoull(argv[2]);
    if (argc > 3) node_cap = stoull(argv[3]);
    if (argc > 4) time_limit_sec = stod(argv[4]);

    filesystem::create_directories("output");
    const string path = "output/q4_astar_heuristic_compare.csv";
    ofstream csv(path);
    csv << "sample_id,total_dist,state_hex,basic_status,strong_status,basic_steps,strong_steps,same_steps,"
        << "basic_expanded,strong_expanded,expanded_ratio_basic_over_strong,"
        << "basic_states,strong_states,basic_sec,strong_sec,time_ratio_basic_over_strong,"
        << "basic_h0,strong_h0\n";

    const auto es = edges();
    auto t0 = Clock::now();
    auto deadline = Clock::now() + chrono::milliseconds(static_cast<long long>(time_limit_sec * 1000.0));

    int done = 0;
    int mismatches = 0;
    int both_solved = 0;
    long long sum_basic_expanded = 0;
    long long sum_strong_expanded = 0;
    double sum_basic_sec = 0.0;
    double sum_strong_sec = 0.0;

    cout << "Q4 A* heuristic comparison\n";
    cout << "basic_h=ceil(total_hamming_distance/2)\n";
    cout << "strong_h=parity_adjust(max(basic_h,max_token_distance,cycle_lower_bound))\n";
    cout << "target_cases=" << target_cases
         << ", seed=" << seed
         << ", node_cap=" << node_cap
         << ", time_limit_sec=" << fixed << setprecision(0) << time_limit_sec << "\n";

    print_progress(0.0, time_limit_sec, done, target_cases, mismatches);

    for (int sample = 1; sample <= target_cases && Clock::now() < deadline; ++sample) {
        uint64_t s = make_random_state_for_sample(seed, sample);
        int td = total_distance(s);
        int h_basic = basic_heuristic(s);
        int h_strong = strong_heuristic(s);

        AStarResult basic = astar(s, es, HeuristicKind::Basic, node_cap, deadline);
        AStarResult strong = astar(s, es, HeuristicKind::Strong, node_cap, deadline);

        bool same_steps = basic.steps >= 0 && strong.steps >= 0 && basic.steps == strong.steps;
        if (basic.steps >= 0 && strong.steps >= 0) {
            both_solved++;
            sum_basic_expanded += basic.expanded;
            sum_strong_expanded += strong.expanded;
            sum_basic_sec += basic.sec;
            sum_strong_sec += strong.sec;
            if (!same_steps) {
                mismatches++;
            }
        }

        double expanded_ratio = strong.expanded > 0 ? static_cast<double>(basic.expanded) / strong.expanded : 0.0;
        double time_ratio = strong.sec > 0.0 ? basic.sec / strong.sec : 0.0;

        csv << sample << "," << td << "," << state_hex(s) << ","
            << basic.status << "," << strong.status << ","
            << basic.steps << "," << strong.steps << "," << (same_steps ? 1 : 0) << ","
            << basic.expanded << "," << strong.expanded << ","
            << fixed << setprecision(6) << expanded_ratio << ","
            << basic.reached_states << "," << strong.reached_states << ","
            << basic.sec << "," << strong.sec << "," << time_ratio << ","
            << h_basic << "," << h_strong << "\n";
        csv.flush();

        done++;
        double elapsed = chrono::duration<double>(Clock::now() - t0).count();
        print_progress(elapsed, time_limit_sec, done, target_cases, mismatches);

        if (basic.status == "time_limit" || strong.status == "time_limit") {
            break;
        }
    }

    cout << "\n";
    cout << "done=" << done
         << ", both_solved=" << both_solved
         << ", step_mismatches=" << mismatches << "\n";
    if (both_solved > 0) {
        cout << fixed << setprecision(6)
             << "avg_basic_expanded=" << (static_cast<double>(sum_basic_expanded) / both_solved)
             << ", avg_strong_expanded=" << (static_cast<double>(sum_strong_expanded) / both_solved)
             << ", expanded_ratio_basic_over_strong="
             << (sum_strong_expanded > 0 ? static_cast<double>(sum_basic_expanded) / sum_strong_expanded : 0.0)
             << "\n"
             << "avg_basic_sec=" << (sum_basic_sec / both_solved)
             << ", avg_strong_sec=" << (sum_strong_sec / both_solved)
             << ", time_ratio_basic_over_strong="
             << (sum_strong_sec > 0.0 ? sum_basic_sec / sum_strong_sec : 0.0) << "\n";
    }
    cout << "CSV: " << path << "\n";
    return mismatches == 0 ? 0 : 1;
}
