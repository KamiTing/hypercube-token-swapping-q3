#include "qk/cases.h"

#include "qk/hypercube.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

using namespace std;

namespace qk {

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
        if (dim < 1 || dim > 15) {
            ostringstream msg;
            msg << "line " << line_no << ": supported custom CSV dimensions are Q1..Q15, got Q" << dim;
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


} // namespace qk
