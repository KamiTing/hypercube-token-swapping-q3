#include "qk/search.h"

#include "qk/hypercube.h"
#include "qk/io.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <queue>
#include <sqlite3.h>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace std;

namespace qk {

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

struct ParentInfo {
    string prev;
    SwapStep swap;
};

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

struct BeamPathRecord {
    int32_t parent = -1;
    uint16_t u = 0;
    uint16_t v = 0;
};

static_assert(sizeof(BeamPathRecord) == 8, "BeamPathRecord must stay compact");

class BeamPathStore {
public:
    explicit BeamPathStore(bool enabled) : enabled_(enabled) {
        if (!enabled_) {
            return;
        }

        static atomic<unsigned long long> next_id{0};
        unsigned long long id = next_id.fetch_add(1, memory_order_relaxed);
        auto ticks = chrono::steady_clock::now().time_since_epoch().count();
        path_ = filesystem::temp_directory_path() /
                ("qk_beam_path_" + to_string(ticks) + "_" + to_string(id) + ".bin");

        out_.open(path_, ios::binary | ios::trunc);
        if (!out_) {
            throw runtime_error("cannot create Beam path temporary file: " + path_.string());
        }
    }

    BeamPathStore(const BeamPathStore&) = delete;
    BeamPathStore& operator=(const BeamPathStore&) = delete;

    ~BeamPathStore() {
        close_noexcept();
        if (!path_.empty()) {
            error_code ec;
            filesystem::remove(path_, ec);
        }
    }

    int append(int parent, SwapStep step) {
        if (!enabled_) {
            return -1;
        }
        if (count_ == numeric_limits<int>::max()) {
            throw runtime_error("Beam path temporary file exceeded int back-pointer range");
        }
        if (step.u < 0 || step.v < 0 ||
            step.u > numeric_limits<uint16_t>::max() ||
            step.v > numeric_limits<uint16_t>::max()) {
            throw runtime_error("Beam path temporary file supports node ids up to 65535");
        }
        BeamPathRecord record{
            static_cast<int32_t>(parent),
            static_cast<uint16_t>(step.u),
            static_cast<uint16_t>(step.v)
        };
        out_.write(reinterpret_cast<const char*>(&record), sizeof(record));
        if (!out_) {
            throw runtime_error("cannot write Beam path temporary file");
        }
        return count_++;
    }

    vector<SwapStep> reconstruct(int path_node) {
        vector<SwapStep> path;
        if (!enabled_ || path_node < 0) {
            return path;
        }

        flush();
        while (path_node >= 0) {
            BeamPathRecord record = read(path_node);
            path.push_back({
                static_cast<int>(record.u),
                static_cast<int>(record.v)
            });
            path_node = record.parent;
        }
        reverse(path.begin(), path.end());
        return path;
    }

    string path_string_with_step(int parent_path_node, SwapStep step) {
        vector<SwapStep> path = reconstruct(parent_path_node);
        path.push_back(step);
        return path_string(path);
    }

private:
    void flush() {
        if (out_.is_open()) {
            out_.flush();
        }
    }

    BeamPathRecord read(int path_node) {
        if (path_node < 0 || path_node >= count_) {
            throw runtime_error("invalid Beam path back-pointer");
        }
        if (!in_.is_open()) {
            in_.open(path_, ios::binary);
            if (!in_) {
                throw runtime_error("cannot read Beam path temporary file: " + path_.string());
            }
        }
        in_.clear();
        in_.seekg(static_cast<streamoff>(path_node) * static_cast<streamoff>(sizeof(BeamPathRecord)));
        BeamPathRecord record;
        in_.read(reinterpret_cast<char*>(&record), sizeof(record));
        if (!in_) {
            throw runtime_error("cannot read Beam path back-pointer");
        }
        return record;
    }

    void close_noexcept() {
        if (out_.is_open()) {
            out_.close();
        }
        if (in_.is_open()) {
            in_.close();
        }
    }

    bool enabled_ = false;
    filesystem::path path_;
    ofstream out_;
    ifstream in_;
    int count_ = 0;
};

class LayerVisitedWindow {
public:
    explicit LayerVisitedWindow(int rounds) : max_rounds_(max(rounds, 0)) {}

    bool enabled() const {
        return max_rounds_ > 0;
    }

