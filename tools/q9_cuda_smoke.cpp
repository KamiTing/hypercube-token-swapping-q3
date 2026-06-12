#include "qk/cuda_candidate_generator.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

using namespace std;
using namespace qk;

namespace {

State load_case(const string& path, const string& name) {
    ifstream in(path);
    if (!in) {
        throw runtime_error("cannot open cases csv: " + path);
    }
    string line;
    const string marker = "," + name + ",";
    while (getline(in, line)) {
        if (line.empty() || line[0] == '#') {
            continue;
        }
        if (line.find(marker) == string::npos) {
            continue;
        }
        size_t first_quote = line.find('"');
        size_t last_quote = line.rfind('"');
        if (first_quote == string::npos || last_quote == first_quote) {
            throw runtime_error("case line does not contain quoted permutation");
        }
        string body = line.substr(first_quote + 1, last_quote - first_quote - 1);
        stringstream ss(body);
        State state;
        int value = 0;
        while (ss >> value) {
            state.push_back(value);
        }
        return state;
    }
    throw runtime_error("case not found: " + name);
}

int bit_count(unsigned value) {
    int count = 0;
    while (value != 0) {
        count += static_cast<int>(value & 1u);
        value >>= 1u;
    }
    return count;
}

array<int, 16> distance_histogram(const State& state) {
    array<int, 16> histogram{};
    for (int node = 0; node < static_cast<int>(state.size()); ++node) {
        histogram[bit_count(static_cast<unsigned>(node ^ state[node]))]++;
    }
    return histogram;
}

vector<Edge> make_edges(int dim) {
    vector<Edge> result;
    int n = 1 << dim;
    for (int u = 0; u < n; ++u) {
        for (int bit = 0; bit < dim; ++bit) {
            int v = u ^ (1 << bit);
            if (u < v) {
                result.push_back({u, v, bit});
            }
        }
    }
    return result;
}

int smoke_total_distance(const State& state) {
    int total = 0;
    for (int node = 0; node < static_cast<int>(state.size()); ++node) {
        total += bit_count(static_cast<unsigned>(node ^ state[node]));
    }
    return total;
}

int smoke_misplaced_count(const State& state) {
    int total = 0;
    for (int node = 0; node < static_cast<int>(state.size()); ++node) {
        total += state[node] != node;
    }
    return total;
}

bool candidate_better(const CudaLayerCandidate& a, const CudaLayerCandidate& b) {
    if (a.total_dist != b.total_dist) return a.total_dist < b.total_dist;
    if (a.max_dist != b.max_dist) return a.max_dist < b.max_dist;
    if (a.misplaced != b.misplaced) return a.misplaced < b.misplaced;
    if (a.edge_id != b.edge_id) return a.edge_id < b.edge_id;
    if (!(a.fingerprint == b.fingerprint)) return fingerprint_less(a.fingerprint, b.fingerprint);
    return a.order < b.order;
}

} // namespace

int main(int argc, char** argv) {
    string csv = argc > 1 ? argv[1] : "custom_qk_cases.csv";
    string case_name = argc > 2 ? argv[2] : "q9_case1";
    int beam_width = argc > 3 ? stoi(argv[3]) : 256;
    int pool_width = argc > 4 ? stoi(argv[4]) : 256;
    int depths = argc > 5 ? stoi(argv[5]) : 4;

    if (!cuda_candidate_backend_available()) {
        cerr << cuda_candidate_backend_unavailable_reason() << "\n";
        return 2;
    }

    State initial = load_case(csv, case_name);
    int dim = 0;
    while ((1 << dim) < static_cast<int>(initial.size())) {
        dim++;
    }
    if ((1 << dim) != static_cast<int>(initial.size())) {
        throw runtime_error("case size is not a power of two");
    }
    vector<Edge> es = make_edges(dim);
    vector<CudaLayerParent> beam;
    beam.push_back({
        initial,
        -1,
        -1,
        fingerprint_from_state(initial),
        smoke_total_distance(initial),
        smoke_misplaced_count(initial),
        distance_histogram(initial)
    });
    vector<Fingerprint128> recent;
    recent.push_back(beam.front().fingerprint);

    cout << "case=" << case_name
         << " dim=Q" << dim
         << " nodes=" << initial.size()
         << " edges=" << es.size()
         << " beam_width=" << beam_width
         << " pool_width=" << pool_width
         << " depths=" << depths << "\n";

    long long candidate_order = 0;
    for (int depth = 1; depth <= depths; ++depth) {
        auto t0 = chrono::steady_clock::now();
        CudaLayerGenerationResult result = generate_layer_only_candidates_cuda(
            beam,
            es,
            static_cast<int>(initial.size()),
            depth,
            pool_width,
            candidate_order,
            recent,
            false
        );
        auto t1 = chrono::steady_clock::now();
        if (!result.used_cuda) {
            cerr << "CUDA not used: " << result.fallback_reason << "\n";
            return 3;
        }
        sort(result.candidates.begin(), result.candidates.end(), candidate_better);
        if (static_cast<int>(result.candidates.size()) > beam_width) {
            result.candidates.resize(static_cast<size_t>(beam_width));
        }

        cout << "depth=" << depth
             << " used_cuda=" << result.used_cuda
             << " layer_candidates=" << result.layer_candidates
             << " returned=" << result.candidates.size()
             << " best_total_dist=" << (result.candidates.empty() ? -1 : result.candidates.front().total_dist)
             << " best_misplaced=" << (result.candidates.empty() ? -1 : result.candidates.front().misplaced)
             << " sec=" << chrono::duration<double>(t1 - t0).count()
             << "\n";

        vector<CudaLayerParent> next_beam;
        next_beam.reserve(result.candidates.size());
        recent.clear();
        for (const CudaLayerCandidate& candidate : result.candidates) {
            State state = beam[static_cast<size_t>(candidate.parent_index)].state;
            const Edge& edge = es[static_cast<size_t>(candidate.edge_id)];
            swap(state[edge.u], state[edge.v]);
            next_beam.push_back({
                state,
                candidate.edge_id,
                -1,
                candidate.fingerprint,
                candidate.total_dist,
                candidate.misplaced,
                distance_histogram(state)
            });
            recent.push_back(candidate.fingerprint);
        }
        beam = move(next_beam);
        candidate_order += static_cast<long long>(beam.size()) * static_cast<long long>(es.size());
        if (beam.empty()) {
            break;
        }
    }

    return 0;
}
