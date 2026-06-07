#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <queue>
#include <sqlite3.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <malloc.h>
#include <windows.h>
#ifdef far
#undef far
#endif
#ifdef near
#undef near
#endif
#endif

using namespace std;

namespace qk {
using Clock = chrono::steady_clock;
using State = vector<int>;

struct Edge {
    int u;
    int v;
    int bit;
};

struct SwapStep {
    int u;
    int v;
};

struct SpecialCase {
    string name;
    string note;
    State state;
};

struct PathResult {
    int steps = -1;
    long long expanded = 0;
    size_t reached_states = 0;
    double sec = 0.0;
    string status = "skipped";
    vector<SwapStep> swaps;
};

struct BatcherResult {
    bool success = false;
    int swaps = 0;
    int compares = 0;
    int rounds = 0;
    double sec = 0.0;
    vector<SwapStep> path;
};

enum class CandidateTraceMode {
    None = 0,
    RetainedOnly = 1,
    AllNewCandidates = 2
};

enum class BeamVisitedMode {
    ExactPacked = 0,
    Fingerprint128 = 1,
    Fingerprint128Disk = 2
};

struct ProgressContext {
    bool enabled = false;
    int case_index = 0;
    int total_cases = 0;
    int dim = 0;
    string case_name;
};

struct PackedStateKey {
    array<uint64_t, 32> words{};

    bool operator==(const PackedStateKey& other) const {
        return words == other.words;
    }
};

struct PackedStateKeyHash {
    size_t operator()(const PackedStateKey& key) const {
        uint64_t h = 0x9E3779B97F4A7C15ULL;
        for (uint64_t word : key.words) {
            h ^= word + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
        }
        return static_cast<size_t>(h);
    }
};

uint64_t splitmix64(uint64_t value) {
    value += 0x9E3779B97F4A7C15ULL;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31);
}

struct Fingerprint128 {
    uint64_t low = 0;
    uint64_t high = 0;

    bool operator==(const Fingerprint128& other) const {
        return low == other.low && high == other.high;
    }
};

struct Fingerprint128Hash {
    size_t operator()(const Fingerprint128& key) const {
        return static_cast<size_t>(
            splitmix64(key.low ^ (key.high + 0x9E3779B97F4A7C15ULL))
        );
    }
};

static_assert(sizeof(Fingerprint128) == 16, "Fingerprint128 must be exactly 16 bytes");

bool fingerprint_less(const Fingerprint128& a, const Fingerprint128& b) {
    return tie(a.high, a.low) < tie(b.high, b.low);
}

bool packed_key_less(const PackedStateKey& a, const PackedStateKey& b) {
    for (size_t word_index = 0; word_index < a.words.size(); ++word_index) {
        uint64_t aw = a.words[word_index];
        uint64_t bw = b.words[word_index];
        if (aw == bw) {
            continue;
        }
        for (int byte_index = 0; byte_index < 8; ++byte_index) {
            unsigned av = static_cast<unsigned>((aw >> (byte_index * 8)) & 0xffu);
            unsigned bv = static_cast<unsigned>((bw >> (byte_index * 8)) & 0xffu);
            if (av != bv) {
                return av < bv;
            }
        }
    }
    return false;
}

struct ParentInfo {
    string prev;
    SwapStep swap;
};

int node_count(int dim) {
    return 1 << dim;
}

State target_state(int n) {
    State s(n);
    iota(s.begin(), s.end(), 0);
    return s;
}

string key_from_state(const State& s) {
    string key;
    key.reserve(s.size());
    for (int token : s) {
        key.push_back(static_cast<char>(token));
    }
    return key;
}

PackedStateKey packed_key_from_state(const State& s) {
    if (s.size() > 256) {
        throw runtime_error("packed state key supports up to Q8 states only");
    }

    PackedStateKey key;
    for (size_t i = 0; i < s.size(); ++i) {
        uint64_t token = static_cast<uint64_t>(static_cast<unsigned>(s[i]) & 0xffu);
        key.words[i / 8] |= token << ((i % 8) * 8);
    }
    return key;
}

Fingerprint128 fingerprint_component(int position, int token) {
    uint64_t pair = (static_cast<uint64_t>(static_cast<uint32_t>(position)) << 32) |
                    static_cast<uint32_t>(token);
    return {
        splitmix64(pair ^ 0x243F6A8885A308D3ULL),
        splitmix64(pair ^ 0x13198A2E03707344ULL)
    };
}

Fingerprint128 fingerprint_from_state(const State& s) {
    Fingerprint128 fingerprint;
    for (size_t position = 0; position < s.size(); ++position) {
        Fingerprint128 component = fingerprint_component(
            static_cast<int>(position),
            s[position]
        );
        fingerprint.low ^= component.low;
        fingerprint.high ^= component.high;
    }
    return fingerprint;
}

Fingerprint128 fingerprint_after_swap(
    Fingerprint128 fingerprint,
    int u,
    int v,
    int token_u,
    int token_v
) {
    Fingerprint128 old_u = fingerprint_component(u, token_u);
    Fingerprint128 old_v = fingerprint_component(v, token_v);
    Fingerprint128 new_u = fingerprint_component(u, token_v);
    Fingerprint128 new_v = fingerprint_component(v, token_u);
    fingerprint.low ^= old_u.low ^ old_v.low ^ new_u.low ^ new_v.low;
    fingerprint.high ^= old_u.high ^ old_v.high ^ new_u.high ^ new_v.high;
    return fingerprint;
}

class BloomFilter {
public:
    explicit BloomFilter(size_t memory_mb) {
        size_t bytes = max<size_t>(memory_mb, 1) * 1024ULL * 1024ULL;
        words_.assign(max<size_t>(bytes / sizeof(uint64_t), 1), 0);
        bit_count_ = words_.size() * 64ULL;
    }

    bool possibly_contains(const Fingerprint128& fingerprint) const {
        uint64_t h1 = splitmix64(fingerprint.low ^ 0xA4093822299F31D0ULL);
        uint64_t h2 = splitmix64(fingerprint.high ^ 0x082EFA98EC4E6C89ULL) | 1ULL;
        for (int i = 0; i < hash_count_; ++i) {
            size_t bit = static_cast<size_t>((h1 + static_cast<uint64_t>(i) * h2) % bit_count_);
            if ((words_[bit / 64] & (1ULL << (bit % 64))) == 0) {
                return false;
            }
        }
        return true;
    }

    void add(const Fingerprint128& fingerprint) {
        uint64_t h1 = splitmix64(fingerprint.low ^ 0xA4093822299F31D0ULL);
        uint64_t h2 = splitmix64(fingerprint.high ^ 0x082EFA98EC4E6C89ULL) | 1ULL;
        for (int i = 0; i < hash_count_; ++i) {
            size_t bit = static_cast<size_t>((h1 + static_cast<uint64_t>(i) * h2) % bit_count_);
            words_[bit / 64] |= 1ULL << (bit % 64);
        }
    }

private:
    static constexpr int hash_count_ = 7;
    vector<uint64_t> words_;
    size_t bit_count_ = 0;
};

class DiskFingerprintVisited {
public:
    DiskFingerprintVisited(
        const filesystem::path& database_path,
        size_t bloom_mb,
        size_t batch_size,
        size_t sqlite_cache_mb
    )
        : database_path_(database_path),
          bloom_(bloom_mb),
          batch_size_(max<size_t>(batch_size, 1)) {
        filesystem::create_directories(database_path_.parent_path());
        filesystem::remove(database_path_);
        filesystem::remove(database_path_.string() + "-journal");
        filesystem::remove(database_path_.string() + "-wal");
        filesystem::remove(database_path_.string() + "-shm");

        string database_utf8 = database_path_.u8string();
        int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX;
        if (sqlite3_open_v2(database_utf8.c_str(), &db_, flags, nullptr) != SQLITE_OK) {
            string message = db_ != nullptr ? sqlite3_errmsg(db_) : "unknown SQLite open error";
            close_noexcept();
            throw runtime_error("cannot open Beam visited database: " + message);
        }

        sqlite3_busy_timeout(db_, 30000);
        exec("PRAGMA journal_mode=OFF;");
        exec("PRAGMA synchronous=OFF;");
        exec("PRAGMA locking_mode=EXCLUSIVE;");
        exec("PRAGMA temp_store=MEMORY;");
        exec(
            (
                "PRAGMA cache_size=-" +
                to_string(max<size_t>(sqlite_cache_mb, 1) * 1024ULL) +
                ";"
            ).c_str()
        );
        exec(
            (
                "PRAGMA mmap_size=" +
                to_string(max<size_t>(sqlite_cache_mb, 1) * 1024ULL * 1024ULL) +
                ";"
            ).c_str()
        );
        exec("CREATE TABLE seen (fingerprint BLOB PRIMARY KEY) WITHOUT ROWID;");

        prepare(
            "SELECT 1 FROM seen WHERE fingerprint=?1 LIMIT 1;",
            &contains_statement_
        );
        prepare(
            "INSERT INTO seen(fingerprint) VALUES(?1);",
            &insert_statement_
        );

        pending_.reserve(batch_size_);
        pending_order_.reserve(batch_size_);
    }

    DiskFingerprintVisited(const DiskFingerprintVisited&) = delete;
    DiskFingerprintVisited& operator=(const DiskFingerprintVisited&) = delete;

    ~DiskFingerprintVisited() {
        try {
            flush();
        } catch (...) {
        }
        close_noexcept();
    }

    bool insert_if_new(const Fingerprint128& fingerprint) {
        bool maybe_present = bloom_.possibly_contains(fingerprint);
        if (maybe_present) {
            if (pending_.find(fingerprint) != pending_.end()) {
                return false;
            }
            if (database_contains(fingerprint)) {
                return false;
            }
        }

        auto inserted = pending_.insert(fingerprint);
        if (!inserted.second) {
            return false;
        }
        pending_order_.push_back(fingerprint);
        bloom_.add(fingerprint);
        total_size_++;

        if (pending_order_.size() >= batch_size_) {
            flush();
        }
        return true;
    }