    bool contains(const Fingerprint128& fingerprint) const {
        if (!enabled()) {
            return false;
        }
        return counts_.find(fingerprint) != counts_.end();
    }

    void add_layer(vector<Fingerprint128> fingerprints) {
        if (!enabled()) {
            return;
        }
        for (const Fingerprint128& fingerprint : fingerprints) {
            counts_[fingerprint]++;
        }
        layers_.push_back(move(fingerprints));
        while (static_cast<int>(layers_.size()) > max_rounds_) {
            for (const Fingerprint128& fingerprint : layers_.front()) {
                auto it = counts_.find(fingerprint);
                if (it == counts_.end()) {
                    continue;
                }
                it->second--;
                if (it->second <= 0) {
                    counts_.erase(it);
                }
            }
            layers_.pop_front();
        }
    }

    size_t unique_size() const {
        return counts_.size();
    }

private:
    int max_rounds_ = 0;
    deque<vector<Fingerprint128>> layers_;
    unordered_map<Fingerprint128, int, Fingerprint128Hash> counts_;
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
    BeamPathStore& path_store,
    int path_node
) {
    return path_store.reconstruct(path_node);
}

string beam_path_string_with_step(
    BeamPathStore& path_store,
    int parent_path_node,
    SwapStep step
) {
    return path_store.path_string_with_step(parent_path_node, step);
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

uint64_t perturbation_key(const BeamCandidate& candidate, int depth) {
    uint64_t key =
        candidate.fingerprint.low ^
        (candidate.fingerprint.high + 0x9E3779B97F4A7C15ULL) ^
        (static_cast<uint64_t>(depth) * 0xBF58476D1CE4E5B9ULL) ^
        (static_cast<uint64_t>(candidate.edge_id + 1) * 0x94D049BB133111EBULL) ^
        static_cast<uint64_t>(candidate.order);
    return splitmix64(key);
}

vector<BeamCandidate> select_layer_only_retained(
    vector<BeamCandidate>& sorted_candidates,
    int beam_width,
    double perturbation_ratio,
    int depth
) {
    int perturb_slots = 0;
    if (perturbation_ratio > 0.0) {
        perturb_slots = static_cast<int>(ceil(static_cast<double>(beam_width) * perturbation_ratio));
        perturb_slots = min(max(perturb_slots, 1), beam_width);
    }
    int greedy_slots = beam_width - perturb_slots;

    unordered_set<Fingerprint128, Fingerprint128Hash> retained_seen;
    retained_seen.reserve(static_cast<size_t>(beam_width) * 2);
    vector<char> chosen(sorted_candidates.size(), 0);
    vector<BeamCandidate> retained;
    retained.reserve(static_cast<size_t>(beam_width));

    auto keep_index = [&](size_t index) -> bool {
        if (chosen[index]) {
            return false;
        }
        const Fingerprint128 fingerprint = sorted_candidates[index].fingerprint;
        if (!retained_seen.insert(fingerprint).second) {
            chosen[index] = 1;
            return false;
        }
        chosen[index] = 1;
        retained.push_back(move(sorted_candidates[index]));
        return true;
    };

    for (size_t i = 0;
         i < sorted_candidates.size() && static_cast<int>(retained.size()) < greedy_slots;
         ++i) {
        keep_index(i);
    }

    if (perturb_slots > 0 && static_cast<int>(retained.size()) < beam_width) {
        struct PerturbChoice {
            uint64_t key = 0;
            size_t index = 0;
        };
        vector<PerturbChoice> choices;
        choices.reserve(sorted_candidates.size());
        for (size_t i = 0; i < sorted_candidates.size(); ++i) {
            if (!chosen[i]) {
                choices.push_back({perturbation_key(sorted_candidates[i], depth), i});
            }
        }
        sort(
            choices.begin(),
            choices.end(),
            [](const PerturbChoice& a, const PerturbChoice& b) {
                if (a.key != b.key) {
                    return a.key < b.key;
                }
                return a.index < b.index;
            }
        );
        for (const PerturbChoice& choice : choices) {
            if (static_cast<int>(retained.size()) >= beam_width) {
                break;
            }
            keep_index(choice.index);
        }
    }

    for (size_t i = 0;
         i < sorted_candidates.size() && static_cast<int>(retained.size()) < beam_width;
         ++i) {
        keep_index(i);
    }

    return retained;
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

struct LayerParallelResult {
    priority_queue<BeamCandidate, vector<BeamCandidate>, BeamWorstFirst> top_candidates;
    long long layer_candidates = 0;
    bool solved = false;
    BeamCandidate solved_candidate;
    int solved_parent_path_node = -1;
    SwapStep solved_step{-1, -1};
};

struct LayerParallelWorkerResult {
    priority_queue<BeamCandidate, vector<BeamCandidate>, BeamWorstFirst> top_candidates;
    long long layer_candidates = 0;
};

LayerParallelResult generate_layer_only_candidates_parallel(
    const vector<BeamItem>& beam,
    const vector<Edge>& es,
    const State& goal,
    int depth,
    bool use_packed_tie_key,
    int candidate_keep_limit,
    int worker_threads,
    long long candidate_order_base,
    const LayerVisitedWindow& recent_visited
) {
    LayerParallelResult result;
    atomic<size_t> next_parent{0};
    atomic<bool> solved{false};
    mutex solved_mutex;
    mutex error_mutex;
    exception_ptr worker_error;
    const long long edge_count = static_cast<long long>(es.size());

    auto keep_solution = [&](BeamCandidate cand, int parent_path_node, SwapStep step) {
        lock_guard<mutex> lock(solved_mutex);
        if (!result.solved || candidate_better(cand, result.solved_candidate)) {
            result.solved = true;
            result.solved_candidate = move(cand);
            result.solved_parent_path_node = parent_path_node;
            result.solved_step = step;
        }
        solved.store(true, memory_order_relaxed);
    };

    int active_threads = min<int>(
        max(worker_threads, 1),
        max<int>(static_cast<int>(beam.size()), 1)
    );
    int local_keep_limit = candidate_keep_limit;
    if (active_threads > 1) {
        long long split_limit =
            (static_cast<long long>(candidate_keep_limit) + active_threads - 1) /
            active_threads;
        long long extra_limit = min<long long>(candidate_keep_limit, 65536);
        local_keep_limit = static_cast<int>(min(
            static_cast<long long>(candidate_keep_limit),
            split_limit + extra_limit
        ));
    }
    vector<LayerParallelWorkerResult> worker_results(active_threads);

    auto keep_candidate_local = [&](LayerParallelWorkerResult& local, BeamCandidate cand) {
        if (
            static_cast<int>(local.top_candidates.size()) >= local_keep_limit &&
            !candidate_better(cand, local.top_candidates.top())
        ) {
            return;
        }
        if (static_cast<int>(local.top_candidates.size()) >= local_keep_limit) {
            local.top_candidates.pop();
        }
        local.top_candidates.push(move(cand));
    };

    auto worker = [&](int worker_index) {
        LayerParallelWorkerResult& local = worker_results[worker_index];
        try {
            while (!solved.load(memory_order_relaxed)) {
                size_t parent_index = next_parent.fetch_add(1, memory_order_relaxed);
                if (parent_index >= beam.size()) {
                    break;
                }

                const BeamItem& item = beam[parent_index];
                for (int eid = 0; eid < static_cast<int>(es.size()); ++eid) {
                    if (solved.load(memory_order_relaxed)) {
                        break;
                    }
                    if (eid == item.last_edge) {
                        continue;
                    }

                    const Edge& edge = es[eid];
                    int token_u = item.state[edge.u];
                    int token_v = item.state[edge.v];
                    State state = item.state;
                    swap(state[edge.u], state[edge.v]);

                    Fingerprint128 fingerprint = fingerprint_after_swap(
                        item.fingerprint,
                        edge.u,
                        edge.v,
                        token_u,
                        token_v
                    );
                    PackedStateKey packed_key;
                    if (use_packed_tie_key) {
                        packed_key = packed_key_from_state(state);
                    }
                    if (recent_visited.contains(fingerprint)) {
                        continue;
                    }

                    local.layer_candidates++;
                    long long order =
                        candidate_order_base +
                        static_cast<long long>(parent_index) * edge_count +
                        static_cast<long long>(eid) +
                        1;

                    if (state == goal) {
                        keep_solution(
                            BeamCandidate{
                                0,
                                0,
                                0,
                                depth,
                                {},
                                eid,
                                item.path_node,
                                move(packed_key),
                                fingerprint,
                                use_packed_tie_key,
                                order
                            },
                            item.path_node,
                            {edge.u, edge.v}
                        );
                        break;
                    }

                    keep_candidate_local(local, BeamCandidate{
                        total_distance(state),
                        misplaced_count(state),
                        max_packet_distance(state),
                        depth,
                        move(state),
                        eid,
                        item.path_node,
                        move(packed_key),
                        fingerprint,
                        use_packed_tie_key,
                        order
                    });
                }
            }
        } catch (...) {
            lock_guard<mutex> lock(error_mutex);
            if (worker_error == nullptr) {
                worker_error = current_exception();
            }
            solved.store(true, memory_order_relaxed);
        }
    };

    vector<thread> workers;
    workers.reserve(active_threads);
    for (int i = 0; i < active_threads; ++i) {
        workers.emplace_back(worker, i);
    }
    for (thread& worker_thread : workers) {
        worker_thread.join();
    }
    if (worker_error != nullptr) {
        rethrow_exception(worker_error);
    }

    for (LayerParallelWorkerResult& local : worker_results) {
        result.layer_candidates += local.layer_candidates;
        while (!local.top_candidates.empty()) {
            BeamCandidate cand = move(const_cast<BeamCandidate&>(local.top_candidates.top()));
            local.top_candidates.pop();
            if (
                static_cast<int>(result.top_candidates.size()) >= candidate_keep_limit &&
                !candidate_better(cand, result.top_candidates.top())
            ) {
                continue;
            }
            if (static_cast<int>(result.top_candidates.size()) >= candidate_keep_limit) {
                result.top_candidates.pop();
            }
            result.top_candidates.push(move(cand));
        }
    }
    return result;
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
    BeamVisitedMode visited_mode = BeamVisitedMode::ExactPacked,
    int worker_threads,
    int layer_pool_width,
    int layer_visited_window,
    int layer_plateau_limit,
    double layer_perturbation_ratio
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
        throw runtime_error("exact packed Beam visited supports up to Q8; use fingerprint128 or layer_only for larger cases");
    }

    bool layer_only_visited = visited_mode == BeamVisitedMode::LayerOnly;
    bool use_packed_tie_key = n <= 256;
    Fingerprint128 initial_fingerprint = fingerprint_from_state(init);

    vector<BeamItem> beam = {{init, -1, -1, initial_fingerprint}};
    bool keep_backpointers = record_path || trace_mode != CandidateTraceMode::None;
    BeamPathStore path_store(keep_backpointers);
    LayerVisitedWindow recent_visited(layer_only_visited ? layer_visited_window : 0);
    if (layer_only_visited) {
        vector<Fingerprint128> initial_layer;
        initial_layer.push_back(initial_fingerprint);
        recent_visited.add_layer(move(initial_layer));
    }
    unordered_set<PackedStateKey, PackedStateKeyHash> exact_seen;
    unordered_set<Fingerprint128, Fingerprint128Hash> fingerprint_seen;
    size_t reserve_hint = static_cast<size_t>(max(beam_width, 1)) *
                          static_cast<size_t>(max(1, min(max_depth, 1024)));
    reserve_hint = max<size_t>(reserve_hint, 1024);
    if (visited_mode == BeamVisitedMode::ExactPacked) {
        exact_seen.reserve(reserve_hint);
        exact_seen.insert(packed_key_from_state(init));
    } else if (!layer_only_visited) {
        fingerprint_seen.reserve(reserve_hint);
        fingerprint_seen.insert(initial_fingerprint);
    }

    long long expanded = 0;
    int best_seen_total_dist = numeric_limits<int>::max();
    int plateau_depths = 0;
    auto reached_state_count = [&]() -> size_t {
        if (visited_mode == BeamVisitedMode::ExactPacked) {
            return exact_seen.size();
        }
        if (layer_only_visited) {
            return static_cast<size_t>(expanded + 1);
        }
        return fingerprint_seen.size();
    };

    long long candidate_order = 0;
    for (int depth = 1; depth <= max_depth; ++depth) {
        print_progress_line(progress, "beam_depth", depth, max_depth, expanded);

        priority_queue<BeamCandidate, vector<BeamCandidate>, BeamWorstFirst> top_candidates;
        int candidate_keep_limit = beam_width;
        if (layer_only_visited) {
            long long widened_limit =
                layer_pool_width > 0
                    ? static_cast<long long>(layer_pool_width)
                    : min(
                          static_cast<long long>(beam_width) * 4,
                          static_cast<long long>(beam_width) + 4096
                      );
            candidate_keep_limit = static_cast<int>(max(static_cast<long long>(beam_width), widened_limit));
        }
        long long layer_candidates = 0;
        bool use_parallel_layer =
            layer_only_visited &&
            trace_mode == CandidateTraceMode::None &&
            worker_threads > 1 &&
            beam.size() > 1;

        if (use_parallel_layer) {
            long long layer_order_span =
                static_cast<long long>(beam.size()) *
                static_cast<long long>(es.size());
            LayerParallelResult parallel = generate_layer_only_candidates_parallel(
                beam,
                es,
                goal,
                depth,
                use_packed_tie_key,
                candidate_keep_limit,
                worker_threads,
                candidate_order,
                recent_visited
            );
            top_candidates = move(parallel.top_candidates);
            layer_candidates = parallel.layer_candidates;
            expanded += layer_candidates;
            candidate_order += layer_order_span;

            if (parallel.solved) {
                auto t1 = Clock::now();
                vector<BeamCandidate> solved_selected = {parallel.solved_candidate};
                double elapsed = chrono::duration<double>(t1 - t0).count();
                size_t reached_count = reached_state_count();
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

                int solved_parent_path_node = parallel.solved_parent_path_node;
                SwapStep solved_step = parallel.solved_step;
                priority_queue<BeamCandidate, vector<BeamCandidate>, BeamWorstFirst>().swap(top_candidates);
                vector<BeamItem>().swap(beam);
                unordered_set<PackedStateKey, PackedStateKeyHash>().swap(exact_seen);
                unordered_set<Fingerprint128, Fingerprint128Hash>().swap(fingerprint_seen);

                vector<SwapStep> next_path;
                if (record_path) {
                    next_path = reconstruct_beam_path(path_store, solved_parent_path_node);
                    next_path.push_back(solved_step);
                }
                return {depth, expanded, reached_count, elapsed, "solved", next_path};
            }
        } else {
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
                    inserted = layer_only_visited ? true : fingerprint_seen.insert(fingerprint).second;
                    if (use_packed_tie_key) {
                        packed_key = packed_key_from_state(ns);
                    }
                    if (layer_only_visited && recent_visited.contains(fingerprint)) {
                        inserted = false;
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
                    int solved_parent_path_node = item.path_node;
                    SwapStep solved_step{e.u, e.v};
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
                            beam_path_string_with_step(path_store, solved_parent_path_node, solved_step)
                        );
                        candidate_trace_csv->flush();
                    }
                    double elapsed = chrono::duration<double>(t1 - t0).count();
                    size_t reached_count = reached_state_count();
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

                    priority_queue<BeamCandidate, vector<BeamCandidate>, BeamWorstFirst>().swap(top_candidates);
                    vector<BeamItem>().swap(beam);
                    unordered_set<PackedStateKey, PackedStateKeyHash>().swap(exact_seen);
                    unordered_set<Fingerprint128, Fingerprint128Hash>().swap(fingerprint_seen);

                    vector<SwapStep> next_path;
                    if (record_path) {
                        next_path = reconstruct_beam_path(path_store, solved_parent_path_node);
                        next_path.push_back(solved_step);
                    }
                    return {depth, expanded, reached_count, elapsed, "solved", next_path};
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
                        beam_path_string_with_step(path_store, item.path_node, {e.u, e.v})
                    );
                    if ((candidate_order % 10000) == 0) {
                        candidate_trace_csv->flush();
                    }
                }

                if (static_cast<int>(top_candidates.size()) >= candidate_keep_limit && !candidate_better(cand, top_candidates.top())) {
                    continue;
                }

                if (static_cast<int>(top_candidates.size()) >= candidate_keep_limit) {
                    top_candidates.pop();
                }

                top_candidates.push(move(cand));
            }
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
        if (layer_only_visited) {
            selected = select_layer_only_retained(
                selected,
                beam_width,
                layer_perturbation_ratio,
                depth
            );
        }

