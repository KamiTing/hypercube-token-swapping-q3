#pragma once

#include "qk/common.h"

#include <string>
#include <vector>

namespace qk {

std::string csv_escape(const std::string& value);
std::string state_perm(const State& s);
std::string path_string(const std::vector<SwapStep>& swaps);
std::string progress_bar(long long current, long long total, int width = 24);
void print_progress_line(
    const ProgressContext& progress,
    const std::string& phase,
    long long current,
    long long total,
    long long expanded
);
void clear_progress_line();
void trim_process_memory();

} // namespace qk
