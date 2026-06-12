#include "qk/path_optimizer.h"

#include "qk/hypercube.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <limits>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace std;

namespace qk {
namespace {

struct SegmentAStarResult {
    string status = "skipped";
    int steps = -1;
    long long expanded = 0;
    size_t reached_states = 0;
    vector<SwapStep> swaps;
};

struct SegmentParent {
    string prev;
    SwapStep swap{-1, -1};
};

struct SegmentNode {
    int f = 0;
    int h = 0;
    int g = 0;
    int total_dist = 0;
    int max_dist = 0;
    long long order = 0;
    State state;
    string key;
    array<int, 16> distance_hist{};

    bool operator<(const SegmentNode& other) const {
        if (f != other.f) {
            return f > other.f;
        }
        if (h != other.h) {
            return h > other.h;
        }
        if (g != other.g) {
            return g > other.g;
        }
        return order > other.order;
    }
};

struct SegmentTask {
    int start = 0;
    int end = 0;
};

struct SegmentTaskResult {
    bool improved = false;
    int start = 0;
    int end = 0;
    SegmentAStarResult astar;
};

bool same_edge(const SwapStep& a, const SwapStep& b) {
    return (a.u == b.u && a.v == b.v) || (a.u == b.v && a.v == b.u);
}

bool disjoint_edges(const SwapStep& a, const SwapStep& b) {
    return a.u != b.u && a.u != b.v && a.v != b.u && a.v != b.v;
}

vector<SwapStep> reduce_commuting_cancellations(
    const vector<SwapStep>& path,
    int& removed_steps
) {
    vector<SwapStep> reduced;
    reduced.reserve(path.size());

    for (const SwapStep& step : path) {
        int cancel_index = -1;
        for (int i = static_cast<int>(reduced.size()) - 1; i >= 0; --i) {
            if (same_edge(reduced[i], step)) {
                cancel_index = i;
                break;
            }
            if (!disjoint_edges(reduced[i], step)) {
                break;
            }
        }

        if (cancel_index >= 0) {
            reduced.erase(reduced.begin() + cancel_index);
            removed_steps += 2;
        } else {
            reduced.push_back(step);
        }
    }

    return reduced;
}

string state_key16(const State& s) {
    string key;
    key.reserve(s.size() * 2);
    for (int token : s) {
        if (token < 0 || token > 65535) {
            throw runtime_error("path optimizer state key supports token labels 0..65535");
        }
        unsigned value = static_cast<unsigned>(token);
        key.push_back(static_cast<char>(value & 0xffu));
        key.push_back(static_cast<char>((value >> 8) & 0xffu));
    }
    return key;
}

vector<int> target_positions(const State& target) {
    vector<int> positions(target.size(), -1);
    for (int position = 0; position < static_cast<int>(target.size()); ++position) {
        int token = target[position];
        if (token < 0 || token >= static_cast<int>(target.size())) {
            throw runtime_error("path optimizer target state contains out-of-range token");
        }
        positions[token] = position;
    }
    for (int token = 0; token < static_cast<int>(positions.size()); ++token) {
        if (positions[token] < 0) {
            throw runtime_error("path optimizer target state is not a permutation");
        }
    }
    return positions;
}

int relative_total_distance(const State& state, const vector<int>& target_pos) {
    int total = 0;
    for (int position = 0; position < static_cast<int>(state.size()); ++position) {
        total += hdist(position, target_pos[state[position]]);
    }
    return total;
}

array<int, 16> relative_distance_histogram(const State& state, const vector<int>& target_pos) {
    array<int, 16> hist{};
    for (int position = 0; position < static_cast<int>(state.size()); ++position) {
        int distance = hdist(position, target_pos[state[position]]);
        if (distance < 0 || distance >= static_cast<int>(hist.size())) {
            throw runtime_error("path optimizer distance histogram is too small");
        }
        hist[distance]++;
    }
    return hist;
}

int histogram_max_distance(const array<int, 16>& hist) {
    for (int distance = static_cast<int>(hist.size()) - 1; distance >= 0; --distance) {
        if (hist[distance] > 0) {
            return distance;
        }
    }
    return 0;
}

int relative_parity(const State& state, const vector<int>& target_pos) {
    vector<int> mapping(state.size(), 0);
    for (int position = 0; position < static_cast<int>(state.size()); ++position) {
        mapping[position] = target_pos[state[position]];
    }

    int parity = 0;
    for (int i = 0; i < static_cast<int>(mapping.size()); ++i) {
        for (int j = i + 1; j < static_cast<int>(mapping.size()); ++j) {
            if (mapping[i] > mapping[j]) {
                parity ^= 1;
            }
        }
    }
    return parity;
}

int relative_distance_lower_bound(int total_dist, int max_dist, int parity) {
    int lower_bound = max((total_dist + 1) / 2, max_dist);
    if ((lower_bound & 1) != parity) {
        lower_bound++;
    }
    return lower_bound;
}

vector<SwapStep> reconstruct_segment_path(
    const string& init_key,
    const string& goal_key,
    const unordered_map<string, SegmentParent>& parent
) {
    vector<SwapStep> path;
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

SegmentAStarResult bounded_segment_astar(
    const State& init,
    const State& target,
    const vector<Edge>& es,
    int max_depth,
    size_t node_cap,
    double time_limit_sec
) {
    auto t0 = Clock::now();
    const string init_key = state_key16(init);
    const string goal_key = state_key16(target);

    if (init_key == goal_key) {
        return {"solved", 0, 0, 1, {}};
    }
    if (max_depth < 0) {
        return {"bound", -1, 0, 1, {}};
    }

    const vector<int> target_pos = target_positions(target);
    const int initial_total = relative_total_distance(init, target_pos);
    const array<int, 16> initial_hist = relative_distance_histogram(init, target_pos);
    const int initial_max = histogram_max_distance(initial_hist);
    const int initial_parity = relative_parity(init, target_pos);
    const int h0 = relative_distance_lower_bound(initial_total, initial_max, initial_parity);
    if (h0 > max_depth) {
        return {"bound", -1, 0, 1, {}};
    }

    priority_queue<SegmentNode> pq;
    unordered_map<string, int> best;
    size_t reserve_cap = node_cap == 0 ? 500000 : min<size_t>(node_cap, 500000);
    best.reserve(reserve_cap);
    best.max_load_factor(0.7f);

    unordered_map<string, SegmentParent> parent;
    parent.reserve(reserve_cap);
    parent.max_load_factor(0.7f);

    long long order = 0;
    pq.push({h0, h0, 0, initial_total, initial_max, order++, init, init_key, initial_hist});
    best[init_key] = 0;

    const auto deadline = time_limit_sec > 0.0
        ? t0 + chrono::milliseconds(static_cast<long long>(time_limit_sec * 1000.0))
        : Clock::time_point::max();

    long long expanded = 0;
    while (!pq.empty()) {
        if ((expanded & 0x3FF) == 0 && Clock::now() >= deadline) {
            return {"time_limit", -1, expanded, best.size(), {}};
        }

        SegmentNode cur = pq.top();
        pq.pop();

        auto current_best = best.find(cur.key);
        if (current_best == best.end() || current_best->second != cur.g) {
            continue;
        }

        expanded++;
        if (node_cap > 0 && best.size() > node_cap) {
            return {"cap", -1, expanded, best.size(), {}};
        }

        if (cur.key == goal_key) {
            vector<SwapStep> path = reconstruct_segment_path(init_key, goal_key, parent);
            return {path.empty() ? string("path_error") : string("solved"),
                    static_cast<int>(path.size()),
                    expanded,
                    best.size(),
                    move(path)};
        }

        for (const auto& edge : es) {
            int ng = cur.g + 1;
            if (ng > max_depth) {
                continue;
            }

            int token_u = cur.state[edge.u];
            int token_v = cur.state[edge.v];
            int old_u_dist = hdist(edge.u, target_pos[token_u]);
            int old_v_dist = hdist(edge.v, target_pos[token_v]);
            int new_u_dist = hdist(edge.v, target_pos[token_u]);
            int new_v_dist = hdist(edge.u, target_pos[token_v]);

            array<int, 16> hist = cur.distance_hist;
            hist[old_u_dist]--;
            hist[old_v_dist]--;
            hist[new_u_dist]++;
            hist[new_v_dist]++;

            int total_dist = cur.total_dist - old_u_dist - old_v_dist + new_u_dist + new_v_dist;
            int max_dist = histogram_max_distance(hist);
            int parity = initial_parity ^ (ng & 1);
            int h = relative_distance_lower_bound(total_dist, max_dist, parity);
            if (ng + h > max_depth) {
                continue;
            }

            State ns = cur.state;
            swap(ns[edge.u], ns[edge.v]);
            string nk = state_key16(ns);
            auto old = best.find(nk);
            if (old != best.end() && ng >= old->second) {
                continue;
            }

            best[nk] = ng;
            parent[nk] = {cur.key, {edge.u, edge.v}};
            pq.push({ng + h, h, ng, total_dist, max_dist, order++, move(ns), move(nk), hist});
        }
    }

    return {"exhausted", -1, expanded, best.size(), {}};
}

bool replay_path_states(
    const State& init,
    const vector<SwapStep>& path,
    vector<State>& states
) {
    states.clear();
    states.reserve(path.size() + 1);
    states.push_back(init);
    int n = static_cast<int>(init.size());
    for (const auto& step : path) {
        if (step.u < 0 || step.u >= n || step.v < 0 || step.v >= n) {
            return false;
        }
        if (hdist(step.u, step.v) != 1) {
            return false;
        }
        State next = states.back();
        swap(next[step.u], next[step.v]);
        states.push_back(move(next));
    }
    return true;
}

vector<int> window_schedule(int max_window, int min_window, int path_size) {
    vector<int> windows;
    int window = min(max_window, path_size);
    while (window >= min_window) {
        windows.push_back(window);
        if (window == min_window) {
            break;
        }
        window = max(min_window, window / 2);
    }
    return windows;
}

int effective_stride(int window, int requested_stride) {
    if (requested_stride > 0) {
        return min(requested_stride, window);
    }
    return max(1, window / 4);
}

vector<SegmentTask> build_segment_tasks(int path_size, int window, int stride) {
    vector<SegmentTask> tasks;
    if (window <= 1 || window > path_size) {
        return tasks;
    }

    for (int start = 0; start + window <= path_size; start += stride) {
        tasks.push_back({start, start + window});
    }

    int final_start = path_size - window;
    if (final_start > 0 && (tasks.empty() || tasks.back().start != final_start)) {
        tasks.push_back({final_start, path_size});
    }

    return tasks;
}

vector<SegmentTaskResult> select_non_overlapping_improvements(
    vector<SegmentTaskResult> results
) {
    vector<SegmentTaskResult> improved;
    for (auto& result : results) {
        if (result.improved) {
            improved.push_back(move(result));
        }
    }
    if (improved.empty()) {
        return improved;
    }

    sort(improved.begin(), improved.end(), [](const auto& a, const auto& b) {
        if (a.end != b.end) {
            return a.end < b.end;
        }
        return a.start < b.start;
    });

    int n = static_cast<int>(improved.size());
    vector<int> ends;
    ends.reserve(n);
    for (const auto& result : improved) {
        ends.push_back(result.end);
    }

    vector<int> previous(n, -1);
    for (int i = 0; i < n; ++i) {
        previous[i] = static_cast<int>(
            upper_bound(ends.begin(), ends.begin() + i, improved[i].start) - ends.begin()
        ) - 1;
    }

    vector<int> dp(n + 1, 0);
    vector<char> take(n, 0);
    for (int i = 0; i < n; ++i) {
        int weight = (improved[i].end - improved[i].start) - improved[i].astar.steps;
        int take_value = dp[previous[i] + 1] + weight;
        int skip_value = dp[i];
        if (take_value > skip_value) {
            dp[i + 1] = take_value;
            take[i] = 1;
        } else {
            dp[i + 1] = skip_value;
        }
    }

    vector<SegmentTaskResult> selected;
    for (int i = n - 1; i >= 0;) {
        if (take[i]) {
            selected.push_back(move(improved[i]));
            i = previous[i];
        } else {
            --i;
        }
    }

    reverse(selected.begin(), selected.end());
    return selected;
}

vector<SegmentTaskResult> solve_tasks_parallel(
    const vector<SegmentTask>& tasks,
    const vector<State>& states,
    const vector<Edge>& es,
    const PathOptimizerOptions& options
) {
    vector<SegmentTaskResult> results(tasks.size());
    if (tasks.empty()) {
        return results;
    }

    atomic<size_t> next_task{0};
    mutex error_mutex;
    exception_ptr worker_error;
    int active_threads = min<int>(
        max(options.worker_threads, 1),
        static_cast<int>(tasks.size())
    );

    auto worker = [&]() {
        try {
            while (true) {
                size_t index = next_task.fetch_add(1);
                if (index >= tasks.size()) {
                    break;
                }

                const SegmentTask& task = tasks[index];
                int original_len = task.end - task.start;
                SegmentAStarResult astar = bounded_segment_astar(
                    states[task.start],
                    states[task.end],
                    es,
                    original_len - 1,
                    options.node_cap,
                    options.segment_time_sec
                );

                bool improved =
                    astar.status == "solved" &&
                    astar.steps >= 0 &&
                    astar.steps < original_len;
                results[index] = {improved, task.start, task.end, move(astar)};
            }
        } catch (...) {
            lock_guard<mutex> lock(error_mutex);
            if (worker_error == nullptr) {
                worker_error = current_exception();
            }
        }
    };

    if (active_threads <= 1) {
        worker();
        if (worker_error != nullptr) {
            rethrow_exception(worker_error);
        }
        return results;
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
    return results;
}

void apply_segment_results(
    const vector<SegmentTaskResult>& results,
    const vector<SwapStep>& current_path,
    vector<SwapStep>& next_path
) {
    next_path.clear();
    next_path.reserve(current_path.size());

    int cursor = 0;
    for (const auto& result : results) {
        next_path.insert(
            next_path.end(),
            current_path.begin() + cursor,
            current_path.begin() + result.start
        );
        if (result.improved) {
            next_path.insert(next_path.end(), result.astar.swaps.begin(), result.astar.swaps.end());
        } else {
            next_path.insert(
                next_path.end(),
                current_path.begin() + result.start,
                current_path.begin() + result.end
            );
        }
        cursor = result.end;
    }
    next_path.insert(next_path.end(), current_path.begin() + cursor, current_path.end());
}

} // namespace

PathOptimizerResult optimize_path_shortcuts(
    const State& init,
    const vector<Edge>& es,
    const vector<SwapStep>& original_path,
    const PathOptimizerOptions& options
) {
    auto t0 = Clock::now();
    PathOptimizerResult result;
    result.original_steps = static_cast<int>(original_path.size());

    if (!options.enabled || options.max_window <= 1 || original_path.empty()) {
        result.status = "skipped";
        result.optimized_steps = result.original_steps;
        result.swaps = original_path;
        result.sec = chrono::duration<double>(Clock::now() - t0).count();
        return result;
    }

    vector<State> states;
    if (!replay_path_states(init, original_path, states)) {
        result.status = "invalid_original";
        result.optimized_steps = result.original_steps;
        result.sec = chrono::duration<double>(Clock::now() - t0).count();
        return result;
    }
    const State goal_state = states.back();

    vector<SwapStep> current_path = original_path;
    if (options.word_reduction) {
        int removed = 0;
        current_path = reduce_commuting_cancellations(current_path, removed);
        result.word_reductions += removed;
    }
    vector<SwapStep> next_path;
    const int min_window = max(2, options.min_window);
    const int pass_count = max(1, options.passes);

    for (int pass = 0; pass < pass_count; ++pass) {
        bool pass_improved = false;
        vector<int> windows = window_schedule(
            max(options.max_window, min_window),
            min_window,
            static_cast<int>(current_path.size())
        );

        for (int window : windows) {
            if (window > static_cast<int>(current_path.size())) {
                continue;
            }
            if (!replay_path_states(init, current_path, states)) {
                result.status = "invalid_optimized";
                result.optimized_steps = static_cast<int>(current_path.size());
                result.sec = chrono::duration<double>(Clock::now() - t0).count();
                result.swaps = move(current_path);
                return result;
            }

            int stride = effective_stride(window, options.window_stride);
            vector<SegmentTask> tasks = build_segment_tasks(
                static_cast<int>(current_path.size()),
                window,
                stride
            );
            if (tasks.empty()) {
                continue;
            }

            vector<SegmentTaskResult> task_results =
                solve_tasks_parallel(tasks, states, es, options);
            result.attempts += static_cast<int>(task_results.size());

            bool window_improved = false;
            for (const auto& task_result : task_results) {
                result.expanded += task_result.astar.expanded;
                result.reached_states += task_result.astar.reached_states;
                if (task_result.improved) {
                    window_improved = true;
                }
            }

            if (window_improved) {
                vector<SegmentTaskResult> selected_results =
                    select_non_overlapping_improvements(move(task_results));
                if (selected_results.empty()) {
                    continue;
                }
                result.improved_segments += static_cast<int>(selected_results.size());
                apply_segment_results(selected_results, current_path, next_path);
                current_path.swap(next_path);
                if (options.word_reduction) {
                    int removed = 0;
                    current_path = reduce_commuting_cancellations(current_path, removed);
                    result.word_reductions += removed;
                }
                pass_improved = true;
            }
        }

        if (!pass_improved) {
            break;
        }
    }

    if (!replay_path_states(init, current_path, states) || states.back() != goal_state) {
        result.status = "invalid_optimized";
        result.optimized_steps = static_cast<int>(current_path.size());
        result.improvement = result.original_steps - result.optimized_steps;
        result.sec = chrono::duration<double>(Clock::now() - t0).count();
        result.swaps = move(current_path);
        return result;
    }

    result.optimized_steps = static_cast<int>(current_path.size());
    result.improvement = result.original_steps - result.optimized_steps;
    result.status = result.improvement > 0 ? "improved" : "no_improvement";
    result.sec = chrono::duration<double>(Clock::now() - t0).count();
    result.swaps = move(current_path);
    return result;
}

} // namespace qk
