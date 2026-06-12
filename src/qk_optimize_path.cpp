#include "qk/hypercube.h"
#include "qk/io.h"
#include "qk/path_optimizer.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std;

namespace qk {
namespace {

void print_usage(const char* exe) {
    cerr << "Usage: " << exe
         << " <input_qk_special_cases.csv> <output_csv>"
         << " [case_name_filter=-] [path_opt_window=32] [path_opt_passes=1]"
         << " [path_opt_node_cap=200000] [path_opt_segment_time_sec=0.25]"
         << " [path_opt_threads=16] [path_opt_stride=0] [path_opt_word_reduce=1]\n";
}

vector<string> parse_csv_line(const string& line) {
    vector<string> fields;
    string field;
    bool in_quote = false;
    for (size_t i = 0; i < line.size(); ++i) {
        char ch = line[i];
        if (in_quote) {
            if (ch == '"') {
                if (i + 1 < line.size() && line[i + 1] == '"') {
                    field.push_back('"');
                    i++;
                } else {
                    in_quote = false;
                }
            } else {
                field.push_back(ch);
            }
        } else if (ch == '"') {
            in_quote = true;
        } else if (ch == ',') {
            fields.push_back(field);
            field.clear();
        } else {
            field.push_back(ch);
        }
    }
    fields.push_back(field);
    return fields;
}

State parse_state_perm(const string& text, int n) {
    State state;
    state.reserve(n);
    istringstream in(text);
    int token = 0;
    while (in >> token) {
        state.push_back(token);
    }
    if (static_cast<int>(state.size()) != n) {
        throw runtime_error("state_perm length does not match dim");
    }
    vector<char> seen(n, 0);
    for (int value : state) {
        if (value < 0 || value >= n || seen[value]) {
            throw runtime_error("state_perm is not a valid permutation");
        }
        seen[value] = 1;
    }
    return state;
}

vector<SwapStep> parse_path(const string& text) {
    vector<SwapStep> path;
    istringstream in(text);
    string token;
    while (in >> token) {
        size_t dash = token.find('-');
        if (dash == string::npos) {
            throw runtime_error("path entry is missing '-' separator: " + token);
        }
        int u = stoi(token.substr(0, dash));
        int v = stoi(token.substr(dash + 1));
        path.push_back({u, v});
    }
    return path;
}

int required_column(const map<string, int>& columns, const string& name) {
    auto it = columns.find(name);
    if (it == columns.end()) {
        throw runtime_error("input CSV is missing required column: " + name);
    }
    return it->second;
}

} // namespace
} // namespace qk

