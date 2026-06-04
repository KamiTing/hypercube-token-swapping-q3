#pragma once

#include <array>
#include <string>

#include "config.h"

double safe_div(double a, double b);
double percent(long long part, long long total);

void write_distribution_csv(
    const std::array<long long, cfg::MAX_STEP_BUCKET>& bfs_counter,
    const std::array<long long, cfg::MAX_STEP_BUCKET>& astar_counter,
    const std::array<long long, cfg::MAX_STEP_BUCKET>& strong_astar_counter,
    const std::array<long long, cfg::MAX_STEP_BUCKET>& beam_counter,
    const std::array<long long, cfg::MAX_STEP_BUCKET>& batcher_counter,
    const std::string& filename
);

void write_text_report(
    const std::string& filename,
    int total_cases,
    int edge_count,
    int max_threads,
    double total_elapsed,
    double total_bfs_time,
    double total_astar_time,
    double total_strong_astar_time,
    double total_beam_time,
    double total_batcher_time,
    long long astar_match_bfs,
    long long astar_mismatch,
    long long strong_astar_match_bfs,
    long long strong_astar_mismatch,
    long long beam_success,
    long long beam_failed,
    long long beam_match_bfs,
    long long beam_not_optimal,
    long long batcher_success,
    long long batcher_failed,
    long long batcher_match_bfs,
    long long batcher_not_optimal,
    long long total_batcher_compares,
    long long total_batcher_swaps,
    long long total_batcher_rounds,
    const std::array<long long, cfg::MAX_STEP_BUCKET>& bfs_counter,
    const std::array<long long, cfg::MAX_STEP_BUCKET>& astar_counter,
    const std::array<long long, cfg::MAX_STEP_BUCKET>& strong_astar_counter,
    const std::array<long long, cfg::MAX_STEP_BUCKET>& beam_counter,
    const std::array<long long, cfg::MAX_STEP_BUCKET>& batcher_counter
);

void print_ascii_distribution(const std::array<long long, cfg::MAX_STEP_BUCKET>& counter);
