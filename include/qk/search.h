#pragma once

#include "qk/common.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace qk {

PathResult astar_exact(
    const State& init,
    const std::vector<Edge>& es,
    std::size_t node_cap,
    Clock::time_point deadline,
    bool record_path,
    const ProgressContext& progress,
    double time_limit_sec
);

PathResult beam_search(
    const State& init,
    const std::vector<Edge>& es,
    int beam_width,
    int max_depth,
    bool record_path,
    const ProgressContext& progress,
    std::ofstream* depth_progress_csv,
    std::ofstream* candidate_trace_csv,
    CandidateTraceMode trace_mode,
    BeamVisitedMode visited_mode
);

PathResult beam_search_disk(
    const State& init,
    const std::vector<Edge>& es,
    int beam_width,
    int max_depth,
    bool record_path,
    int worker_threads,
    std::size_t bloom_mb,
    std::size_t disk_batch_size,
    int disk_shards,
    const std::filesystem::path& visited_database_path,
    const ProgressContext& progress,
    std::ofstream* depth_progress_csv,
    std::ofstream* candidate_trace_csv,
    std::ofstream* disk_progress_csv,
    CandidateTraceMode trace_mode
);

std::string beam_visited_mode_name(BeamVisitedMode mode);
BeamVisitedMode parse_beam_visited_mode(std::string value);

} // namespace qk