    void flush() {
        if (pending_order_.empty()) {
            return;
        }

        exec("BEGIN IMMEDIATE;");
        bool committed = false;
        try {
            for (const Fingerprint128& fingerprint : pending_order_) {
                bind_fingerprint(insert_statement_, fingerprint);
                int rc = sqlite3_step(insert_statement_);
                if (rc != SQLITE_DONE) {
                    throw_sqlite("cannot insert Beam fingerprint", rc);
                }
                sqlite3_reset(insert_statement_);
                sqlite3_clear_bindings(insert_statement_);
            }
            exec("COMMIT;");
            committed = true;
        } catch (...) {
            sqlite3_reset(insert_statement_);
            sqlite3_clear_bindings(insert_statement_);
            if (!committed) {
                sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            }
            throw;
        }

        pending_.clear();
        pending_order_.clear();
    }

    size_t size() const {
        return total_size_;
    }

    size_t pending_size() const {
        return pending_order_.size();
    }

    uintmax_t disk_bytes() const {
        error_code ec;
        uintmax_t bytes = filesystem::file_size(database_path_, ec);
        return ec ? 0 : bytes;
    }

private:
    void exec(const char* sql) {
        char* error = nullptr;
        int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &error);
        if (rc != SQLITE_OK) {
            string message = error != nullptr ? error : sqlite3_errmsg(db_);
            sqlite3_free(error);
            throw runtime_error("SQLite Beam visited error: " + message);
        }
    }

    void prepare(const char* sql, sqlite3_stmt** statement) {
        int rc = sqlite3_prepare_v2(db_, sql, -1, statement, nullptr);
        if (rc != SQLITE_OK) {
            throw_sqlite("cannot prepare Beam visited statement", rc);
        }
    }

    void bind_fingerprint(sqlite3_stmt* statement, const Fingerprint128& fingerprint) {
        int rc = sqlite3_bind_blob(
            statement,
            1,
            &fingerprint,
            static_cast<int>(sizeof(fingerprint)),
            SQLITE_TRANSIENT
        );
        if (rc != SQLITE_OK) {
            throw_sqlite("cannot bind Beam fingerprint", rc);
        }
    }

    bool database_contains(const Fingerprint128& fingerprint) {
        bind_fingerprint(contains_statement_, fingerprint);
        int rc = sqlite3_step(contains_statement_);
        bool found = rc == SQLITE_ROW;
        if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
            sqlite3_reset(contains_statement_);
            sqlite3_clear_bindings(contains_statement_);
            throw_sqlite("cannot query Beam fingerprint", rc);
        }
        sqlite3_reset(contains_statement_);
        sqlite3_clear_bindings(contains_statement_);
        return found;
    }

    [[noreturn]] void throw_sqlite(const string& prefix, int rc) const {
        ostringstream message;
        message << prefix << " (rc=" << rc << "): "
                << (db_ != nullptr ? sqlite3_errmsg(db_) : "database unavailable");
        throw runtime_error(message.str());
    }

    void close_noexcept() {
        if (contains_statement_ != nullptr) {
            sqlite3_finalize(contains_statement_);
            contains_statement_ = nullptr;
        }
        if (insert_statement_ != nullptr) {
            sqlite3_finalize(insert_statement_);
            insert_statement_ = nullptr;
        }
        if (db_ != nullptr) {
            sqlite3_close_v2(db_);
            db_ = nullptr;
        }
    }

    filesystem::path database_path_;
    BloomFilter bloom_;
    size_t batch_size_;
    sqlite3* db_ = nullptr;
    sqlite3_stmt* contains_statement_ = nullptr;
    sqlite3_stmt* insert_statement_ = nullptr;
    unordered_set<Fingerprint128, Fingerprint128Hash> pending_;
    vector<Fingerprint128> pending_order_;
    size_t total_size_ = 0;
};

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
    return __builtin_popcount(static_cast<unsigned>(a ^ b));
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

string state_perm(const State& s) {
    ostringstream out;
    for (int i = 0; i < static_cast<int>(s.size()); ++i) {
        if (i > 0) {
            out << ' ';
        }
        out << s[i];
    }
    return out.str();
}

string path_string(const vector<SwapStep>& swaps) {
    ostringstream out;
    for (size_t i = 0; i < swaps.size(); ++i) {
        if (i > 0) {
            out << ' ';
        }
        out << swaps[i].u << "-" << swaps[i].v;
    }
    return out.str();
}

string progress_bar(long long current, long long total, int width = 24) {
    if (total <= 0) {
        total = 1;
    }
    current = max<long long>(0, min(current, total));
    int filled = static_cast<int>((static_cast<double>(current) / total) * width);

    string out = "[";
    for (int i = 0; i < width; ++i) {
        out.push_back(i < filled ? '#' : '.');
    }
    out += "]";
    return out;
}

void print_progress_line(
    const ProgressContext& progress,
    const string& phase,
    long long current,
    long long total,
    long long expanded
) {
    if (!progress.enabled) {
        return;
    }

    double percent = total > 0 ? (100.0 * current / total) : 0.0;
    cout << "\r" << progress_bar(current, total)
         << " case " << progress.case_index << "/" << progress.total_cases
         << " Q" << progress.dim << " " << progress.case_name
         << " " << phase << " " << current << "/" << total
         << " " << fixed << setprecision(1) << percent << "%"
         << " expanded=" << expanded << flush;
}

void clear_progress_line() {
    cout << "\r" << string(180, ' ') << "\r" << flush;
}

