#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <functional>
#include <iostream>
#include <limits>
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
constexpr double DEFAULT_TIME_LIMIT_SEC = 2.0 * 60.0 * 60.0;
constexpr double DEFAULT_PROOF_TIME_LIMIT_SEC = 300.0;
constexpr double DEFAULT_MEMORY_BUDGET_GIB = 24.0;
constexpr double ESTIMATED_BYTES_PER_DFS_CACHE_ENTRY = 128.0;
using Clock = chrono::steady_clock;

struct Edge {
    int id;
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

struct ProofResult {
    bool verified_optimal;
    bool found_shorter;
    int proof_depth;
    long long expanded;
    long long pruned_lb;
    long long pruned_cache;
    long long cache_writes;
    size_t cache_size;
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
    int id = 0;
    for (int u = 0; u < NODES; ++u) {
        for (int bit = 0; bit < DIM; ++bit) {
            int v = u ^ (1 << bit);
            if (u < v) {
                out.push_back({id++, u, v, bit});
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

int current_astar_h(uint64_t s) {
    return (total_distance(s) + 1) / 2;
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

int verifier_lower_bound(uint64_t s) {
    int lb = max(cycle_lower_bound(s), max_packet_distance(s));
    int parity = permutation_parity(s);
    if ((lb & 1) != parity) {
        lb++;
    }
    return lb;
}

size_t cap_from_memory_budget(double memory_budget_gib) {
    if (memory_budget_gib <= 0.0) {
        return 0;
    }
    double budget_bytes = memory_budget_gib * 1024.0 * 1024.0 * 1024.0;
    double cap = budget_bytes / ESTIMATED_BYTES_PER_DFS_CACHE_ENTRY;
    return max<size_t>(1, static_cast<size_t>(cap));
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

SolveResult astar_candidate_steps(uint64_t init, const vector<Edge>& es, size_t node_cap, Clock::time_point deadline) {
    auto t0 = Clock::now();
    uint64_t target = target_state();
    if (init == target) {
        return {0, 0, 0.0, "solved"};
    }

    priority_queue<ANode> pq;
    unordered_map<uint64_t, int> best;
    size_t reserve_cap = node_cap == 0 ? 2000000 : min<size_t>(node_cap, 2000000);
    best.reserve(reserve_cap);
    best.max_load_factor(0.7f);

    int h0 = current_astar_h(init);
    pq.push({h0, h0, 0, init});
    best[init] = 0;

    long long expanded = 0;
    while (!pq.empty()) {
        if ((expanded & 0x3FFF) == 0 && Clock::now() >= deadline) {
            auto t1 = Clock::now();
            return {-1, expanded, chrono::duration<double>(t1 - t0).count(), "astar_time_limit"};
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
            int nh = current_astar_h(ns);
            pq.push({ng + nh, nh, ng, ns});
        }
    }

    auto t1 = Clock::now();
    return {-1, expanded, chrono::duration<double>(t1 - t0).count(), "astar_exhausted"};
}

struct DfsKey {
    uint64_t state;
    unsigned char last_edge_code;

    bool operator==(const DfsKey& o) const {
        return state == o.state && last_edge_code == o.last_edge_code;
    }
};

struct DfsKeyHash {
    size_t operator()(const DfsKey& k) const {
        uint64_t x = k.state ^ (static_cast<uint64_t>(k.last_edge_code) * 0x9E3779B97F4A7C15ULL);
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33;
        return static_cast<size_t>(x);
    }
};

enum class DfsOutcome {
    FoundShorter,
    NoShorter,
    Timeout
};

class BoundedDfsVerifier {
public:
    BoundedDfsVerifier(
        const vector<Edge>& edges,
        size_t cache_cap,
        Clock::time_point deadline,
        function<void(long long, long long, long long, size_t)> progress = {}
    )
        : edges_(edges), cache_cap_(cache_cap), deadline_(deadline), progress_(std::move(progress)) {
        failed_.reserve(min<size_t>(cache_cap_, 2000000));
        failed_.max_load_factor(0.7f);
        last_progress_ = Clock::now();
    }

    ProofResult prove_no_solution_within(uint64_t init, int proof_depth) {
        auto t0 = Clock::now();
        DfsOutcome outcome = dfs(init, proof_depth, -1);
        auto t1 = Clock::now();

        ProofResult result{};
        result.verified_optimal = outcome == DfsOutcome::NoShorter;
        result.found_shorter = outcome == DfsOutcome::FoundShorter;
        result.proof_depth = proof_depth;
        result.expanded = expanded_;
        result.pruned_lb = pruned_lb_;
        result.pruned_cache = pruned_cache_;
        result.cache_writes = cache_writes_;
        result.cache_size = failed_.size();
        result.sec = chrono::duration<double>(t1 - t0).count();
        if (outcome == DfsOutcome::FoundShorter) {
            result.status = "counterexample";
        } else if (outcome == DfsOutcome::NoShorter) {
            result.status = "verified";
        } else {
            result.status = "proof_time_limit";
        }
        return result;
    }

private:
    DfsOutcome dfs(uint64_t s, int remaining, int last_edge_id) {
        if ((expanded_ & 0x3FFF) == 0 && Clock::now() >= deadline_) {
            return DfsOutcome::Timeout;
        }

        expanded_++;
        if ((expanded_ & 0xFFFFF) == 0 && progress_) {
            auto now = Clock::now();
            if (chrono::duration<double>(now - last_progress_).count() >= 1.0) {
                last_progress_ = now;
                progress_(expanded_, pruned_lb_, pruned_cache_, failed_.size());
            }
        }
        if (s == target_state()) {
            return DfsOutcome::FoundShorter;
        }

        int lb = verifier_lower_bound(s);
        if (lb > remaining) {
            pruned_lb_++;
            store_failure(s, remaining, last_edge_id);
            return DfsOutcome::NoShorter;
        }

        if (remaining == 0) {
            store_failure(s, remaining, last_edge_id);
            return DfsOutcome::NoShorter;
        }

        DfsKey key{s, static_cast<unsigned char>(last_edge_id + 1)};
        auto old = failed_.find(key);
        if (old != failed_.end() && old->second >= remaining) {
            pruned_cache_++;
            return DfsOutcome::NoShorter;
        }

        for (const auto& e : edges_) {
            if (e.id == last_edge_id) {
                continue;
            }

            uint64_t ns = swap_nodes(s, e.u, e.v);
            DfsOutcome child = dfs(ns, remaining - 1, e.id);
            if (child != DfsOutcome::NoShorter) {
                return child;
            }
        }

        store_failure(s, remaining, last_edge_id);
        return DfsOutcome::NoShorter;
    }

    void store_failure(uint64_t s, int remaining, int last_edge_id) {
        if (cache_cap_ > 0 && failed_.size() >= cache_cap_) {
            return;
        }

        DfsKey key{s, static_cast<unsigned char>(last_edge_id + 1)};
        auto it = failed_.find(key);
        if (it == failed_.end()) {
            failed_.emplace(key, static_cast<unsigned char>(remaining));
            cache_writes_++;
        } else if (it->second < remaining) {
            it->second = static_cast<unsigned char>(remaining);
        }
    }

    const vector<Edge>& edges_;
    size_t cache_cap_;
    Clock::time_point deadline_;
    function<void(long long, long long, long long, size_t)> progress_;
    Clock::time_point last_progress_;
    unordered_map<DfsKey, unsigned char, DfsKeyHash> failed_;
    long long expanded_ = 0;
    long long pruned_lb_ = 0;
    long long pruned_cache_ = 0;
    long long cache_writes_ = 0;
};

void print_progress(
    double elapsed_sec,
    double time_limit_sec,
    const string& phase,
    int sample_id,
    int astar_step,
    int proof_depth,
    double phase_limit_sec,
    int attempts,
    int candidates,
    int verified,
    int skipped_low,
    int mismatches,
    long long dfs_expanded = -1,
    long long dfs_pruned_lb = -1,
    long long dfs_pruned_cache = -1,
    long long dfs_cache_size = -1
) {
    constexpr int width = 12;
    static size_t last_len = 0;
    double ratio = time_limit_sec <= 0.0 ? 1.0 : min(1.0, elapsed_sec / time_limit_sec);
    int filled = static_cast<int>(ratio * width);

    auto compact = [](long long x) -> string {
        ostringstream out;
        if (x < 0) {
            return "-";
        }
        if (x >= 1000000) {
            out << fixed << setprecision(1) << (static_cast<double>(x) / 1000000.0) << "M";
        } else if (x >= 1000) {
            out << fixed << setprecision(1) << (static_cast<double>(x) / 1000.0) << "K";
        } else {
            out << x;
        }
        return out.str();
    };

    string short_phase = phase;
    if (short_phase == "DFS proof") short_phase = "DFS";
    if (short_phase == "proof_time_limit") short_phase = "timeout";
    if (short_phase == "skipped_low_step") short_phase = "low";
    if (short_phase.size() > 7) {
        short_phase = short_phase.substr(0, 7);
    }

    ostringstream line;
    line << "[";
    for (int i = 0; i < width; ++i) {
        line << (i < filled ? '#' : '.');
    }
    line << "] " << fixed << setprecision(0) << elapsed_sec << "/" << time_limit_sec << "s"
         << " " << short_phase
         << " s" << sample_id
         << " A" << astar_step
         << " d" << proof_depth
         << " att" << attempts
         << " cand" << candidates
         << " ok" << verified
         << " low" << skipped_low
         << " mis" << mismatches;

    if (dfs_expanded >= 0) {
        line << " exp" << compact(dfs_expanded)
             << " lb" << compact(dfs_pruned_lb)
             << " c" << compact(dfs_cache_size);
    }

    string text = line.str();
    cout << '\r' << text;
    if (last_len > text.size()) {
        cout << string(last_len - text.size(), ' ');
    }
    last_len = text.size();
    cout << flush;
}
} // namespace q4

int main(int argc, char** argv) {
    using namespace q4;

    double time_limit_sec = DEFAULT_TIME_LIMIT_SEC;
    double proof_time_limit_sec = DEFAULT_PROOF_TIME_LIMIT_SEC;
    unsigned long long seed = 42;
    int min_astar_step = 15;
    size_t astar_cap = 0;
    double memory_budget_gib = DEFAULT_MEMORY_BUDGET_GIB;
    size_t dfs_cache_cap = 0;
    bool dfs_cache_cap_overridden = false;

    if (argc > 1) time_limit_sec = stod(argv[1]);
    if (argc > 2) seed = stoull(argv[2]);
    if (argc > 3) min_astar_step = stoi(argv[3]);
    if (argc > 4) astar_cap = stoull(argv[4]);
    if (argc > 5) memory_budget_gib = stod(argv[5]);
    if (argc > 6) {
        dfs_cache_cap = stoull(argv[6]);
        dfs_cache_cap_overridden = dfs_cache_cap > 0;
    }
    if (argc > 7) proof_time_limit_sec = stod(argv[7]);

    if (!dfs_cache_cap_overridden) {
        dfs_cache_cap = cap_from_memory_budget(memory_budget_gib);
    }
    double estimated_cache_gib =
        (static_cast<double>(dfs_cache_cap) * ESTIMATED_BYTES_PER_DFS_CACHE_ENTRY) /
        (1024.0 * 1024.0 * 1024.0);

    filesystem::create_directories("output");

    const auto es = edges();
    const string run_path = "output/q4_astar_dfs_verify.csv";
    ofstream run_csv(run_path);
    run_csv << "sample_id,total_dist,state_hex,status,astar_steps,proof_depth,match,"
            << "astar_expanded,dfs_expanded,dfs_pruned_lb,dfs_pruned_cache,dfs_cache_size,"
            << "astar_sec,dfs_sec,proof_time_limit_sec\n";

    const string verified_path = "output/q4_dfs_verified_cases.csv";
    bool verified_needs_header =
        !filesystem::exists(verified_path) || filesystem::file_size(verified_path) == 0;
    ofstream verified_csv(verified_path, ios::app);
    if (verified_needs_header) {
        verified_csv << "sample_id,total_dist,state_hex,astar_steps,proof_depth,match,"
                     << "astar_expanded,dfs_expanded,dfs_pruned_lb,dfs_pruned_cache,dfs_cache_size,"
                     << "astar_sec,dfs_sec,proof_time_limit_sec\n";
        verified_csv.flush();
    }

    cout << "Q4 A* candidate + independent bounded DFS verifier\n";
    cout << "generator=random_permutation (uniform shuffle over all 16! token placements)\n";
    cout << "verifier=bounded_dfs_without_current_astar_heuristic\n";
    cout << "safe_pruning=no_immediate_inverse,cycle_lower_bound,max_token_distance,parity,memoized_failed_subproblems\n";
    cout << "time_limit_sec=" << fixed << setprecision(0) << time_limit_sec
         << ", proof_time_limit_sec=" << fixed << setprecision(0) << proof_time_limit_sec
         << ", seed=" << seed
         << ", min_astar_step=" << min_astar_step
         << ", astar_cap=" << astar_cap
         << ", memory_budget_gib=" << fixed << setprecision(1) << memory_budget_gib
         << ", dfs_cache_cap=" << dfs_cache_cap
         << (dfs_cache_cap_overridden ? " (manual)" : " (auto)")
         << ", estimated_dfs_cache_gib=" << fixed << setprecision(1) << estimated_cache_gib << "\n";

    auto t0 = Clock::now();
    auto deadline = Clock::now() + chrono::milliseconds(static_cast<long long>(time_limit_sec * 1000.0));
    int attempts = 0;
    int candidates = 0;
    int verified = 0;
    int skipped_low = 0;
    int astar_failed = 0;
    int proof_timeout = 0;
    int mismatches = 0;
    double sum_astar_sec = 0.0;
    double sum_dfs_sec = 0.0;

    print_progress(0.0, time_limit_sec, "idle", 0, -1, -1, 0.0,
                   attempts, candidates, verified, skipped_low, mismatches);

    while (Clock::now() < deadline) {
        attempts++;
        uint64_t state = make_random_state_for_sample(seed, attempts);
        int td = total_distance(state);
        string hex = state_hex(state);

        double elapsed_before_astar = chrono::duration<double>(Clock::now() - t0).count();
        print_progress(elapsed_before_astar, time_limit_sec, "A*", attempts, -1, -1, 0.0,
                       attempts - 1, candidates, verified, skipped_low, mismatches);

        SolveResult astar = astar_candidate_steps(state, es, astar_cap, deadline);
        string status = astar.status;
        int proof_depth = astar.steps > 0 ? astar.steps - 1 : -1;
        int match = 0;
        ProofResult proof{};

        if (astar.steps < 0) {
            astar_failed++;
        } else if (astar.steps < min_astar_step) {
            skipped_low++;
            status = "skipped_low_step";
        } else {
            candidates++;
            auto proof_deadline = min(
                deadline,
                Clock::now() + chrono::milliseconds(static_cast<long long>(proof_time_limit_sec * 1000.0))
            );
            double effective_proof_limit =
                chrono::duration<double>(proof_deadline - Clock::now()).count();
            double elapsed_before_proof = chrono::duration<double>(Clock::now() - t0).count();
            print_progress(elapsed_before_proof, time_limit_sec, "DFS proof", attempts, astar.steps, proof_depth,
                           max(0.0, effective_proof_limit), attempts - 1, candidates, verified, skipped_low, mismatches);
            auto proof_progress = [&](long long expanded, long long pruned_lb, long long pruned_cache, size_t cache_size) {
                double elapsed = chrono::duration<double>(Clock::now() - t0).count();
                print_progress(elapsed, time_limit_sec, "DFS proof", attempts, astar.steps, proof_depth,
                               max(0.0, chrono::duration<double>(proof_deadline - Clock::now()).count()),
                               attempts - 1, candidates, verified, skipped_low, mismatches,
                               expanded, pruned_lb, pruned_cache, static_cast<long long>(cache_size));
            };
            BoundedDfsVerifier verifier(es, dfs_cache_cap, proof_deadline, proof_progress);
            proof = verifier.prove_no_solution_within(state, proof_depth);
            status = proof.status;

            if (proof.verified_optimal) {
                verified++;
                match = 1;
                sum_astar_sec += astar.sec;
                sum_dfs_sec += proof.sec;
                verified_csv << attempts << "," << td << "," << hex << ","
                             << astar.steps << "," << proof_depth << "," << match << ","
                             << astar.expanded << "," << proof.expanded << ","
                             << proof.pruned_lb << "," << proof.pruned_cache << "," << proof.cache_size << ","
                             << fixed << setprecision(6) << astar.sec << "," << proof.sec << ","
                             << proof_time_limit_sec << "\n";
                verified_csv.flush();
            } else if (proof.found_shorter) {
                mismatches++;
            } else {
                proof_timeout++;
            }
        }

        run_csv << attempts << "," << td << "," << hex << "," << status << ","
                << astar.steps << "," << proof_depth << "," << match << ","
                << astar.expanded << "," << proof.expanded << ","
                << proof.pruned_lb << "," << proof.pruned_cache << "," << proof.cache_size << ","
                << fixed << setprecision(6) << astar.sec << "," << proof.sec << ","
                << proof_time_limit_sec << "\n";
        run_csv.flush();

        double elapsed = chrono::duration<double>(Clock::now() - t0).count();
        print_progress(elapsed, time_limit_sec, status, attempts, astar.steps, proof_depth, proof.sec,
                       attempts, candidates, verified, skipped_low, mismatches);

        if (status == "astar_time_limit") {
            break;
        }
    }

    cout << "\n";
    cout << "attempts=" << attempts
         << ", candidates=" << candidates
         << ", verified=" << verified
         << ", skipped_low=" << skipped_low
         << ", astar_failed=" << astar_failed
         << ", proof_timeout=" << proof_timeout
         << ", mismatches=" << mismatches << "\n";

    if (verified > 0) {
        cout << fixed << setprecision(6)
             << "avg_astar_sec=" << (sum_astar_sec / verified)
             << ", avg_dfs_proof_sec=" << (sum_dfs_sec / verified) << "\n";
    }

    cout << "Run CSV: " << run_path << "\n";
    cout << "Verified append CSV: " << verified_path << "\n";
    return mismatches == 0 ? 0 : 1;
}