int main(int argc, char** argv) {
    using namespace qk;

    if (argc < 3) {
        print_usage(argv[0]);
        return 2;
    }

    filesystem::path input_path = argv[1];
    filesystem::path output_path = argv[2];
    string case_name_filter = "-";
    PathOptimizerOptions options;
    options.enabled = true;
    options.max_window = 32;
    options.passes = 1;
    options.node_cap = 200000;
    options.segment_time_sec = 0.25;
    options.worker_threads = 16;
    options.window_stride = 0;
    options.word_reduction = true;

    try {
        if (argc > 3) case_name_filter = argv[3];
        if (argc > 4) options.max_window = stoi(argv[4]);
        if (argc > 5) options.passes = stoi(argv[5]);
        if (argc > 6) options.node_cap = stoull(argv[6]);
        if (argc > 7) options.segment_time_sec = stod(argv[7]);
        if (argc > 8) options.worker_threads = stoi(argv[8]);
        if (argc > 9) options.window_stride = stoi(argv[9]);
        if (argc > 10) options.word_reduction = stoi(argv[10]) != 0;
    } catch (const exception& e) {
        cerr << "Argument parse error: " << e.what() << "\n";
        print_usage(argv[0]);
        return 2;
    }

    if (options.max_window <= 1 || options.passes <= 0 ||
        options.segment_time_sec < 0.0 || options.worker_threads <= 0 ||
        options.window_stride < 0) {
        cerr << "path optimizer arguments are out of range.\n";
        return 2;
    }

    ifstream input(input_path);
    if (!input) {
        cerr << "Cannot open input CSV: " << input_path.generic_string() << "\n";
        return 2;
    }
    if (!output_path.parent_path().empty()) {
        filesystem::create_directories(output_path.parent_path());
    }
    ofstream output(output_path);
    if (!output) {
        cerr << "Cannot open output CSV: " << output_path.generic_string() << "\n";
        return 2;
    }

    string header_line;
    if (!getline(input, header_line)) {
        cerr << "Input CSV is empty.\n";
        return 2;
    }
    vector<string> headers = parse_csv_line(header_line);
    map<string, int> columns;
    for (int i = 0; i < static_cast<int>(headers.size()); ++i) {
        columns[headers[i]] = i;
    }

    int dim_col = required_column(columns, "dim");
    int case_col = required_column(columns, "case_name");
    int state_col = required_column(columns, "state_perm");
    int beam_path_col = required_column(columns, "beam_path");

    output << "dim,case_name,original_steps,optimized_status,optimized_steps,"
           << "optimized_improvement,optimized_word_reductions,optimized_attempts,optimized_segments,"
           << "optimized_expanded,optimized_states,optimized_sec,"
           << "original_path_valid,optimized_path_valid,state_perm,original_path,optimized_path\n";

    cout << "Qk path optimizer\n"
         << "input=" << input_path.generic_string()
         << ", output=" << output_path.generic_string()
         << ", case_name_filter=" << case_name_filter
         << ", path_opt_window=" << options.max_window
         << ", path_opt_passes=" << options.passes
         << ", path_opt_node_cap=" << options.node_cap
         << ", path_opt_segment_time_sec=" << options.segment_time_sec
         << ", path_opt_threads=" << options.worker_threads
         << ", path_opt_stride=" << options.window_stride
         << ", path_opt_word_reduce=" << (options.word_reduction ? 1 : 0) << "\n";

    string line;
    int processed = 0;
    while (getline(input, line)) {
        if (line.empty()) {
            continue;
        }
        vector<string> fields = parse_csv_line(line);
        if (static_cast<int>(fields.size()) <= max({dim_col, case_col, state_col, beam_path_col})) {
            cerr << "Skipping malformed row with too few columns.\n";
            continue;
        }

        string case_name = fields[case_col];
        if (case_name_filter != "-" && !case_name_filter.empty() && case_name != case_name_filter) {
            continue;
        }

        int dim = stoi(fields[dim_col]);
        int n = node_count(dim);
        State state = parse_state_perm(fields[state_col], n);
        vector<SwapStep> original_path = parse_path(fields[beam_path_col]);
        bool original_valid = path_reaches_goal(state, original_path, dim);

        PathOptimizerResult optimized;
        if (original_valid) {
            optimized = optimize_path_shortcuts(
                state,
                edges(dim),
                original_path,
                options
            );
        } else {
            optimized.status = "invalid_original";
            optimized.original_steps = static_cast<int>(original_path.size());
            optimized.optimized_steps = optimized.original_steps;
        }

        bool optimized_valid =
            (optimized.optimized_steps == 0 ||
             (optimized.optimized_steps > 0 && path_reaches_goal(state, optimized.swaps, dim)));

        output << dim << "," << csv_escape(case_name) << ","
               << original_path.size() << ","
               << optimized.status << ","
               << optimized.optimized_steps << ","
               << optimized.improvement << ","
               << optimized.word_reductions << ","
               << optimized.attempts << ","
               << optimized.improved_segments << ","
               << optimized.expanded << ","
               << optimized.reached_states << ","
               << fixed << optimized.sec << ","
               << (original_valid ? 1 : 0) << ","
               << (optimized_valid ? 1 : 0) << ","
               << csv_escape(state_perm(state)) << ","
               << csv_escape(path_string(original_path)) << ","
               << csv_escape(path_string(optimized.swaps)) << "\n";
        output.flush();

        processed++;
        cout << "  Q" << dim << " " << case_name
             << " original=" << original_path.size()
             << " optimized=" << optimized.optimized_steps
             << " improvement=" << optimized.improvement
             << " status=" << optimized.status
             << " valid=" << (optimized_valid ? 1 : 0) << "\n";
    }

    cout << "processed=" << processed << "\n";
    return 0;
}