void trim_process_memory() {
#ifdef _WIN32
    _heapmin();
    SetProcessWorkingSetSize(GetCurrentProcess(), static_cast<SIZE_T>(-1), static_cast<SIZE_T>(-1));
#endif
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

vector<SwapStep> reconstruct_path(
    const string& init_key,
    const string& goal_key,
    const unordered_map<string, ParentInfo>& parent
) {
    vector<SwapStep> path;
    if (init_key == goal_key) {
        return path;
    }

    string cur = goal_key;
    while (cur != init_key) {
        auto it = parent.find(cur);
        if (it == parent.end()) {
            return {};
        }
        path.push_back(it->second.swap);
        cur = it->second.prev;
    }

    reverse(path.begin(), path.end());
    return path;
}

struct ANode {
    int f;
    int h;
    int g;
    State state;
    string key;

    bool operator<(const ANode& other) const {
        if (f != other.f) {
            return f > other.f;
        }
        if (h != other.h) {
            return h > other.h;
        }
        return g > other.g;
    }
};

PathResult astar_exact(
    const State& init,
    const vector<Edge>& es,
    size_t node_cap,
    Clock::time_point deadline,
    bool record_path,
    const ProgressContext& progress = {},
    double time_limit_sec = 0.0
) {
    auto t0 = Clock::now();
    State goal = target_state(static_cast<int>(init.size()));
    string init_key = key_from_state(init);
    string goal_key = key_from_state(goal);

    if (init_key == goal_key) {
        return {0, 0, 1, 0.0, "solved", {}};
    }

    priority_queue<ANode> pq;
    unordered_map<string, int> best;
    size_t reserve_cap = node_cap == 0 ? 500000 : min<size_t>(node_cap, 500000);
    best.reserve(reserve_cap);
    best.max_load_factor(0.7f);

    unordered_map<string, ParentInfo> parent;
    if (record_path) {
        parent.reserve(reserve_cap);
        parent.max_load_factor(0.7f);
    }

    int h0 = strong_lower_bound(init);
    pq.push({h0, h0, 0, init, init_key});
    best[init_key] = 0;

    long long expanded = 0;
    while (!pq.empty()) {
        if ((expanded & 0x3FF) == 0) {
            auto now = Clock::now();
            if (progress.enabled && time_limit_sec > 0.0) {
                long long elapsed_ms = chrono::duration_cast<chrono::milliseconds>(now - t0).count();
                long long total_ms = max<long long>(1, static_cast<long long>(time_limit_sec * 1000.0));
                print_progress_line(progress, "strong_astar_time", elapsed_ms, total_ms, expanded);
            }
            if (now >= deadline) {
                auto t1 = Clock::now();
                return {-1, expanded, best.size(), chrono::duration<double>(t1 - t0).count(), "time_limit", {}};
            }
        }

        ANode cur = pq.top();
        pq.pop();

        auto current_best = best.find(cur.key);
        if (current_best == best.end() || current_best->second != cur.g) {
            continue;
        }

        expanded++;
        if (node_cap > 0 && best.size() > node_cap) {
            auto t1 = Clock::now();
            return {-1, expanded, best.size(), chrono::duration<double>(t1 - t0).count(), "cap", {}};
        }

        if (cur.key == goal_key) {
            auto t1 = Clock::now();
            vector<SwapStep> path = record_path ? reconstruct_path(init_key, goal_key, parent) : vector<SwapStep>{};
            return {cur.g, expanded, best.size(), chrono::duration<double>(t1 - t0).count(), "solved", path};
        }

        for (const auto& e : es) {
            State ns = cur.state;
            swap(ns[e.u], ns[e.v]);
            string nk = key_from_state(ns);
            int ng = cur.g + 1;
            auto old = best.find(nk);
            if (old != best.end() && ng >= old->second) {
                continue;
            }

            best[nk] = ng;
            if (record_path) {
                parent[nk] = {cur.key, {e.u, e.v}};
            }
            int nh = strong_lower_bound(ns);
            pq.push({ng + nh, nh, ng, move(ns), move(nk)});
        }
    }

    auto t1 = Clock::now();
    return {-1, expanded, best.size(), chrono::duration<double>(t1 - t0).count(), "exhausted", {}};
}

struct BeamItem {
    State state;
    int last_edge = -1;
    int path_node = -1;
    Fingerprint128 fingerprint;
};

struct BeamPathNode {
    int parent = -1;
    SwapStep swap{-1, -1};
};

struct BeamCandidate {
    int total_dist = 0;
    int misplaced = 0;
    int max_dist = 0;
    int depth = 0;
    State state;
    int edge_id = -1;
    int parent_path_node = -1;
    PackedStateKey packed_key;
    Fingerprint128 fingerprint;
    bool has_packed_key = false;
    long long order = 0;
};

vector<SwapStep> reconstruct_beam_path(
    const vector<BeamPathNode>& path_nodes,
    int path_node
) {
    vector<SwapStep> path;
    while (path_node >= 0) {
        if (path_node >= static_cast<int>(path_nodes.size())) {
            throw runtime_error("invalid Beam path back-pointer");
        }
        const BeamPathNode& node = path_nodes[path_node];
        path.push_back(node.swap);
        path_node = node.parent;
    }
    reverse(path.begin(), path.end());
    return path;
}

string beam_path_string_with_step(
    const vector<BeamPathNode>& path_nodes,
    int parent_path_node,
    SwapStep step
) {
    vector<SwapStep> path = reconstruct_beam_path(path_nodes, parent_path_node);
    path.push_back(step);
    return path_string(path);
}

bool candidate_better(const BeamCandidate& a, const BeamCandidate& b) {
    if (a.total_dist != b.total_dist) {
        return a.total_dist < b.total_dist;
    }
    if (a.max_dist != b.max_dist) {
        return a.max_dist < b.max_dist;
    }
    if (a.misplaced != b.misplaced) {
        return a.misplaced < b.misplaced;
    }
    if (a.depth != b.depth) {
        return a.depth < b.depth;
    }
    if (a.edge_id != b.edge_id) {
        return a.edge_id < b.edge_id;
    }
    if (a.has_packed_key && b.has_packed_key && !(a.packed_key == b.packed_key)) {
        return packed_key_less(a.packed_key, b.packed_key);
    }
    if (!(a.fingerprint == b.fingerprint)) {
        return fingerprint_less(a.fingerprint, b.fingerprint);
    }
    return a.order < b.order;
}

struct BeamWorstFirst {
    bool operator()(const BeamCandidate& a, const BeamCandidate& b) const {
        return candidate_better(a, b);
    }
};

void write_beam_depth_progress(
    ofstream* csv,
    const ProgressContext& progress,
    int depth,
    int max_depth,
    long long expanded,
    long long layer_candidates,
    size_t retained,
    const vector<BeamCandidate>& selected,
    double elapsed_sec
) {
    if (csv == nullptr) {
        return;
    }

    int best_total = selected.empty() ? -1 : selected.front().total_dist;
    int best_misplaced = selected.empty() ? -1 : selected.front().misplaced;
    int best_max_dist = selected.empty() ? -1 : selected.front().max_dist;

    *csv << progress.case_index << "," << progress.total_cases << ","
         << progress.dim << "," << csv_escape(progress.case_name) << ","
         << depth << "," << max_depth << ","
         << expanded << "," << layer_candidates << "," << retained << ","
         << best_total << "," << best_misplaced << "," << best_max_dist << ","
         << fixed << setprecision(6) << elapsed_sec << "\n";
    csv->flush();
}

void write_beam_candidate_trace(
    ofstream* csv,
    const ProgressContext& progress,
    const string& trace_kind,
    int depth,
    long long candidate_id,
    const BeamCandidate& cand,
    const State& state,
    const string& path
) {
    if (csv == nullptr) {
        return;
    }

    *csv << progress.case_index << "," << progress.total_cases << ","
         << progress.dim << "," << csv_escape(progress.case_name) << ","
         << trace_kind << "," << depth << "," << candidate_id << ","
         << cand.total_dist << "," << cand.misplaced << "," << cand.max_dist << ","
         << cand.edge_id << "," << depth << ","
         << csv_escape(state_perm(state)) << ","
         << csv_escape(path) << "\n";
}

PathResult beam_search(
    const State& init,
    const vector<Edge>& es,
    int beam_width,
    int max_depth,
    bool record_path,
    const ProgressContext& progress = {},
    ofstream* depth_progress_csv = nullptr,
    ofstream* candidate_trace_csv = nullptr,
    CandidateTraceMode trace_mode = CandidateTraceMode::None,
    BeamVisitedMode visited_mode = BeamVisitedMode::ExactPacked
) {
    auto t0 = Clock::now();
    int n = static_cast<int>(init.size());
    State goal = target_state(n);
    if (candidate_trace_csv == nullptr) {
        trace_mode = CandidateTraceMode::None;
    }

    if (init == goal) {
        return {0, 0, 1, 0.0, "solved", {}};
    }
    if (visited_mode == BeamVisitedMode::ExactPacked && n > 256) {
        throw runtime_error("exact packed Beam visited supports up to Q8; use fingerprint128 for Q9");
    }

    bool use_packed_tie_key = n <= 256;
    Fingerprint128 initial_fingerprint;
    if (visited_mode == BeamVisitedMode::Fingerprint128) {
        initial_fingerprint = fingerprint_from_state(init);
    }

    vector<BeamItem> beam = {{init, -1, -1, initial_fingerprint}};
    vector<BeamPathNode> path_nodes;
    bool keep_backpointers = record_path || trace_mode != CandidateTraceMode::None;
    if (keep_backpointers) {
        size_t path_node_reserve = static_cast<size_t>(max(beam_width, 1)) *
                                   static_cast<size_t>(max(max_depth, 1));
        path_nodes.reserve(path_node_reserve);
    }
    unordered_set<PackedStateKey, PackedStateKeyHash> exact_seen;
    unordered_set<Fingerprint128, Fingerprint128Hash> fingerprint_seen;
    size_t reserve_hint = static_cast<size_t>(max(beam_width, 1)) *
                          static_cast<size_t>(max(1, min(max_depth, 1024)));
    reserve_hint = max<size_t>(reserve_hint, 1024);
    if (visited_mode == BeamVisitedMode::ExactPacked) {
        exact_seen.reserve(reserve_hint);
        exact_seen.insert(packed_key_from_state(init));
    } else {
        fingerprint_seen.reserve(reserve_hint);
        fingerprint_seen.insert(initial_fingerprint);
    }

    auto reached_state_count = [&]() -> size_t {
        return visited_mode == BeamVisitedMode::ExactPacked
            ? exact_seen.size()
            : fingerprint_seen.size();
    };

    long long expanded = 0;
    long long candidate_order = 0;
    for (int depth = 1; depth <= max_depth; ++depth) {
        print_progress_line(progress, "beam_depth", depth, max_depth, expanded);

        priority_queue<BeamCandidate, vector<BeamCandidate>, BeamWorstFirst> top_candidates;
        long long layer_candidates = 0;

        for (const auto& item : beam) {
            for (int eid = 0; eid < static_cast<int>(es.size()); ++eid) {
                if (eid == item.last_edge) {
                    continue;
                }

                const auto& e = es[eid];
                int token_u = item.state[e.u];
                int token_v = item.state[e.v];
                State ns = item.state;
                swap(ns[e.u], ns[e.v]);

                PackedStateKey packed_key;
                Fingerprint128 fingerprint = item.fingerprint;
                bool inserted = false;
                if (visited_mode == BeamVisitedMode::ExactPacked) {
                    packed_key = packed_key_from_state(ns);
                    inserted = exact_seen.insert(packed_key).second;
                } else {
                    fingerprint = fingerprint_after_swap(
                        item.fingerprint,
                        e.u,
                        e.v,
                        token_u,
                        token_v
                    );
                    inserted = fingerprint_seen.insert(fingerprint).second;
                    if (use_packed_tie_key) {
                        packed_key = packed_key_from_state(ns);
                    }
                }
                if (!inserted) {
                    continue;
                }

                expanded++;
                layer_candidates++;
                candidate_order++;

                if (ns == goal) {
                    auto t1 = Clock::now();
                    vector<SwapStep> next_path;
                    if (record_path) {
                        next_path = reconstruct_beam_path(path_nodes, item.path_node);
                        next_path.push_back({e.u, e.v});
                    }
                    vector<BeamCandidate> solved_selected = {{
                        0,
                        0,
                        0,
                        depth,
                        {},
                        eid,
                        item.path_node,
                        packed_key,
                        fingerprint,
                        use_packed_tie_key,
                        candidate_order
                    }};
                    if (trace_mode == CandidateTraceMode::AllNewCandidates) {
                        write_beam_candidate_trace(
                            candidate_trace_csv,
                            progress,
                            "new",
                            depth,
                            candidate_order,
                            solved_selected.front(),
                            goal,
                            beam_path_string_with_step(path_nodes, item.path_node, {e.u, e.v})
                        );
                        candidate_trace_csv->flush();
                    }
                    double elapsed = chrono::duration<double>(t1 - t0).count();
                    write_beam_depth_progress(
                        depth_progress_csv,
                        progress,
                        depth,
                        max_depth,
                        expanded,
                        layer_candidates,
                        1,
                        solved_selected,
                        elapsed
                    );
                    return {depth, expanded, reached_state_count(), elapsed, "solved", next_path};
                }

                BeamCandidate cand{
                    total_distance(ns),
                    misplaced_count(ns),
                    max_packet_distance(ns),
                    depth,
                    move(ns),
                    eid,
                    item.path_node,
                    move(packed_key),
                    fingerprint,
                    use_packed_tie_key,
                    candidate_order
                };

                if (trace_mode == CandidateTraceMode::AllNewCandidates) {
                    write_beam_candidate_trace(
                        candidate_trace_csv,
                        progress,
                        "new",
                        depth,
                        candidate_order,
                        cand,
                        cand.state,
                        beam_path_string_with_step(path_nodes, item.path_node, {e.u, e.v})
                    );
                    if ((candidate_order % 10000) == 0) {
                        candidate_trace_csv->flush();
                    }
                }

                if (static_cast<int>(top_candidates.size()) >= beam_width && !candidate_better(cand, top_candidates.top())) {
                    continue;
                }

                if (static_cast<int>(top_candidates.size()) >= beam_width) {
                    top_candidates.pop();
                }

                top_candidates.push(move(cand));
            }
        }

        if (top_candidates.empty()) {
            write_beam_depth_progress(
                depth_progress_csv,
                progress,
                depth,
                max_depth,
                expanded,
                layer_candidates,
                0,
                {},
                chrono::duration<double>(Clock::now() - t0).count()
            );
            break;
        }

        vector<BeamCandidate> selected;
        selected.reserve(top_candidates.size());
        while (!top_candidates.empty()) {
            selected.push_back(top_candidates.top());
            top_candidates.pop();
        }

        sort(selected.begin(), selected.end(), candidate_better);

        vector<int> selected_path_nodes(selected.size(), -1);
        if (keep_backpointers) {
            for (size_t i = 0; i < selected.size(); ++i) {
                const Edge& edge = es[selected[i].edge_id];
                path_nodes.push_back({
                    selected[i].parent_path_node,
                    {edge.u, edge.v}
                });
                selected_path_nodes[i] = static_cast<int>(path_nodes.size() - 1);
            }
        }

        if (trace_mode == CandidateTraceMode::RetainedOnly) {
            for (size_t i = 0; i < selected.size(); ++i) {
                write_beam_candidate_trace(
                    candidate_trace_csv,
                    progress,
                    "retained",
                    depth,
                    static_cast<long long>(i + 1),
                    selected[i],
                    selected[i].state,
                    path_string(reconstruct_beam_path(path_nodes, selected_path_nodes[i]))
                );
            }
            candidate_trace_csv->flush();
        }

        write_beam_depth_progress(
            depth_progress_csv,
            progress,
            depth,
            max_depth,
            expanded,
            layer_candidates,
            selected.size(),
            selected,
            chrono::duration<double>(Clock::now() - t0).count()
        );

        int keep = static_cast<int>(selected.size());
        beam.clear();
        beam.reserve(keep);
        for (int i = 0; i < keep; ++i) {
            beam.push_back({
                move(selected[i].state),
                selected[i].edge_id,
                selected_path_nodes[i],
                selected[i].fingerprint
            });
        }
    }

    auto t1 = Clock::now();
    return {
        -1,
        expanded,
        reached_state_count(),
        chrono::duration<double>(t1 - t0).count(),
        "failed",
        {}
    };
}

struct DiskBeamItem {
    State state;
    int last_edge = -1;
    int path_node = -1;
    Fingerprint128 fingerprint;
    int total_dist = 0;
    int misplaced = 0;
    array<int, 16> distance_hist{};
};

struct DiskGeneratedCandidate {
    bool valid = false;
    int total_dist = 0;
    int misplaced = 0;
    int max_dist = 0;
    int parent_index = -1;
    int edge_id = -1;
    PackedStateKey packed_key;
    Fingerprint128 fingerprint;
    bool has_packed_key = false;
};

class ShardedDiskFingerprintVisited {
public:
    ShardedDiskFingerprintVisited(
        const filesystem::path& database_path,
        int shard_count,
        size_t total_bloom_mb,
        size_t total_batch_size,
        size_t total_sqlite_cache_mb = 256
    ) {
        int count = max(shard_count, 1);
        size_t bloom_per_shard = max<size_t>(total_bloom_mb / count, 1);
        size_t batch_per_shard = max<size_t>(total_batch_size / count, 1);
        size_t cache_per_shard = max<size_t>(total_sqlite_cache_mb / count, 1);

        filesystem::path parent = database_path.parent_path();
        string stem = database_path.stem().string();
        string extension = database_path.extension().string();
        for (int shard = 0; shard < count; ++shard) {
            ostringstream suffix;
            suffix << ".part" << setfill('0') << setw(2) << shard;
            filesystem::path shard_path =
                parent / (stem + suffix.str() + extension);
            shards_.push_back(make_unique<DiskFingerprintVisited>(
                shard_path,
                bloom_per_shard,
                batch_per_shard,
                cache_per_shard
            ));
        }
    }

    bool insert_if_new(const Fingerprint128& fingerprint) {
        return shards_[shard_for(fingerprint)]->insert_if_new(fingerprint);
    }

    void mark_new_parallel(
        const vector<DiskGeneratedCandidate>& generated,
        size_t processing_limit,
        int worker_threads,
        vector<uint8_t>& is_new
    ) {
        is_new.assign(generated.size(), 0);
        processing_limit = min(processing_limit, generated.size());
        vector<vector<size_t>> shard_indices(shards_.size());
        for (size_t index = 0; index < processing_limit; ++index) {
            if (generated[index].valid) {
                shard_indices[shard_for(generated[index].fingerprint)].push_back(index);
            }
        }

        atomic<size_t> next_shard{0};
        mutex error_mutex;
        exception_ptr worker_error;
        int active_threads = min<int>(
            max(worker_threads, 1),
            static_cast<int>(shards_.size())
        );
        auto worker = [&]() {
            try {
                while (true) {
                    size_t shard = next_shard.fetch_add(1, memory_order_relaxed);
                    if (shard >= shards_.size()) {
                        break;
                    }
                    DiskFingerprintVisited& visited = *shards_[shard];
                    for (size_t index : shard_indices[shard]) {
                        is_new[index] = visited.insert_if_new(
                            generated[index].fingerprint
                        ) ? 1 : 0;
                    }
                }
            } catch (...) {
                lock_guard<mutex> lock(error_mutex);
                if (worker_error == nullptr) {
                    worker_error = current_exception();
                }
            }
        };

        if (active_threads == 1) {
            worker();
            if (worker_error != nullptr) {
                rethrow_exception(worker_error);
            }
            return;
        }

        vector<thread> workers;
        workers.reserve(active_threads);
        for (int i = 0; i < active_threads; ++i) {
            workers.emplace_back(worker);
        }
        for (thread& worker_thread : workers) {
            worker_thread.join();
        }
        if (worker_error != nullptr) {
            rethrow_exception(worker_error);
        }
    }

    void flush(int worker_threads) {
        atomic<size_t> next_shard{0};
        mutex error_mutex;
        exception_ptr worker_error;
        int active_threads = min<int>(
            max(worker_threads, 1),
            static_cast<int>(shards_.size())
        );
        auto worker = [&]() {
            try {
                while (true) {
                    size_t shard = next_shard.fetch_add(1, memory_order_relaxed);
                    if (shard >= shards_.size()) {
                        break;
                    }
                    shards_[shard]->flush();
                }
            } catch (...) {
                lock_guard<mutex> lock(error_mutex);
                if (worker_error == nullptr) {
                    worker_error = current_exception();
                }
            }
        };

        if (active_threads == 1) {
            worker();
            if (worker_error != nullptr) {
                rethrow_exception(worker_error);
            }
            return;
        }

        vector<thread> workers;
        workers.reserve(active_threads);
        for (int i = 0; i < active_threads; ++i) {
            workers.emplace_back(worker);
        }
        for (thread& worker_thread : workers) {
            worker_thread.join();
        }
        if (worker_error != nullptr) {
            rethrow_exception(worker_error);
        }
    }

    size_t size() const {
        size_t total = 0;
        for (const auto& shard : shards_) {
            total += shard->size();
        }
        return total;
    }

    size_t pending_size() const {
        size_t total = 0;
        for (const auto& shard : shards_) {
            total += shard->pending_size();
        }
        return total;
    }

    uintmax_t disk_bytes() const {
        uintmax_t total = 0;
        for (const auto& shard : shards_) {
            total += shard->disk_bytes();
        }
        return total;
    }

private:
    size_t shard_for(const Fingerprint128& fingerprint) const {
        uint64_t hash = splitmix64(
            fingerprint.low ^
            (fingerprint.high + 0x9E3779B97F4A7C15ULL)
        );
        return static_cast<size_t>(hash % shards_.size());
    }

    vector<unique_ptr<DiskFingerprintVisited>> shards_;
};

struct DiskBeamCandidate {
    int total_dist = 0;
    int misplaced = 0;
    int max_dist = 0;
    int depth = 0;
    int parent_index = -1;
    int edge_id = -1;
    PackedStateKey packed_key;
    Fingerprint128 fingerprint;
    bool has_packed_key = false;
    long long order = 0;
};

bool disk_candidate_better(const DiskBeamCandidate& a, const DiskBeamCandidate& b) {
    if (a.total_dist != b.total_dist) {
        return a.total_dist < b.total_dist;
    }
    if (a.max_dist != b.max_dist) {
        return a.max_dist < b.max_dist;
    }
    if (a.misplaced != b.misplaced) {
        return a.misplaced < b.misplaced;
    }
    if (a.depth != b.depth) {
        return a.depth < b.depth;
    }
    if (a.edge_id != b.edge_id) {
        return a.edge_id < b.edge_id;
    }
    if (a.has_packed_key && b.has_packed_key && !(a.packed_key == b.packed_key)) {
        return packed_key_less(a.packed_key, b.packed_key);
    }
    if (!(a.fingerprint == b.fingerprint)) {
        return fingerprint_less(a.fingerprint, b.fingerprint);
    }
    return a.order < b.order;
}

struct DiskBeamWorstFirst {
    bool operator()(const DiskBeamCandidate& a, const DiskBeamCandidate& b) const {
        return disk_candidate_better(a, b);
    }
};

array<int, 16> distance_histogram(const State& state) {
    array<int, 16> histogram{};
    for (int node = 0; node < static_cast<int>(state.size()); ++node) {
        histogram[hdist(node, state[node])]++;
    }
    return histogram;
}

int histogram_max_distance(const array<int, 16>& histogram) {
    for (int distance = static_cast<int>(histogram.size()) - 1; distance >= 0; --distance) {
        if (histogram[distance] > 0) {
            return distance;
        }
    }
    return 0;
}

void generate_disk_candidates_parallel(
    const vector<DiskBeamItem>& beam,
    const vector<Edge>& es,
    int worker_threads,
    bool use_packed_tie_key,
    vector<DiskGeneratedCandidate>& generated
) {
    size_t edge_count = es.size();
    generated.clear();
    generated.resize(beam.size() * edge_count);

    atomic<size_t> next_parent{0};
    int active_threads = min<int>(
        max(worker_threads, 1),
        max<int>(static_cast<int>(beam.size()), 1)
    );

    auto worker = [&]() {
        while (true) {
            size_t parent_index = next_parent.fetch_add(1, memory_order_relaxed);
            if (parent_index >= beam.size()) {
                break;
            }

            const DiskBeamItem& item = beam[parent_index];
            size_t base = parent_index * edge_count;
            for (int eid = 0; eid < static_cast<int>(edge_count); ++eid) {
                if (eid == item.last_edge) {
                    continue;
                }

                const Edge& edge = es[eid];
                int token_u = item.state[edge.u];
                int token_v = item.state[edge.v];
                int old_u_dist = hdist(edge.u, token_u);
                int old_v_dist = hdist(edge.v, token_v);
                int new_u_dist = hdist(edge.u, token_v);
                int new_v_dist = hdist(edge.v, token_u);

                array<int, 16> histogram = item.distance_hist;
                histogram[old_u_dist]--;
                histogram[old_v_dist]--;
                histogram[new_u_dist]++;
                histogram[new_v_dist]++;

                DiskGeneratedCandidate candidate;
                candidate.valid = true;
                candidate.total_dist =
                    item.total_dist - old_u_dist - old_v_dist + new_u_dist + new_v_dist;
                candidate.misplaced =
                    item.misplaced -
                    (token_u != edge.u) -
                    (token_v != edge.v) +
                    (token_v != edge.u) +
                    (token_u != edge.v);
                candidate.max_dist = histogram_max_distance(histogram);
                candidate.parent_index = static_cast<int>(parent_index);
                candidate.edge_id = eid;
                candidate.fingerprint = fingerprint_after_swap(
                    item.fingerprint,
                    edge.u,
                    edge.v,
                    token_u,
                    token_v
                );
                candidate.has_packed_key = use_packed_tie_key;
                if (use_packed_tie_key) {
                    State state = item.state;
                    swap(state[edge.u], state[edge.v]);
                    candidate.packed_key = packed_key_from_state(state);
                }
                generated[base + static_cast<size_t>(eid)] = move(candidate);
            }
        }
    };

    if (active_threads == 1) {
        worker();
        return;
    }

    vector<thread> workers;
    workers.reserve(active_threads);
    for (int i = 0; i < active_threads; ++i) {
        workers.emplace_back(worker);
    }
    for (thread& worker_thread : workers) {
        worker_thread.join();
    }
}

BeamCandidate materialize_disk_candidate(
    const DiskBeamCandidate& candidate,
    const vector<DiskBeamItem>& beam,
    const vector<Edge>& es
) {
    const DiskBeamItem& parent = beam[candidate.parent_index];
    const Edge& edge = es[candidate.edge_id];
    State state = parent.state;
    swap(state[edge.u], state[edge.v]);

    BeamCandidate materialized;
    materialized.total_dist = candidate.total_dist;
    materialized.misplaced = candidate.misplaced;
    materialized.max_dist = candidate.max_dist;
    materialized.depth = candidate.depth;
    materialized.state = move(state);
    materialized.edge_id = candidate.edge_id;
    materialized.parent_path_node = parent.path_node;
    materialized.packed_key = candidate.packed_key;
    materialized.fingerprint = candidate.fingerprint;
    materialized.has_packed_key = candidate.has_packed_key;
    materialized.order = candidate.order;
    return materialized;
}

void write_beam_disk_progress(
    ofstream* csv,
    const ProgressContext& progress,
    int depth,
    size_t visited_states,
    size_t pending_states,
    uintmax_t disk_bytes,
    int worker_threads,
    double generation_sec,
    double visited_sec,
    double selection_sec
) {
    if (csv == nullptr) {
        return;
    }
    *csv << progress.case_index << "," << progress.total_cases << ","
         << progress.dim << "," << csv_escape(progress.case_name) << ","
         << depth << "," << visited_states << "," << pending_states << ","
         << disk_bytes << "," << worker_threads << ","
         << fixed << setprecision(6) << generation_sec << ","
         << visited_sec << "," << selection_sec << "\n";
    csv->flush();
}

PathResult beam_search_disk(
    const State& init,
    const vector<Edge>& es,
    int beam_width,
    int max_depth,
    bool record_path,
    int worker_threads,
    size_t bloom_mb,
    size_t disk_batch_size,
    int disk_shards,
    const filesystem::path& visited_database_path,
    const ProgressContext& progress = {},
    ofstream* depth_progress_csv = nullptr,
    ofstream* candidate_trace_csv = nullptr,
    ofstream* disk_progress_csv = nullptr,
    CandidateTraceMode trace_mode = CandidateTraceMode::None
) {
    auto t0 = Clock::now();
    int n = static_cast<int>(init.size());
    State goal = target_state(n);
    if (candidate_trace_csv == nullptr) {
        trace_mode = CandidateTraceMode::None;
    }
    if (init == goal) {
        return {0, 0, 1, 0.0, "solved", {}};
    }

    bool use_packed_tie_key = n <= 256;
    Fingerprint128 initial_fingerprint = fingerprint_from_state(init);
    ShardedDiskFingerprintVisited visited(
        visited_database_path,
        disk_shards,
        bloom_mb,
        disk_batch_size
    );
    visited.insert_if_new(initial_fingerprint);

    DiskBeamItem initial_item;
    initial_item.state = init;
    initial_item.fingerprint = initial_fingerprint;
    initial_item.total_dist = total_distance(init);
    initial_item.misplaced = misplaced_count(init);
    initial_item.distance_hist = distance_histogram(init);
    vector<DiskBeamItem> beam = {move(initial_item)};

    vector<BeamPathNode> path_nodes;
    bool keep_backpointers = record_path || trace_mode != CandidateTraceMode::None;
    if (keep_backpointers) {
        size_t path_node_reserve = static_cast<size_t>(max(beam_width, 1)) *
                                   static_cast<size_t>(max(max_depth, 1));
        path_nodes.reserve(path_node_reserve);
    }

    vector<DiskGeneratedCandidate> generated;
    vector<uint8_t> is_new;
    long long expanded = 0;
    long long candidate_order = 0;

    for (int depth = 1; depth <= max_depth; ++depth) {
        print_progress_line(progress, "beam_disk_depth", depth, max_depth, expanded);

        auto generation_start = Clock::now();
        generate_disk_candidates_parallel(
            beam,
            es,
            worker_threads,
            use_packed_tie_key,
            generated
        );
        auto generation_end = Clock::now();

        size_t processing_limit = generated.size();
        for (size_t index = 0; index < generated.size(); ++index) {
            if (generated[index].valid && generated[index].total_dist == 0) {
                processing_limit = index + 1;
                break;
            }
        }

        auto visited_start = generation_end;
        visited.mark_new_parallel(
            generated,
            processing_limit,
            worker_threads,
            is_new
        );
        auto visited_end = Clock::now();

        auto selection_start = visited_end;
        priority_queue<
            DiskBeamCandidate,
            vector<DiskBeamCandidate>,
            DiskBeamWorstFirst
        > top_candidates;
        long long layer_candidates = 0;

        for (size_t parent_index = 0; parent_index < beam.size(); ++parent_index) {
            size_t base = parent_index * es.size();
            for (int eid = 0; eid < static_cast<int>(es.size()); ++eid) {
                const DiskGeneratedCandidate& generated_candidate =
                    generated[base + static_cast<size_t>(eid)];
                if (!generated_candidate.valid) {
                    continue;
                }
                if (is_new[base + static_cast<size_t>(eid)] == 0) {
                    continue;
                }

                expanded++;
                layer_candidates++;
                candidate_order++;

                DiskBeamCandidate candidate;
                candidate.total_dist = generated_candidate.total_dist;
                candidate.misplaced = generated_candidate.misplaced;
                candidate.max_dist = generated_candidate.max_dist;
                candidate.depth = depth;
                candidate.parent_index = generated_candidate.parent_index;
                candidate.edge_id = generated_candidate.edge_id;
                candidate.packed_key = generated_candidate.packed_key;
                candidate.fingerprint = generated_candidate.fingerprint;
                candidate.has_packed_key = generated_candidate.has_packed_key;
                candidate.order = candidate_order;

                if (candidate.total_dist == 0) {
                    const DiskBeamItem& parent = beam[candidate.parent_index];
                    const Edge& edge = es[candidate.edge_id];
                    vector<SwapStep> next_path;
                    if (record_path) {
                        next_path = reconstruct_beam_path(path_nodes, parent.path_node);
                        next_path.push_back({edge.u, edge.v});
                    }

                    BeamCandidate solved = materialize_disk_candidate(candidate, beam, es);
                    vector<BeamCandidate> solved_selected = {move(solved)};
                    if (trace_mode == CandidateTraceMode::AllNewCandidates) {
                        write_beam_candidate_trace(
                            candidate_trace_csv,
                            progress,
                            "new",
                            depth,
                            candidate_order,
                            solved_selected.front(),
                            goal,
                            beam_path_string_with_step(
                                path_nodes,
                                parent.path_node,
                                {edge.u, edge.v}
                            )
                        );
                        candidate_trace_csv->flush();
                    }

                    visited.flush(worker_threads);
                    double elapsed = chrono::duration<double>(Clock::now() - t0).count();
                    write_beam_depth_progress(
                        depth_progress_csv,
                        progress,
                        depth,
                        max_depth,
                        expanded,
                        layer_candidates,
                        1,
                        solved_selected,
                        elapsed
                    );
                    write_beam_disk_progress(
                        disk_progress_csv,
                        progress,
                        depth,
                        visited.size(),
                        visited.pending_size(),
                        visited.disk_bytes(),
                        worker_threads,
                        chrono::duration<double>(generation_end - generation_start).count(),
                        chrono::duration<double>(visited_end - visited_start).count(),
                        chrono::duration<double>(Clock::now() - selection_start).count()
                    );
                    return {
                        depth,
                        expanded,
                        visited.size(),
                        elapsed,
                        "solved",
                        next_path
                    };
                }

                if (trace_mode == CandidateTraceMode::AllNewCandidates) {
                    BeamCandidate materialized = materialize_disk_candidate(candidate, beam, es);
                    const DiskBeamItem& parent = beam[candidate.parent_index];
                    const Edge& edge = es[candidate.edge_id];
                    write_beam_candidate_trace(
                        candidate_trace_csv,
                        progress,
                        "new",
                        depth,
                        candidate_order,
                        materialized,
                        materialized.state,
                        beam_path_string_with_step(
                            path_nodes,
                            parent.path_node,
                            {edge.u, edge.v}
                        )
                    );
                    if ((candidate_order % 10000) == 0) {
                        candidate_trace_csv->flush();
                    }
                }

                if (
                    static_cast<int>(top_candidates.size()) >= beam_width &&
                    !disk_candidate_better(candidate, top_candidates.top())
                ) {
                    continue;
                }
                if (static_cast<int>(top_candidates.size()) >= beam_width) {
                    top_candidates.pop();
                }
                top_candidates.push(move(candidate));
            }
        }

        auto selection_end = Clock::now();
        if (top_candidates.empty()) {
            write_beam_depth_progress(
                depth_progress_csv,
                progress,
                depth,
                max_depth,
                expanded,
                layer_candidates,
                0,
                {},
                chrono::duration<double>(Clock::now() - t0).count()
            );
            break;
        }

        vector<DiskBeamCandidate> selected_disk;
        selected_disk.reserve(top_candidates.size());
        while (!top_candidates.empty()) {
            selected_disk.push_back(top_candidates.top());
            top_candidates.pop();
        }
        sort(selected_disk.begin(), selected_disk.end(), disk_candidate_better);

        vector<BeamCandidate> selected;
        selected.reserve(selected_disk.size());
        for (const DiskBeamCandidate& candidate : selected_disk) {
            selected.push_back(materialize_disk_candidate(candidate, beam, es));
        }

        vector<int> selected_path_nodes(selected.size(), -1);
        if (keep_backpointers) {
            for (size_t i = 0; i < selected.size(); ++i) {
                const Edge& edge = es[selected[i].edge_id];
                path_nodes.push_back({
                    selected[i].parent_path_node,
                    {edge.u, edge.v}
                });
                selected_path_nodes[i] = static_cast<int>(path_nodes.size() - 1);
            }
        }

        if (trace_mode == CandidateTraceMode::RetainedOnly) {
            for (size_t i = 0; i < selected.size(); ++i) {
                write_beam_candidate_trace(
                    candidate_trace_csv,
                    progress,
                    "retained",
                    depth,
                    static_cast<long long>(i + 1),
                    selected[i],
                    selected[i].state,
                    path_string(reconstruct_beam_path(path_nodes, selected_path_nodes[i]))
                );
            }
            candidate_trace_csv->flush();
        }

        write_beam_depth_progress(
            depth_progress_csv,
            progress,
            depth,
            max_depth,
            expanded,
            layer_candidates,
            selected.size(),
            selected,
            chrono::duration<double>(Clock::now() - t0).count()
        );
        write_beam_disk_progress(
            disk_progress_csv,
            progress,
            depth,
            visited.size(),
            visited.pending_size(),
            visited.disk_bytes(),
            worker_threads,
            chrono::duration<double>(generation_end - generation_start).count(),
            chrono::duration<double>(visited_end - visited_start).count(),
            chrono::duration<double>(selection_end - selection_start).count()
        );

        vector<DiskBeamItem> next_beam;
        next_beam.reserve(selected.size());
        for (size_t i = 0; i < selected.size(); ++i) {
            DiskBeamItem item;
            item.state = move(selected[i].state);
            item.last_edge = selected[i].edge_id;
            item.path_node = selected_path_nodes[i];
            item.fingerprint = selected[i].fingerprint;
            item.total_dist = selected[i].total_dist;
            item.misplaced = selected[i].misplaced;
            item.distance_hist = distance_histogram(item.state);
            next_beam.push_back(move(item));
        }
        beam = move(next_beam);
    }

    visited.flush(worker_threads);
    auto t1 = Clock::now();
    return {
        -1,
        expanded,
        visited.size(),
        chrono::duration<double>(t1 - t0).count(),
        "failed",
        {}
    };
}

BatcherResult batcher_baseline(const State& init, bool record_path) {
    auto t0 = Clock::now();
    State s = init;
    int n = static_cast<int>(s.size());
    if (s == target_state(n)) {
        auto t1 = Clock::now();
        return {
            true,
            0,
            0,
            0,
            chrono::duration<double>(t1 - t0).count(),
            {}
        };
    }

    int compares = 0;
    int swaps = 0;
    int rounds = 0;
    vector<SwapStep> path;

    for (int k = 2; k <= n; k <<= 1) {
        for (int j = k >> 1; j > 0; j >>= 1) {
            rounds++;
            for (int i = 0; i < n; ++i) {
                int p = i ^ j;
                if (p <= i) {
                    continue;
                }
                bool asc = ((i & k) == 0);
                bool should_swap = asc ? (s[i] > s[p]) : (s[i] < s[p]);
                compares++;
                if (should_swap) {
                    swap(s[i], s[p]);
                    swaps++;
                    if (record_path) {
                        path.push_back({i, p});
                    }
                }
            }
        }
    }

    auto t1 = Clock::now();
    return {
        s == target_state(n),
        swaps,
        compares,
        rounds,
        chrono::duration<double>(t1 - t0).count(),
        path
    };
}

int reverse_bits(int x, int dim) {
    int out = 0;
    for (int bit = 0; bit < dim; ++bit) {
        if (x & (1 << bit)) {
            out |= 1 << (dim - 1 - bit);
        }
    }
    return out;
}

int rotate_left_bits(int x, int dim) {
    int mask = (1 << dim) - 1;
    return ((x << 1) & mask) | (x >> (dim - 1));
}

State make_pair_swap(int dim, int a, int b) {
    State s = target_state(node_count(dim));
    swap(s[a], s[b]);
    return s;
}

State make_matching_swap(int dim, int bit) {
    int n = node_count(dim);
    State s = target_state(n);
    for (int u = 0; u < n; ++u) {
        int v = u ^ (1 << bit);
        if (u < v) {
            swap(s[u], s[v]);
        }
    }
    return s;
}

State make_bitwise_complement(int dim) {
    int n = node_count(dim);
    int mask = n - 1;
    State s(n);
    for (int node = 0; node < n; ++node) {
        s[node] = node ^ mask;
    }
    return s;
}

State make_bit_reversal(int dim) {
    int n = node_count(dim);
    State s(n);
    for (int node = 0; node < n; ++node) {
        s[node] = reverse_bits(node, dim);
    }
    return s;
}

State make_coordinate_rotation(int dim) {
    int n = node_count(dim);
    State s(n);
    for (int node = 0; node < n; ++node) {
        s[node] = rotate_left_bits(node, dim);
    }
    return s;
}

State make_gray_cycle_shift(int dim) {
    int n = node_count(dim);
    vector<int> gray(n);
    for (int i = 0; i < n; ++i) {
        gray[i] = i ^ (i >> 1);
    }

    State s(n);
    for (int i = 0; i < n; ++i) {
        s[gray[i]] = gray[(i + 1) % n];
    }
    return s;
}

State make_integer_cycle_shift(int dim) {
    int n = node_count(dim);
    State s(n);
    for (int node = 0; node < n; ++node) {
        s[node] = (node + 1) % n;
    }
    return s;
}

State make_within_halves_complement(int dim) {
    int n = node_count(dim);
    int half_mask = (1 << max(0, dim - 1)) - 1;
    State s(n);
    for (int node = 0; node < n; ++node) {
        s[node] = (node & ~half_mask) | ((node & half_mask) ^ half_mask);
    }
    return s;
}

vector<SpecialCase> make_cases(int dim) {
    int n = node_count(dim);
    int far = n - 1;

    vector<SpecialCase> cases;
    cases.push_back({"identity", "already solved control case", target_state(n)});
    cases.push_back({"adjacent_swap_0_1", "one legal edge swap from the goal", make_pair_swap(dim, 0, 1)});
    cases.push_back({"antipodal_swap_0_allones", "two tokens swapped across maximum hypercube distance", make_pair_swap(dim, 0, far)});
    cases.push_back({"dimension0_matching_swap", "all tokens swapped across bit 0 edges", make_matching_swap(dim, 0)});
    cases.push_back({"highest_bit_matching_swap", "all tokens swapped across the highest-bit cut", make_matching_swap(dim, dim - 1)});
    cases.push_back({"bitwise_complement_all_pairs", "every token is at its antipodal vertex", make_bitwise_complement(dim)});
    cases.push_back({"bit_reversal_labels", "coordinate order is reversed", make_bit_reversal(dim)});
    cases.push_back({"coordinate_rotation_labels", "coordinate labels are cyclically rotated left", make_coordinate_rotation(dim)});
    cases.push_back({"gray_cycle_shift", "tokens follow one binary-reflected Gray-code cycle", make_gray_cycle_shift(dim)});
    cases.push_back({"integer_cycle_shift_plus_one", "tokens are shifted by one in integer label order", make_integer_cycle_shift(dim)});
    if (dim >= 2) {
        cases.push_back({"within_halves_complement", "each half-cube is complemented internally", make_within_halves_complement(dim)});
    }

    return cases;
}

string trim(const string& value) {
    size_t first = 0;
    while (first < value.size() && static_cast<unsigned char>(value[first]) <= ' ') {
        first++;
    }

    size_t last = value.size();
    while (last > first && static_cast<unsigned char>(value[last - 1]) <= ' ') {
        last--;
    }

    return value.substr(first, last - first);
}

vector<string> parse_csv_line(const string& line) {
    vector<string> fields;
    string current;
    bool in_quotes = false;

    for (size_t i = 0; i < line.size(); ++i) {
        char ch = line[i];
        if (ch == '"') {
            if (in_quotes && i + 1 < line.size() && line[i + 1] == '"') {
                current.push_back('"');
                i++;
            } else {
                in_quotes = !in_quotes;
            }
        } else if (ch == ',' && !in_quotes) {
            fields.push_back(trim(current));
            current.clear();
        } else {
            current.push_back(ch);
        }
    }

    if (in_quotes) {
        throw runtime_error("unterminated quoted CSV field");
    }

    fields.push_back(trim(current));
    return fields;
}

State parse_permutation(const string& text) {
    State state;
    istringstream in(text);
    int value = 0;
    while (in >> value) {
        state.push_back(value);
    }
    return state;
}

void validate_permutation(int dim, const State& state, int line_no) {
    int n = node_count(dim);
    if (static_cast<int>(state.size()) != n) {
        ostringstream msg;
        msg << "line " << line_no << ": Q" << dim << " requires " << n
            << " numbers, got " << state.size();
        throw runtime_error(msg.str());
    }

    vector<char> seen(n, 0);
    for (int token : state) {
        if (token < 0 || token >= n) {
            ostringstream msg;
            msg << "line " << line_no << ": token out of range for Q" << dim << ": " << token;
            throw runtime_error(msg.str());
        }
        if (seen[token]) {
            ostringstream msg;
            msg << "line " << line_no << ": duplicate token " << token;
            throw runtime_error(msg.str());
        }
        seen[token] = 1;
    }
}

map<int, vector<SpecialCase>> read_custom_cases(const filesystem::path& path) {
    ifstream in(path);
    if (!in) {
        throw runtime_error("cannot open custom case file: " + path.string());
    }

    map<int, vector<SpecialCase>> cases_by_dim;
    string line;
    int line_no = 0;

    while (getline(in, line)) {
        line_no++;
        string stripped = trim(line);
        if (stripped.empty() || stripped[0] == '#') {
            continue;
        }
        if (stripped == "dim,name,permutation") {
            continue;
        }

        vector<string> fields = parse_csv_line(line);
        if (fields.size() != 3) {
            ostringstream msg;
            msg << "line " << line_no << ": expected 3 CSV fields, got " << fields.size();
            throw runtime_error(msg.str());
        }

        int dim = stoi(fields[0]);
        if (dim < 1 || dim > 9) {
            ostringstream msg;
            msg << "line " << line_no << ": supported dimensions are Q1..Q9, got Q" << dim;
            throw runtime_error(msg.str());
        }

        string name = fields[1].empty() ? ("custom_line_" + to_string(line_no)) : fields[1];
        State state = parse_permutation(fields[2]);
        validate_permutation(dim, state, line_no);

        cases_by_dim[dim].push_back({
            name,
            "custom case from " + path.generic_string() + ":" + to_string(line_no),
            move(state)
        });
    }

    return cases_by_dim;
}

string beam_visited_mode_name(BeamVisitedMode mode) {
    switch (mode) {
        case BeamVisitedMode::ExactPacked:
            return "exact";
        case BeamVisitedMode::Fingerprint128:
            return "fingerprint128";
        case BeamVisitedMode::Fingerprint128Disk:
            return "fingerprint128_disk";
    }
    return "unknown";
}

BeamVisitedMode parse_beam_visited_mode(string value) {
    transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(tolower(ch));
    });
    if (value == "0" || value == "exact" || value == "exact_packed") {
        return BeamVisitedMode::ExactPacked;
    }
    if (value == "1" || value == "fingerprint" || value == "fingerprint128") {
        return BeamVisitedMode::Fingerprint128;
    }
    if (
        value == "2" ||
        value == "disk" ||
        value == "fingerprint_disk" ||
        value == "fingerprint128_disk"
    ) {
        return BeamVisitedMode::Fingerprint128Disk;
    }
    throw invalid_argument(
        "beam_visited_mode must be exact/0, fingerprint128/1, or fingerprint128_disk/2"
    );
}