        bool plateau_reached = false;
        if (layer_only_visited && layer_plateau_limit > 0 && !selected.empty()) {
            int current_best_total_dist = selected.front().total_dist;
            if (current_best_total_dist < best_seen_total_dist) {
                best_seen_total_dist = current_best_total_dist;
                plateau_depths = 0;
            } else {
                plateau_depths++;
                plateau_reached = plateau_depths >= layer_plateau_limit;
            }
        }

        vector<int> selected_path_nodes(selected.size(), -1);
        if (keep_backpointers) {
            for (size_t i = 0; i < selected.size(); ++i) {
                const Edge& edge = es[selected[i].edge_id];
                selected_path_nodes[i] = path_store.append(
                    selected[i].parent_path_node,
                    {edge.u, edge.v}
                );
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
                    path_string(reconstruct_beam_path(path_store, selected_path_nodes[i]))
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

        if (plateau_reached) {
            size_t reached_count = reached_state_count();
            double elapsed = chrono::duration<double>(Clock::now() - t0).count();
            priority_queue<BeamCandidate, vector<BeamCandidate>, BeamWorstFirst>().swap(top_candidates);
            vector<BeamCandidate>().swap(selected);
            vector<int>().swap(selected_path_nodes);
            vector<BeamItem>().swap(beam);
            unordered_set<PackedStateKey, PackedStateKeyHash>().swap(exact_seen);
            unordered_set<Fingerprint128, Fingerprint128Hash>().swap(fingerprint_seen);
            return {-1, expanded, reached_count, elapsed, "plateau", {}};
        }

        if (layer_only_visited) {
            vector<Fingerprint128> retained_fingerprints;
            retained_fingerprints.reserve(selected.size());
            for (const BeamCandidate& candidate : selected) {
                retained_fingerprints.push_back(candidate.fingerprint);
            }
            recent_visited.add_layer(move(retained_fingerprints));
        }

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

    void release(int worker_threads) {
        flush(worker_threads);
        shards_.clear();
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

    bool keep_backpointers = record_path || trace_mode != CandidateTraceMode::None;
    BeamPathStore path_store(keep_backpointers);

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
                    int solved_parent_path_node = parent.path_node;
                    SwapStep solved_step{edge.u, edge.v};

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
                                path_store,
                                solved_parent_path_node,
                                solved_step
                            )
                        );
                        candidate_trace_csv->flush();
                    }

