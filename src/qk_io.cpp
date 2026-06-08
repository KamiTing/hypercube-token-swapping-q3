#include "qk/io.h"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <malloc.h>
#include <windows.h>
#endif

using namespace std;

namespace qk {

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

string progress_bar(long long current, long long total, int width) {
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

} // namespace qk