string safe_filename(string value) {
    for (char& ch : value) {
        bool safe =
            (ch >= 'a' && ch <= 'z') ||
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '-' ||
            ch == '_';
        if (!safe) {
            ch = '_';
        }
    }
    return value.empty() ? "case" : value;
}

void print_usage(const char* exe) {
    cerr << "Usage: " << exe
         << " [min_dim=4] [max_dim=6] [beam_width=256] [max_depth=0]"
         << " [exact_max_dim=4] [astar_cap=2000000] [exact_time_sec=30]"
         << " [output_dir=output/qk_special_cases] [custom_cases_csv] [candidate_trace_mode=2]"
         << " [case_name_filter] [beam_visited_mode=exact] [worker_threads=24]"
         << " [disk_bloom_mb=1024] [disk_batch_size=1000000] [disk_shards=16]\n"
         << "max_depth=0 uses auto depth dim * 2^dim for each Qdim.\n";
    cerr << "Use custom_cases_csv=- to select built-in cases while passing later options.\n";
    cerr << "candidate_trace_mode: 0=none, 1=retained beam only, 2=all new candidates.\n";
    cerr << "beam_visited_mode: exact/0 stores full packed states (Q1..Q8); "
         << "fingerprint128/1 stores fingerprints in RAM; "
         << "fingerprint128_disk/2 stores exact fingerprints in SQLite.\n";
}

} // namespace qk