                    visited.flush(worker_threads);
                    size_t visited_size = visited.size();
                    size_t pending_size = visited.pending_size();
                    uintmax_t disk_bytes = visited.disk_bytes();
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
                        visited_size,
                        pending_size,
                        disk_bytes,
                        worker_threads,
                        chrono::duration<double>(generation_end - generation_start).count(),
                        chrono::duration<double>(visited_end - visited_start).count(),
                        chrono::duration<double>(Clock::now() - selection_start).count()
                    );

                    priority_queue<DiskBeamCandidate, vector<DiskBeamCandidate>, DiskBeamWorstFirst>().swap(top_candidates);
                    vector<DiskBeamItem>().swap(beam);
                    vector<DiskGeneratedCandidate>().swap(generated);
                    vector<uint8_t>().swap(is_new);
                    vector<BeamCandidate>().swap(solved_selected);
                    visited.release(worker_threads);

                    vector<SwapStep> next_path;
                    if (record_path) {
                        next_path = reconstruct_beam_path(path_store, solved_parent_path_node);
                        next_path.push_back(solved_step);
                    }
                    return {
                        depth,
                        expanded,
                        visited_size,
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
                            path_store,
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
                selected_path_nodes[i] = path_store.append(
                    selected[i].parent_path_node,
                    {edge.u, edge.v}
                );
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
                    path_string(reconstruct_beam_path(path_store, selected_path_nodes[i]))
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

string beam_visited_mode_name(BeamVisitedMode mode) {
    switch (mode) {
        case BeamVisitedMode::ExactPacked:
            return "exact";
        case BeamVisitedMode::Fingerprint128:
            return "fingerprint128";
        case BeamVisitedMode::Fingerprint128Disk:
            return "fingerprint128_disk";
        case BeamVisitedMode::LayerOnly:
            return "layer_only";
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
    if (
        value == "3" ||
        value == "layer" ||
        value == "layer_only" ||
        value == "layeronly" ||
        value == "no_global" ||
        value == "no_global_visited"
    ) {
        return BeamVisitedMode::LayerOnly;
    }
    throw invalid_argument(
        "beam_visited_mode must be exact/0, fingerprint128/1, fingerprint128_disk/2, or layer_only/3"
    );
}


} // namespace qk
