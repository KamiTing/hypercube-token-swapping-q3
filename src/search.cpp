#include "search.h"

#include <algorithm>
#include <array>
#include <queue>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace std;

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

struct ParentInfo {
    uint64_t prev;
    SwapStep swap;
};

static PathResult reconstruct_path(
    uint64_t initial_state,
    uint64_t target,
    const unordered_map<uint64_t, ParentInfo>& parent
) {
    PathResult result;
    if (initial_state == target) {
        result.steps = 0;
        return result;
    }

    auto it = parent.find(target);
    if (it == parent.end()) {
        return result;
    }

    uint64_t cur = target;
    while (cur != initial_state) {
        const auto p = parent.at(cur);
        result.swaps.push_back(p.swap);
        cur = p.prev;
    }

    reverse(result.swaps.begin(), result.swaps.end());
    result.steps = static_cast<int>(result.swaps.size());
    return result;
}

PathResult bfs_shortest_path(uint64_t initial_state, const vector<Edge>& edges) {
    const uint64_t target = target_state();
    if (initial_state == target) {
        return {0, {}};
    }

    queue<uint64_t> q;
    unordered_map<uint64_t, ParentInfo> parent;
    parent.reserve(50000);
    parent.max_load_factor(0.7f);

    q.push(initial_state);
    parent[initial_state] = {initial_state, {-1, -1}};

    while (!q.empty()) {
        uint64_t state = q.front();
        q.pop();

        for (const auto& e : edges) {
            uint64_t next_state = swap_nodes(state, e.u, e.v);
            if (parent.find(next_state) != parent.end()) {
                continue;
            }

            parent[next_state] = {state, {e.u, e.v}};
            if (next_state == target) {
                return reconstruct_path(initial_state, target, parent);
            }
            q.push(next_state);
        }
    }

    return {};
}

int astar_heuristic(uint64_t state) {
    return (total_distance(state) + 1) / 2;
}

static int cycle_lower_bound(uint64_t state) {
    array<bool, cfg::NODE_COUNT> seen{};
    int cycles = 0;

    for (int i = 0; i < cfg::NODE_COUNT; ++i) {
        if (seen[i]) {
            continue;
        }

        cycles++;
        int cur = i;
        while (!seen[cur]) {
            seen[cur] = true;
            cur = get_packet(state, cur);
        }
    }

    return cfg::NODE_COUNT - cycles;
}

static int permutation_parity(uint64_t state) {
    int parity = 0;
    for (int i = 0; i < cfg::NODE_COUNT; ++i) {
        int pi = get_packet(state, i);
        for (int j = i + 1; j < cfg::NODE_COUNT; ++j) {
            if (pi > get_packet(state, j)) {
                parity ^= 1;
            }
        }
    }
    return parity;
}

static int parity_adjust(uint64_t state, int lower_bound) {
    int parity = permutation_parity(state);
    if ((lower_bound & 1) != parity) {
        lower_bound++;
    }
    return lower_bound;
}

int strong_astar_heuristic(uint64_t state) {
    int h_basic = astar_heuristic(state);
    int h_max_packet = max_packet_distance(state);
    int h_cycle = cycle_lower_bound(state);
    return parity_adjust(state, max({h_basic, h_max_packet, h_cycle}));
}

static int selected_astar_heuristic(uint64_t state, bool use_strong) {
    return use_strong ? strong_astar_heuristic(state) : astar_heuristic(state);
}

struct AStarNode {
    int f;
    int h;
    int g;
    uint64_t state;

    bool operator<(const AStarNode& other) const {
        if (f != other.f) return f > other.f;
        if (h != other.h) return h > other.h;
        return g > other.g;
    }
};

static int astar_min_steps_impl(uint64_t initial_state, const vector<Edge>& edges, bool use_strong) {
    const uint64_t target = target_state();
    if (initial_state == target) {
        return 0;
    }

    priority_queue<AStarNode> pq;
    unordered_map<uint64_t, int> best_g;
    best_g.reserve(50000);
    best_g.max_load_factor(0.7f);

    int h0 = selected_astar_heuristic(initial_state, use_strong);
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
            int next_h = selected_astar_heuristic(next_state, use_strong);
            pq.push({next_g + next_h, next_h, next_g, next_state});
        }
    }

    return -1;
}

int astar_min_steps(uint64_t initial_state, const vector<Edge>& edges) {
    return astar_min_steps_impl(initial_state, edges, false);
}

int strong_astar_min_steps(uint64_t initial_state, const vector<Edge>& edges) {
    return astar_min_steps_impl(initial_state, edges, true);
}

static PathResult astar_shortest_path_impl(uint64_t initial_state, const vector<Edge>& edges, bool use_strong) {
    const uint64_t target = target_state();
    if (initial_state == target) {
        return {0, {}};
    }

    priority_queue<AStarNode> pq;
    unordered_map<uint64_t, int> best_g;
    unordered_map<uint64_t, ParentInfo> parent;
    best_g.reserve(50000);
    parent.reserve(50000);
    best_g.max_load_factor(0.7f);
    parent.max_load_factor(0.7f);

    int h0 = selected_astar_heuristic(initial_state, use_strong);
    pq.push({h0, h0, 0, initial_state});
    best_g[initial_state] = 0;
    parent[initial_state] = {initial_state, {-1, -1}};

    while (!pq.empty()) {
        AStarNode cur = pq.top();
        pq.pop();

        auto it = best_g.find(cur.state);
        if (it == best_g.end() || cur.g != it->second) {
            continue;
        }
        if (cur.state == target) {
            return reconstruct_path(initial_state, target, parent);
        }

        for (const auto& e : edges) {
            uint64_t next_state = swap_nodes(cur.state, e.u, e.v);
            int next_g = cur.g + 1;
            auto old = best_g.find(next_state);
            if (old != best_g.end() && next_g >= old->second) {
                continue;
            }

            best_g[next_state] = next_g;
            parent[next_state] = {cur.state, {e.u, e.v}};
            int next_h = selected_astar_heuristic(next_state, use_strong);
            pq.push({next_g + next_h, next_h, next_g, next_state});
        }
    }

    return {};
}