int main(int argc, char** argv) {
    using namespace qk;

    int min_dim = 4;
    int max_dim = 6;
    int beam_width = 256;
    int max_depth = 0;
    int exact_max_dim = 4;
    size_t astar_cap = 2000000;
    double exact_time_sec = 30.0;
    filesystem::path output_dir = "output/qk_special_cases";
    filesystem::path custom_cases_path;
    bool use_custom_cases = false;
    int candidate_trace_mode_arg = static_cast<int>(CandidateTraceMode::AllNewCandidates);
    string case_name_filter;
    BeamVisitedMode beam_visited_mode = BeamVisitedMode::ExactPacked;
    int worker_threads = 24;
    size_t disk_bloom_mb = 1024;
    size_t disk_batch_size = 1000000;
    int disk_shards = 16;

    try {
        if (argc > 1) min_dim = stoi(argv[1]);
        if (argc > 2) max_dim = stoi(argv[2]);
        if (argc > 3) beam_width = stoi(argv[3]);
        if (argc > 4) max_depth = stoi(argv[4]);
        if (argc > 5) exact_max_dim = stoi(argv[5]);
        if (argc > 6) astar_cap = stoull(argv[6]);
        if (argc > 7) exact_time_sec = stod(argv[7]);
        if (argc > 8) output_dir = argv[8];
        if (argc > 9) {
            string custom_cases_arg = argv[9];
            if (!custom_cases_arg.empty() && custom_cases_arg != "-") {
                custom_cases_path = custom_cases_arg;
                use_custom_cases = true;
            }
        }
        if (argc > 10) candidate_trace_mode_arg = stoi(argv[10]);
        if (argc > 11) case_name_filter = argv[11];
        if (argc > 12) beam_visited_mode = parse_beam_visited_mode(argv[12]);
        if (argc > 13) worker_threads = stoi(argv[13]);
        if (argc > 14) disk_bloom_mb = stoull(argv[14]);
        if (argc > 15) disk_batch_size = stoull(argv[15]);
        if (argc > 16) disk_shards = stoi(argv[16]);
    } catch (const exception& e) {
        cerr << "Argument parse error: " << e.what() << "\n";
        print_usage(argv[0]);
        return 2;
    }

    if (min_dim < 1 || max_dim < min_dim || max_dim > 9) {
        cerr << "Dimensions must satisfy 1 <= min_dim <= max_dim <= 9.\n";
        return 2;
    }
    if (exact_max_dim < 0 || exact_max_dim > 8) {
        cerr << "exact_max_dim must satisfy 0 <= exact_max_dim <= 8.\n";
        return 2;
    }
    if (beam_visited_mode == BeamVisitedMode::ExactPacked && max_dim > 8) {
        cerr << "exact Beam visited supports only Q1..Q8; use fingerprint128 for Q9.\n";
        return 2;
    }
    if (beam_width <= 0) {
        cerr << "beam_width must be positive.\n";
        return 2;
    }
    if (worker_threads <= 0) {
        cerr << "worker_threads must be positive.\n";
        return 2;
    }
    if (disk_bloom_mb == 0 || disk_batch_size == 0 || disk_shards <= 0) {
        cerr << "disk_bloom_mb, disk_batch_size, and disk_shards must be positive.\n";
        return 2;
    }
    if (candidate_trace_mode_arg < 0 || candidate_trace_mode_arg > 2) {
        cerr << "candidate_trace_mode must be 0, 1, or 2.\n";
        return 2;
    }
    CandidateTraceMode candidate_trace_mode = static_cast<CandidateTraceMode>(candidate_trace_mode_arg);

#ifdef _WIN32
    if (beam_visited_mode == BeamVisitedMode::Fingerprint128Disk) {
        SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    }
#endif

    map<int, vector<SpecialCase>> cases_by_dim;
    try {
        if (use_custom_cases) {
            map<int, vector<SpecialCase>> loaded = read_custom_cases(custom_cases_path);
            for (auto& entry : loaded) {
                int dim = entry.first;
                if (dim >= min_dim && dim <= max_dim) {
                    cases_by_dim[dim] = move(entry.second);
                }
            }
        } else {
            for (int dim = min_dim; dim <= max_dim; ++dim) {
                cases_by_dim[dim] = make_cases(dim);
            }
        }
    } catch (const exception& e) {
        cerr << "Custom case load error: " << e.what() << "\n";
        return 2;
    }

    if (!case_name_filter.empty()) {
        map<int, vector<SpecialCase>> filtered;
        for (auto& entry : cases_by_dim) {
            vector<SpecialCase> kept;
            for (auto& c : entry.second) {
                if (c.name == case_name_filter) {
                    kept.push_back(move(c));
                }
            }
            if (!kept.empty()) {
                filtered[entry.first] = move(kept);
            }
        }
        cases_by_dim = move(filtered);
    }

    if (cases_by_dim.empty()) {
        cerr << "No cases to run after applying the dimension filter.\n";
        return 2;
    }

    filesystem::create_directories(output_dir);
    const filesystem::path csv_path = output_dir / "qk_special_cases.csv";
    const filesystem::path checkpoint_csv_path = output_dir / "qk_case_progress.csv";
    const filesystem::path beam_depth_csv_path = output_dir / "qk_beam_depth_progress.csv";
    const filesystem::path beam_candidate_trace_csv_path = output_dir / "qk_beam_candidate_trace.csv";
    const filesystem::path beam_disk_progress_csv_path = output_dir / "qk_beam_disk_progress.csv";
    ofstream csv(csv_path);
    csv << "dim,n,case_name,note,total_dist,misplaced,basic_lb,strong_lb,parity,"
        << "exact_status,exact_steps,exact_expanded,exact_states,exact_sec,exact_path_valid,"
        << "beam_visited_mode,beam_status,beam_steps,beam_expanded,beam_states,beam_sec,beam_path_valid,beam_gap_vs_exact,"
        << "batcher_success,batcher_swaps,batcher_compares,batcher_rounds,batcher_sec,batcher_path_valid,batcher_gap_vs_exact,"
        << "state_perm,beam_path,batcher_path\n";

    ofstream checkpoint_csv(checkpoint_csv_path);
    checkpoint_csv << "completed_case,total_cases,dim,case_name,total_dist,strong_lb,"
                   << "exact_status,exact_steps,exact_sec,exact_path_valid,"
                   << "beam_visited_mode,beam_status,beam_steps,beam_sec,beam_path_valid,"
                   << "batcher_success,batcher_swaps,batcher_sec,batcher_path_valid,"
                   << "elapsed_sec\n";

    ofstream beam_depth_csv(beam_depth_csv_path);
    beam_depth_csv << "case_index,total_cases,dim,case_name,depth,max_depth,"
                   << "expanded,layer_candidates,retained,best_total_dist,best_misplaced,best_max_dist,elapsed_sec\n";

    ofstream beam_candidate_trace_csv(beam_candidate_trace_csv_path);
    beam_candidate_trace_csv << "case_index,total_cases,dim,case_name,trace_kind,depth,candidate_id,"
                             << "total_dist,misplaced,max_dist,edge_id,path_len,state_perm,path\n";

    ofstream beam_disk_progress_csv(beam_disk_progress_csv_path);
    beam_disk_progress_csv << "case_index,total_cases,dim,case_name,depth,visited_states,"
                           << "pending_states,disk_bytes,worker_threads,generation_sec,"
                           << "visited_sec,selection_sec\n";

    cout << "Qk special-case benchmark\n";
    cout << "dims=Q" << min_dim << "..Q" << max_dim
         << ", beam_width=" << beam_width
         << ", max_depth=" << (max_depth > 0 ? to_string(max_depth) : string("auto"))
         << ", exact_max_dim=Q" << exact_max_dim
         << ", astar_cap=" << astar_cap
         << ", exact_time_sec=" << fixed << setprecision(0) << exact_time_sec
         << ", candidate_trace_mode=" << candidate_trace_mode_arg
         << ", beam_visited_mode=" << beam_visited_mode_name(beam_visited_mode)
         << ", worker_threads=" << worker_threads
         << ", disk_bloom_mb=" << disk_bloom_mb
         << ", disk_batch_size=" << disk_batch_size
         << ", disk_shards=" << disk_shards
         << ", output_dir=" << output_dir.generic_string() << "\n";
    if (use_custom_cases) {
        cout << "custom_cases=" << custom_cases_path.generic_string() << "\n";
    }
    if (!case_name_filter.empty()) {
        cout << "case_name_filter=" << case_name_filter << "\n";
    }

    int total_cases = 0;
    for (const auto& entry : cases_by_dim) {
        total_cases += static_cast<int>(entry.second.size());
    }
    int completed_cases = 0;
    auto run_start = Clock::now();

    for (const auto& dim_cases : cases_by_dim) {
        int dim = dim_cases.first;
        const auto es = edges(dim);
        const auto& cases = dim_cases.second;
        int dim_max_depth = max_depth > 0 ? max_depth : dim * node_count(dim);
        int exact_solved = 0;
        int beam_solved = 0;
        int batcher_solved = 0;

        cout << "\nQ" << dim << " cases=" << cases.size()
             << ", beam_max_depth=" << dim_max_depth << "\n";

        for (const auto& c : cases) {
            ProgressContext progress{true, completed_cases + 1, total_cases, dim, c.name};
            int n = node_count(dim);
            int td = total_distance(c.state);
            int misplaced = misplaced_count(c.state);
            int basic_lb = basic_lower_bound(c.state);
            int strong_lb = strong_lower_bound(c.state);
            int parity = permutation_parity(c.state);

            PathResult exact;
            bool run_exact = dim <= exact_max_dim;
            if (run_exact) {
                auto deadline = Clock::now() + chrono::milliseconds(static_cast<long long>(exact_time_sec * 1000.0));
                print_progress_line(progress, "strong_astar_time", 0, max<long long>(1, static_cast<long long>(exact_time_sec * 1000.0)), 0);
                exact = astar_exact(c.state, es, astar_cap, deadline, true, progress, exact_time_sec);
            }

            PathResult beam;
            if (beam_visited_mode == BeamVisitedMode::Fingerprint128Disk) {
                filesystem::path visited_database_path =
                    output_dir /
                    (
                        "q" + to_string(dim) + "_" +
                        safe_filename(c.name) +
                        "_beam_visited.sqlite3"
                    );
                beam = beam_search_disk(
                    c.state,
                    es,
                    beam_width,
                    dim_max_depth,
                    true,
                    worker_threads,
                    disk_bloom_mb,
                    disk_batch_size,
                    disk_shards,
                    visited_database_path,
                    progress,
                    &beam_depth_csv,
                    &beam_candidate_trace_csv,
                    &beam_disk_progress_csv,
                    candidate_trace_mode
                );
            } else {
                beam = beam_search(
                    c.state,
                    es,
                    beam_width,
                    dim_max_depth,
                    true,
                    progress,
                    &beam_depth_csv,
                    &beam_candidate_trace_csv,
                    candidate_trace_mode,
                    beam_visited_mode
                );
            }
            trim_process_memory();
            print_progress_line(progress, "batcher", 1, 1, 0);
            BatcherResult batcher = batcher_baseline(c.state, true);

            bool exact_valid = exact.steps == 0 || (exact.steps > 0 && path_reaches_goal(c.state, exact.swaps, dim));
            if (!run_exact) {
                exact_valid = false;
            }
            bool beam_valid = beam.steps == 0 || (beam.steps > 0 && path_reaches_goal(c.state, beam.swaps, dim));
            bool batcher_valid = batcher.success && path_reaches_goal(c.state, batcher.path, dim);

            if (exact.status == "solved") {
                exact_solved++;
            }
            if (beam.status == "solved") {
                beam_solved++;
            }
            if (batcher.success) {
                batcher_solved++;
            }

            int beam_gap = (exact.status == "solved" && beam.status == "solved") ? beam.steps - exact.steps : 0;
            int batcher_gap = (exact.status == "solved" && batcher.success) ? batcher.swaps - exact.steps : 0;

            completed_cases++;
            clear_progress_line();
            cout << "  " << left << setw(30) << c.name
                 << " lb=" << right << setw(3) << strong_lb
                 << " exact=" << setw(10) << exact.status
                 << "(" << setw(3) << exact.steps << ")"
                 << " beam=" << setw(7) << beam.status
                 << "(" << setw(3) << beam.steps << ")"
                 << " batcher=" << (batcher.success ? "ok" : "fail")
                 << "(" << batcher.swaps << ")\n";

            csv << dim << "," << n << ","
                << csv_escape(c.name) << "," << csv_escape(c.note) << ","
                << td << "," << misplaced << "," << basic_lb << "," << strong_lb << "," << parity << ","
                << exact.status << "," << exact.steps << "," << exact.expanded << "," << exact.reached_states << ","
                << fixed << setprecision(6) << exact.sec << "," << (exact_valid ? 1 : 0) << ","
                << beam_visited_mode_name(beam_visited_mode) << ","
                << beam.status << "," << beam.steps << "," << beam.expanded << "," << beam.reached_states << ","
                << beam.sec << "," << (beam_valid ? 1 : 0) << "," << beam_gap << ","
                << (batcher.success ? 1 : 0) << "," << batcher.swaps << "," << batcher.compares << "," << batcher.rounds << ","
                << batcher.sec << "," << (batcher_valid ? 1 : 0) << "," << batcher_gap << ","
                << csv_escape(state_perm(c.state)) << ","
                << csv_escape(path_string(beam.swaps)) << ","
                << csv_escape(path_string(batcher.path)) << "\n";
            csv.flush();

            double elapsed_sec = chrono::duration<double>(Clock::now() - run_start).count();
            checkpoint_csv << completed_cases << "," << total_cases << ","
                           << dim << "," << csv_escape(c.name) << ","
                           << td << "," << strong_lb << ","
                           << exact.status << "," << exact.steps << ","
                           << fixed << setprecision(6) << exact.sec << "," << (exact_valid ? 1 : 0) << ","
                           << beam_visited_mode_name(beam_visited_mode) << ","
                           << beam.status << "," << beam.steps << "," << beam.sec << "," << (beam_valid ? 1 : 0) << ","
                           << (batcher.success ? 1 : 0) << "," << batcher.swaps << "," << batcher.sec << ","
                           << (batcher_valid ? 1 : 0) << "," << elapsed_sec << "\n";
            checkpoint_csv.flush();
            trim_process_memory();
        }

        cout << "Q" << dim << " summary: exact_solved=" << exact_solved << "/" << cases.size()
             << ", beam_solved=" << beam_solved << "/" << cases.size()
             << ", batcher_solved=" << batcher_solved << "/" << cases.size() << "\n";
    }

    cout << "\nCSV: " << csv_path.generic_string() << "\n";
    cout << "Checkpoint CSV: " << checkpoint_csv_path.generic_string() << "\n";
    cout << "Beam depth CSV: " << beam_depth_csv_path.generic_string() << "\n";
    cout << "Beam candidate trace CSV: " << beam_candidate_trace_csv_path.generic_string() << "\n";
    cout << "Beam disk progress CSV: " << beam_disk_progress_csv_path.generic_string() << "\n";
    return 0;
}
