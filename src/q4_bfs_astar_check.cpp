#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <queue>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace std;

namespace q4 {
constexpr int DIM = 4;
constexpr int NODES = 1 << DIM;
constexpr double DEFAULT_TIME_LIMIT_SEC = 2.0 * 60.0 * 60.0;
constexpr double DEFAULT_MEMORY_BUDGET_GIB = 20.0;
constexpr double ESTIMATED_BYTES_PER_EXACT_STATE = 128.0;
using Clock = chrono::steady_clock;

struct Edge {
    int u;
    int v;
    int bit;
};

struct SolveResult {
    int steps;
    long long expanded;
    double sec;
    string status;
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

int astar_h(uint64_t s) {
    return (total_distance(s) + 1) / 2;
}

size_t cap_from_memory_budget(double memory_budget_gib, int worker_count) {
    if (memory_budget_gib <= 0.0 || worker_count <= 0) {
        return 0;
    }

    double budget_bytes = memory_budget_gib * 1024.0 * 1024.0 * 1024.0;
    double cap = budget_bytes / (static_cast<double>(worker_count) * ESTIMATED_BYTES_PER_EXACT_STATE);
    return max<size_t>(1, static_cast<size_t>(cap));
}

SolveResult bidirectional_bfs_min_steps(
    uint64_t init,
    const vector<Edge>& es,
    size_t node_cap,
    Clock::time_point deadline,
    int bfs_threads
) {
    auto t0 = Clock::now();
    uint64_t target = target_state();
    if (init == target) {
        return {0, 0, 0.0, "solved"};
    }

    unordered_map<uint64_t, int> left_dist;
    unordered_map<uint64_t, int> right_dist;
    size_t reserve_cap = node_cap == 0 ? 2000000 : min<size_t>(node_cap, 2000000);
    left_dist.reserve(reserve_cap);
    right_dist.reserve(reserve_cap);
    left_dist.max_load_factor(0.7f);
    right_dist.max_load_factor(0.7f);

    vector<uint64_t> left_frontier{init};
    vector<uint64_t> right_frontier{target};
    left_dist[init] = 0;
    right_dist[target] = 0;

    long long expanded = 0;
    int thread_count = max(1, bfs_threads);
    while (!left_frontier.empty() && !right_frontier.empty()) {
        if (Clock::now() >= deadline) {
            auto t1 = Clock::now();
            return {-1, expanded, chrono::duration<double>(t1 - t0).count(), "time_limit"};
        }

        if (node_cap > 0 && left_dist.size() + right_dist.size() > node_cap) {
            auto t1 = Clock::now();
            return {-1, expanded, chrono::duration<double>(t1 - t0).count(), "exact_cap"};
        }

        bool expand_left = left_frontier.size() <= right_frontier.size();
        auto& frontier = expand_left ? left_frontier : right_frontier;
        auto& own_dist = expand_left ? left_dist : right_dist;
        auto& other_dist = expand_left ? right_dist : left_dist;

        const auto& own_read = own_dist;
        const auto& other_read = other_dist;
        int next_depth = own_read.find(frontier.front())->second + 1;
        int active_threads = min<int>(thread_count, static_cast<int>(frontier.size()));
        size_t chunk_size = (frontier.size() + active_threads - 1) / active_threads;

        struct LocalExpansion {
            vector<uint64_t> generated;
            int best_meet = numeric_limits<int>::max();
            long long expanded = 0;
        };

        vector<LocalExpansion> locals(active_threads);
        vector<thread> pool;
        pool.reserve(active_threads);
        atomic<bool> deadline_hit{false};

        for (int tid = 0; tid < active_threads; ++tid) {
            size_t begin = static_cast<size_t>(tid) * chunk_size;
            size_t end = min(frontier.size(), begin + chunk_size);
            size_t expected = (end - begin) * es.size();
            locals[tid].generated.reserve(min<size_t>(expected, 1000000));

            pool.emplace_back([&, tid, begin, end]() {
                auto& local = locals[tid];
                for (size_t i = begin; i < end && !deadline_hit.load(); ++i) {
                    local.expanded++;
                    if ((local.expanded & 0x3FFF) == 0 && Clock::now() >= deadline) {
                        deadline_hit.store(true);
                        break;
                    }

                    uint64_t s = frontier[i];
                    auto own_state = own_read.find(s);
                    if (own_state == own_read.end()) {
                        continue;
                    }
                    int d = own_state->second;

                    for (const auto& e : es) {
                        uint64_t ns = swap_nodes(s, e.u, e.v);
                        if (own_read.find(ns) != own_read.end()) {
                            continue;
                        }

                        auto meet = other_read.find(ns);
                        if (meet != other_read.end()) {
                            local.best_meet = min(local.best_meet, d + 1 + meet->second);
                            continue;
                        }

                        local.generated.push_back(ns);
                    }
                }
            });
        }

        for (auto& t : pool) {
            t.join();
        }

        for (const auto& local : locals) {
            expanded += local.expanded;
        }

        if (deadline_hit.load() || Clock::now() >= deadline) {
            auto t1 = Clock::now();
            return {-1, expanded, chrono::duration<double>(t1 - t0).count(), "time_limit"};
        }

        int best_meet = numeric_limits<int>::max();
        for (const auto& local : locals) {
            best_meet = min(best_meet, local.best_meet);
        }
        if (best_meet != numeric_limits<int>::max()) {
            auto t1 = Clock::now();
            return {best_meet, expanded, chrono::duration<double>(t1 - t0).count(), "solved"};
        }

        vector<uint64_t> next_frontier;
        size_t reserve_hint = 0;
        for (const auto& local : locals) {
            reserve_hint += local.generated.size();
        }
        next_frontier.reserve(min<size_t>(reserve_hint, 2000000));

        for (const auto& local : locals) {
            for (uint64_t ns : local.generated) {
                if (own_dist.find(ns) != own_dist.end()) {
                    continue;
                }

                own_dist[ns] = next_depth;
                next_frontier.push_back(ns);

                if ((own_dist.size() & 0x3FFF) == 0) {
                    if (Clock::now() >= deadline) {
                        auto t1 = Clock::now();
                        return {-1, expanded, chrono::duration<double>(t1 - t0).count(), "time_limit"};
                    }
                    if (node_cap > 0 && own_dist.size() + other_dist.size() > node_cap) {
                        auto t1 = Clock::now();
                        return {-1, expanded, chrono::duration<double>(t1 - t0).count(), "exact_cap"};
                    }
                }
            }
        }

        if (node_cap > 0 && own_dist.size() + other_dist.size() > node_cap) {
            auto t1 = Clock::now();
            return {-1, expanded, chrono::duration<double>(t1 - t0).count(), "exact_cap"};
        }

        frontier.swap(next_frontier);
    }

    auto t1 = Clock::now();
    return {-1, expanded, chrono::duration<double>(t1 - t0).count(), "exhausted"};
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

SolveResult astar_min_steps(
    uint64_t init,
    const vector<Edge>& es,
    size_t node_cap,
    Clock::time_point deadline
) {
    auto t0 = Clock::now();
    uint64_t target = target_state();
    if (init == target) {
        return {0, 0, 0.0, "solved"};
    }

    priority_queue<ANode> pq;
    unordered_map<uint64_t, int> best;
    size_t reserve_cap = node_cap == 0 ? 2000000 : min<size_t>(node_cap, 1000000) * 2;
    best.reserve(reserve_cap);
    best.max_load_factor(0.7f);

    int h0 = astar_h(init);
    pq.push({h0, h0, 0, init});
    best[init] = 0;
    long long expanded = 0;

    while (!pq.empty()) {
        ANode cur = pq.top();
        pq.pop();

        auto it = best.find(cur.s);
        if (it == best.end() || it->second != cur.g) {
            continue;
        }

        expanded++;
        if ((expanded & 0x3FFF) == 0 && Clock::now() >= deadline) {
            auto t1 = Clock::now();
            return {-1, expanded, chrono::duration<double>(t1 - t0).count(), "time_limit"};
        }

        if (node_cap > 0 && best.size() > node_cap) {
            auto t1 = Clock::now();
            return {-1, expanded, chrono::duration<double>(t1 - t0).count(), "astar_cap"};
        }

        if (cur.s == target) {
            auto t1 = Clock::now();
            return {cur.g, expanded, chrono::duration<double>(t1 - t0).count(), "solved"};
        }

        for (const auto& e : es) {
            uint64_t ns = swap_nodes(cur.s, e.u, e.v);
            int ng = cur.g + 1;
            auto old = best.find(ns);
            if (old != best.end() && ng >= old->second) {
                continue;
            }
            best[ns] = ng;
            int nh = astar_h(ns);
            pq.push({ng + nh, nh, ng, ns});
        }
    }

    auto t1 = Clock::now();
    return {-1, expanded, chrono::duration<double>(t1 - t0).count(), "exhausted"};
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

void print_progress(
    int verified,
    int attempts,
    int exact_failed,
    int mismatches,
    double elapsed_sec,
    double time_limit_sec
) {
    constexpr int width = 36;
    double ratio = time_limit_sec <= 0.0 ? 1.0 : min(1.0, elapsed_sec / time_limit_sec);
    int filled = static_cast<int>(ratio * width);

    cout << "\r[";
    for (int i = 0; i < width; ++i) {
        cout << (i < filled ? '#' : '.');
    }
    cout << "] time " << setw(5) << fixed << setprecision(0) << elapsed_sec
         << "/" << setw(5) << fixed << setprecision(0) << time_limit_sec << "s"
         << " | verified " << setw(4) << verified
         << " | attempts " << setw(5) << attempts
         << " | exact-skip " << setw(4) << exact_failed
         << " | mismatch " << setw(3) << mismatches << flush;
}
} // namespace q4

int main(int argc, char** argv) {
    using namespace q4;

    double time_limit_sec = DEFAULT_TIME_LIMIT_SEC;
    unsigned long long seed = 42;
    double memory_budget_gib = DEFAULT_MEMORY_BUDGET_GIB;
    size_t exact_cap = 0;
    size_t astar_cap = 0;
    bool exact_cap_overridden = false;
    bool astar_cap_overridden = false;
    int worker_count = 1;
    unsigned int hw_threads = thread::hardware_concurrency();
    int bfs_threads = hw_threads == 0 ? 1 : min(4, static_cast<int>(hw_threads));

    if (argc > 1) time_limit_sec = stod(argv[1]);
    if (argc > 2) worker_count = stoi(argv[2]);
    if (argc > 3) seed = stoull(argv[3]);
    if (argc > 4) {
        exact_cap = stoull(argv[4]);
        exact_cap_overridden = exact_cap > 0;
    }
    if (argc > 5) {
        astar_cap = stoull(argv[5]);
        astar_cap_overridden = astar_cap > 0;
    }
    if (argc > 6) memory_budget_gib = stod(argv[6]);
    if (argc > 7) bfs_threads = stoi(argv[7]);
    worker_count = max(1, worker_count);
    bfs_threads = max(1, bfs_threads);

    size_t auto_cap = cap_from_memory_budget(memory_budget_gib, worker_count);
    if (!exact_cap_overridden) {
        exact_cap = auto_cap;
    }
    if (!astar_cap_overridden) {
        astar_cap = auto_cap;
    }
    double estimated_exact_memory_gib =
        (static_cast<double>(exact_cap) * worker_count * ESTIMATED_BYTES_PER_EXACT_STATE) /
        (1024.0 * 1024.0 * 1024.0);

    filesystem::create_directories("output");

    const auto es = edges();
    ofstream csv("output/q4_bfs_astar_check.csv");
    csv << "sample_id,worker_id,verified_id,total_dist,state_hex,status,exact_steps,astar_steps,match,"
        << "exact_expanded,astar_expanded,exact_sec,astar_sec\n";
    const string verified_path = "output/q4_verified_cases.csv";
    bool verified_needs_header =
        !filesystem::exists(verified_path) || filesystem::file_size(verified_path) == 0;
    ofstream verified_csv(verified_path, ios::app);
    if (verified_needs_header) {
        verified_csv << "sample_id,worker_id,verified_id,total_dist,state_hex,exact_steps,astar_steps,match,"
                     << "exact_expanded,astar_expanded,exact_sec,astar_sec\n";
        verified_csv.flush();
    }

    cout << "Q4 bidirectional BFS vs A* optimality check\n";
    cout << "generator=random_permutation"
         << " (uniform shuffle over all 16! token placements)\n";
    cout << "mode=single-case-focused"
         << " (default keeps the memory budget on one candidate search)\n";
    cout << "stop_condition=time_limit_only"
         << ", time_limit_sec=" << fixed << setprecision(0) << time_limit_sec
         << ", worker_count=" << worker_count
         << ", bfs_threads=" << bfs_threads
         << ", effective_max_threads=" << (worker_count * bfs_threads)
         << ", seed=" << seed
         << ", memory_budget_gib=" << fixed << setprecision(1) << memory_budget_gib
         << ", estimated_bytes_per_state=" << fixed << setprecision(0) << ESTIMATED_BYTES_PER_EXACT_STATE
         << ", exact_cap=" << exact_cap
         << (exact_cap_overridden ? " (manual)" : " (auto)")
         << ", astar_cap=" << astar_cap
         << (astar_cap_overridden ? " (manual)" : " (auto)")
         << ", estimated_exact_memory_gib=" << fixed << setprecision(1) << estimated_exact_memory_gib << "\n";

    atomic<int> next_sample_id{0};
    atomic<int> completed_attempts{0};
    atomic<int> accepted{0};
    atomic<int> exact_failed{0};
    atomic<int> astar_failed{0};
    atomic<int> astar_skipped{0};
    atomic<int> time_limit_hits{0};
    atomic<int> mismatches{0};
    atomic<bool> time_limit_reached{false};
    mutex output_mutex;
    double sum_exact_sec = 0.0;
    double sum_astar_sec = 0.0;
    auto t0 = Clock::now();
    auto deadline = Clock::now() + chrono::milliseconds(static_cast<long long>(time_limit_sec * 1000.0));

    {
        lock_guard<mutex> lock(output_mutex);
        print_progress(accepted.load(), completed_attempts.load(), exact_failed.load(), mismatches.load(), 0.0, time_limit_sec);
    }

    auto reserve_verified_slot = [&]() -> int {
        return accepted.fetch_add(1);
    };

    auto write_progress = [&]() {
        double elapsed = chrono::duration<double>(Clock::now() - t0).count();
        print_progress(
            accepted.load(),
            completed_attempts.load(),
            exact_failed.load(),
            mismatches.load(),
            elapsed,
            time_limit_sec
        );
    };

    auto worker = [&](int worker_id) {
        while (!time_limit_reached.load()) {
            if (Clock::now() >= deadline) {
                time_limit_reached.store(true);
                break;
            }

            int sample_id = next_sample_id.fetch_add(1) + 1;
            uint64_t s = make_random_state_for_sample(seed, sample_id);
            int td = total_distance(s);
            string state = state_hex(s);

            int verified_id = -1;
            int exact_steps = -1;
            int astar_steps = -1;
            int match = 0;
            long long exact_expanded = 0;
            long long astar_expanded = 0;
            double exact_sec = 0.0;
            double astar_sec = 0.0;
            string status;

            SolveResult rexact = bidirectional_bfs_min_steps(s, es, exact_cap, deadline, bfs_threads);
            exact_steps = rexact.steps;
            exact_expanded = rexact.expanded;
            exact_sec = rexact.sec;

            if (rexact.steps < 0) {
                status = rexact.status;
                exact_failed.fetch_add(1);
                astar_skipped.fetch_add(1);
                if (rexact.status == "time_limit") {
                    time_limit_hits.fetch_add(1);
                    time_limit_reached.store(true);
                }
            } else {
                SolveResult rastar = astar_min_steps(s, es, astar_cap, deadline);
                astar_steps = rastar.steps;
                astar_expanded = rastar.expanded;
                astar_sec = rastar.sec;
                match = (rastar.steps >= 0 && rastar.steps == rexact.steps) ? 1 : 0;
                status = rastar.steps < 0 ? rastar.status : (match ? "verified" : "mismatch");

                if (rastar.steps < 0) {
                    astar_failed.fetch_add(1);
                    if (rastar.status == "time_limit") {
                        time_limit_hits.fetch_add(1);
                        time_limit_reached.store(true);
                    }
                } else {
                    if (!match) {
                        mismatches.fetch_add(1);
                    } else {
                        verified_id = reserve_verified_slot();
                    }
                }
            }

            completed_attempts.fetch_add(1);
            {
                lock_guard<mutex> lock(output_mutex);
                csv << sample_id << "," << worker_id << "," << verified_id << "," << td << "," << state << ","
                    << status << "," << exact_steps << "," << astar_steps << "," << match << ","
                    << exact_expanded << "," << astar_expanded << ","
                    << fixed << setprecision(6) << exact_sec << "," << astar_sec << "\n";
                csv.flush();
                if (status == "verified" && verified_id >= 0) {
                    sum_exact_sec += rexact.sec;
                    sum_astar_sec += astar_sec;
                    verified_csv << sample_id << "," << worker_id << "," << verified_id << "," << td << "," << state << ","
                                 << exact_steps << "," << astar_steps << "," << match << ","
                                 << exact_expanded << "," << astar_expanded << ","
                                 << fixed << setprecision(6) << exact_sec << "," << astar_sec << "\n";
                    verified_csv.flush();
                }
                write_progress();
            }
        }
    };

    vector<thread> workers;
    workers.reserve(worker_count);
    for (int i = 0; i < worker_count; ++i) {
        workers.emplace_back(worker, i);
    }
    for (auto& t : workers) {
        t.join();
    }

    if (Clock::now() >= deadline) {
        time_limit_reached.store(true);
    }

    cout << "\n";
    cout << "verified=" << accepted.load()
         << ", attempts=" << completed_attempts.load()
         << ", exact_failed=" << exact_failed.load()
         << ", astar_failed=" << astar_failed.load()
         << ", astar_skipped=" << astar_skipped.load()
         << ", time_limit_hits=" << time_limit_hits.load()
         << ", time_limit_reached=" << (time_limit_reached.load() ? 1 : 0)
         << ", mismatches=" << mismatches.load() << "\n";

    if (accepted.load() > 0) {
        cout << fixed << setprecision(6)
             << "avg_exact_sec=" << (sum_exact_sec / accepted.load())
             << ", avg_astar_sec=" << (sum_astar_sec / accepted.load()) << "\n";
    }

    cout << "CSV: output/q4_bfs_astar_check.csv\n";
    return mismatches.load() == 0 ? 0 : 1;
}