PathResult astar_shortest_path(uint64_t initial_state, const vector<Edge>& edges) {
    return astar_shortest_path_impl(initial_state, edges, false);
}

PathResult strong_astar_shortest_path(uint64_t initial_state, const vector<Edge>& edges) {
    return astar_shortest_path_impl(initial_state, edges, true);
}

struct BeamItem {
    uint64_t state;
    int depth;
    int last_edge_id;
    array<unsigned char, cfg::EDGE_COUNT> used_edges;
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
        return tie(total_dist, misplaced, max_dist, repeat_penalty, neg_improvement,
                   neg_local_improvement, neg_dir_score, neg_touched_max_dist, depth) <
               tie(other.total_dist, other.misplaced, other.max_dist, other.repeat_penalty,
                   other.neg_improvement, other.neg_local_improvement, other.neg_dir_score,
                   other.neg_touched_max_dist, other.depth);
    }
};

struct BeamCandidate {
    BeamScore score;
    BeamItem item;

    bool operator<(const BeamCandidate& other) const {
        return score < other.score;
    }
};

int beam_search_steps(uint64_t initial_state, const vector<Edge>& edges, int beam_width, int max_depth) {
    const uint64_t target = target_state();
    if (initial_state == target) {
        return 0;
    }

    const auto& h = hdist_table();

    BeamItem start{initial_state, 0, -1, {}};
    start.used_edges.fill(0);

    vector<BeamItem> beam{start};
    unordered_map<uint64_t, int> visited_best_depth;
    visited_best_depth.reserve(50000);
    visited_best_depth.max_load_factor(0.7f);
    visited_best_depth[initial_state] = 0;

    for (int depth = 1; depth <= max_depth; ++depth) {
        vector<BeamCandidate> candidates;
        candidates.reserve(static_cast<size_t>(beam.size()) * edges.size());

        for (const auto& item : beam) {
            uint64_t state = item.state;
            auto dir_score = global_direction_preference(state);
            int old_total = total_distance(state);

            for (int eid = 0; eid < static_cast<int>(edges.size()); ++eid) {
                const Edge& e = edges[eid];
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

                int old_du = h[e.u][packet_u];
                int old_dv = h[e.v][packet_v];
                int new_du = h[e.v][packet_u];
                int new_dv = h[e.u][packet_v];

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
                    if (c > 1) repeat_penalty += static_cast<int>(c) - 1;
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

struct BeamPathItem {
    uint64_t state;
    int depth;
    int last_edge_id;
    array<unsigned char, cfg::EDGE_COUNT> used_edges;
    vector<SwapStep> swaps;
};

struct BeamPathCandidate {
    BeamScore score;
    BeamPathItem item;

    bool operator<(const BeamPathCandidate& other) const {
        return score < other.score;
    }
};

PathResult beam_search_path(uint64_t initial_state, const vector<Edge>& edges, int beam_width, int max_depth) {
    const uint64_t target = target_state();
    if (initial_state == target) {
        return {0, {}};
    }

    const auto& h = hdist_table();

    BeamPathItem start{initial_state, 0, -1, {}, {}};
    start.used_edges.fill(0);

    vector<BeamPathItem> beam{start};
    unordered_map<uint64_t, int> visited_best_depth;
    visited_best_depth.reserve(50000);
    visited_best_depth.max_load_factor(0.7f);
    visited_best_depth[initial_state] = 0;

    for (int depth = 1; depth <= max_depth; ++depth) {
        vector<BeamPathCandidate> candidates;
        candidates.reserve(static_cast<size_t>(beam.size()) * edges.size());

        for (const auto& item : beam) {
            uint64_t state = item.state;
            auto dir_score = global_direction_preference(state);
            int old_total = total_distance(state);

            for (int eid = 0; eid < static_cast<int>(edges.size()); ++eid) {
                const Edge& e = edges[eid];
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

                int old_du = h[e.u][packet_u];
                int old_dv = h[e.v][packet_v];
                int new_du = h[e.v][packet_u];
                int new_dv = h[e.u][packet_v];

                int local_improvement = (old_du + old_dv) - (new_du + new_dv);
                int touched_max_dist = max(old_du, old_dv);

                BeamPathItem next_item;
                next_item.state = next_state;
                next_item.depth = depth;
                next_item.last_edge_id = eid;
                next_item.used_edges = item.used_edges;
                next_item.used_edges[eid]++;
                next_item.swaps = item.swaps;
                next_item.swaps.push_back({e.u, e.v});

                int repeat_penalty = 0;
                for (unsigned char c : next_item.used_edges) {
                    if (c > 1) repeat_penalty += static_cast<int>(c) - 1;
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
                    return {depth, next_item.swaps};
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

    return {};
}
