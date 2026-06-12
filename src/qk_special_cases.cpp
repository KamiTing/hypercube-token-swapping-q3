#include "qk/batcher.h"
#include "qk/cases.h"
#include "qk/hypercube.h"
#include "qk/io.h"
#include "qk/path_optimizer.h"
#include "qk/search.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef far
#undef far
#endif
#ifdef near
#undef near
#endif
#endif

using namespace std;

namespace qk {

void print_usage(const char* exe) {
    cerr << "Usage: " << exe
         << " [min_dim=4] [max_dim=6] [beam_width=256] [max_depth=0]"
         << " [exact_max_dim=4] [astar_cap=2000000] [exact_time_sec=30]"
         << " [output_dir=output/qk_special_cases] [custom_cases_csv] [candidate_trace_mode=2]"
         << " [case_name_filter] [beam_visited_mode=exact] [worker_threads=24]"
         << " [disk_bloom_mb=1024] [disk_batch_size=1000000] [disk_shards=16]"
         << " [path_opt_window=0] [path_opt_passes=1] [path_opt_node_cap=200000]"
         << " [path_opt_segment_time_sec=0.25] [path_opt_threads=worker_threads]"
         << " [path_opt_stride=0] [path_opt_word_reduce=1]"
         << " [layer_pool_width=0] [layer_visited_window=0]"
         << " [layer_restart_max_width=0] [layer_plateau_limit=0] [layer_restart_growth=2]"
         << " [layer_perturbation_ratio=0.0]\n"
         << "max_depth=0 uses auto depth dim * 2^dim for each Qdim.\n";
    cerr << "Use custom_cases_csv=- to select built-in cases while passing later options.\n";
    cerr << "candidate_trace_mode: 0=none, 1=retained beam only, 2=all new candidates.\n";
    cerr << "beam_visited_mode: exact/0 stores full packed states (Q1..Q8); "
         << "fingerprint128/1 stores fingerprints in RAM; "
         << "fingerprint128_disk/2 stores exact fingerprints in SQLite; "
         << "layer_only/3 keeps only retained-layer fingerprints and does not keep global visited states.\n";
    cerr << "layer_pool_width: only for layer_only; 0 uses the default small oversampling pool, "
         << "otherwise this sets the per-layer candidate pool before deduping down to beam_width.\n";
    cerr << "layer_visited_window: only for layer_only; records retained fingerprints from the previous K layers.\n";
    cerr << "layer_restart_max_width/layer_plateau_limit/layer_restart_growth: only for layer_only; "
         << "when best total distance does not improve for plateau_limit layers, restart from the initial state "
         << "with a larger beam width up to restart_max_width.\n";
    cerr << "layer_perturbation_ratio: only for layer_only; fraction of retained beam slots selected by "
         << "deterministic hash perturbation from the candidate pool instead of pure greedy ranking.\n";
}

} // namespace qk

int main(int argc, char** argv) {
    using namespace qk;

    int min_dim = 4;
    int max_dim = 6;
    int beam_width = 256;
    int max_depth = 0;
    int exact_max_dim = 4;
    size_t astar_cap = 2000000;
    double exact_time_sec = 30.0;
    filesystem::path output_dir = "output/qk_special_cases";
    filesystem::path custom_cases_path;
    bool use_custom_cases = false;
    int candidate_trace_mode_arg = static_cast<int>(CandidateTraceMode::AllNewCandidates);
    string case_name_filter;
    BeamVisitedMode beam_visited_mode = BeamVisitedMode::ExactPacked;
    int worker_threads = 24;
    size_t disk_bloom_mb = 1024;
    size_t disk_batch_size = 1000000;
    int disk_shards = 16;
    int path_opt_window = 0;
    int path_opt_passes = 1;
    size_t path_opt_node_cap = 200000;
    double path_opt_segment_time_sec = 0.25;
    int path_opt_threads = 0;
    int path_opt_stride = 0;
    bool path_opt_word_reduce = true;
    int layer_pool_width = 0;
    int layer_visited_window = 0;
    int layer_restart_max_width = 0;
    int layer_plateau_limit = 0;
    int layer_restart_growth = 2;
    double layer_perturbation_ratio = 0.0;

    try {
        if (argc > 1) min_dim = stoi(argv[1]);
        if (argc > 2) max_dim = stoi(argv[2]);
        if (argc > 3) beam_width = stoi(argv[3]);
        if (argc > 4) max_depth = stoi(argv[4]);
        if (argc > 5) exact_max_dim = stoi(argv[5]);
        if (argc > 6) astar_cap = stoull(argv[6]);
        if (argc > 7) exact_time_sec = stod(argv[7]);
        if (argc > 8) output_dir = argv[8];
        if (argc > 9) {
            string custom_cases_arg = argv[9];
            if (!custom_cases_arg.empty() && custom_cases_arg != "-") {
                custom_cases_path = custom_cases_arg;
                use_custom_cases = true;
            }
        }
        if (argc > 10) candidate_trace_mode_arg = stoi(argv[10]);
        if (argc > 11) case_name_filter = argv[11];
        if (argc > 12) beam_visited_mode = parse_beam_visited_mode(argv[12]);
        if (argc > 13) worker_threads = stoi(argv[13]);
        if (argc > 14) disk_bloom_mb = stoull(argv[14]);
        if (argc > 15) disk_batch_size = stoull(argv[15]);
        if (argc > 16) disk_shards = stoi(argv[16]);
        if (argc > 17) path_opt_window = stoi(argv[17]);
        if (argc > 18) path_opt_passes = stoi(argv[18]);
        if (argc > 19) path_opt_node_cap = stoull(argv[19]);
        if (argc > 20) path_opt_segment_time_sec = stod(argv[20]);
        if (argc > 21) path_opt_threads = stoi(argv[21]);
        if (argc > 22) path_opt_stride = stoi(argv[22]);
        if (argc > 23) path_opt_word_reduce = stoi(argv[23]) != 0;
        if (argc > 24) layer_pool_width = stoi(argv[24]);
        if (argc > 25) layer_visited_window = stoi(argv[25]);
        if (argc > 26) layer_restart_max_width = stoi(argv[26]);
        if (argc > 27) layer_plateau_limit = stoi(argv[27]);
        if (argc > 28) layer_restart_growth = stoi(argv[28]);
        if (argc > 29) layer_perturbation_ratio = stod(argv[29]);
        if (case_name_filter == "-") case_name_filter.clear();
    } catch (const exception& e) {
        cerr << "Argument parse error: " << e.what() << "\n";
        print_usage(argv[0]);
        return 2;
    }

    if (min_dim < 1 || max_dim < min_dim || max_dim > 15) {
        cerr << "Dimensions must satisfy 1 <= min_dim <= max_dim <= 15.\n";
        return 2;
    }
    if (exact_max_dim < 0 || exact_max_dim > 8) {
        cerr << "exact_max_dim must satisfy 0 <= exact_max_dim <= 8.\n";
        return 2;
    }
    if (beam_visited_mode == BeamVisitedMode::ExactPacked && max_dim > 8) {
        cerr << "exact Beam visited supports only Q1..Q8; use fingerprint128, fingerprint128_disk, or layer_only for larger cases.\n";
        return 2;
    }
    if (beam_width <= 0) {
        cerr << "beam_width must be positive.\n";
        return 2;
    }
    if (worker_threads <= 0) {
        cerr << "worker_threads must be positive.\n";
        return 2;
    }
    if (disk_bloom_mb == 0 || disk_batch_size == 0 || disk_shards <= 0) {
        cerr << "disk_bloom_mb, disk_batch_size, and disk_shards must be positive.\n";
        return 2;
    }
    if (path_opt_window < 0 || path_opt_passes <= 0 ||
        path_opt_segment_time_sec < 0.0 || path_opt_stride < 0) {
        cerr << "path_opt_window must be >= 0, path_opt_passes must be positive, "
             << "path_opt_segment_time_sec must be >= 0, and path_opt_stride must be >= 0.\n";
        return 2;
    }
    if (layer_pool_width < 0) {
        cerr << "layer_pool_width must be >= 0.\n";
        return 2;
    }
    if (layer_visited_window < 0) {
        cerr << "layer_visited_window must be >= 0.\n";
        return 2;
    }
    if (layer_restart_max_width < 0 || layer_plateau_limit < 0 || layer_restart_growth < 1) {
        cerr << "layer_restart_max_width and layer_plateau_limit must be >= 0; layer_restart_growth must be >= 1.\n";
        return 2;
    }
    if (layer_restart_max_width > 0 && layer_restart_max_width < beam_width) {
        cerr << "layer_restart_max_width must be 0 or >= beam_width.\n";
        return 2;
    }
    if (layer_perturbation_ratio < 0.0 || layer_perturbation_ratio > 1.0) {
        cerr << "layer_perturbation_ratio must be between 0.0 and 1.0.\n";
        return 2;
    }
    if (path_opt_threads <= 0) {
        path_opt_threads = worker_threads;
    }
    if (path_opt_threads <= 0) {
        cerr << "path_opt_threads must be positive.\n";
        return 2;
    }
    if (candidate_trace_mode_arg < 0 || candidate_trace_mode_arg > 2) {
        cerr << "candidate_trace_mode must be 0, 1, or 2.\n";
        return 2;
    }
    CandidateTraceMode candidate_trace_mode = static_cast<CandidateTraceMode>(candidate_trace_mode_arg);
    PathOptimizerOptions path_optimizer_options;
    path_optimizer_options.enabled = path_opt_window > 0;
    path_optimizer_options.max_window = path_opt_window;
    path_optimizer_options.passes = path_opt_passes;
    path_optimizer_options.node_cap = path_opt_node_cap;
    path_optimizer_options.segment_time_sec = path_opt_segment_time_sec;
    path_optimizer_options.worker_threads = path_opt_threads;
    path_optimizer_options.window_stride = path_opt_stride;
    path_optimizer_options.word_reduction = path_opt_word_reduce;

#ifdef _WIN32
    if (beam_visited_mode == BeamVisitedMode::Fingerprint128Disk) {
        SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    }
#endif

    map<int, vector<SpecialCase>> cases_by_dim;
    try {
        if (use_custom_cases) {
            map<int, vector<SpecialCase>> loaded = read_custom_cases(custom_cases_path);
            for (auto& entry : loaded) {
                int dim = entry.first;
                if (dim >= min_dim && dim <= max_dim) {
                    cases_by_dim[dim] = move(entry.second);
                }
            }
        } else {
            for (int dim = min_dim; dim <= max_dim; ++dim) {
                cases_by_dim[dim] = make_cases(dim);
            }
        }
    } catch (const exception& e) {
        cerr << "Custom case load error: " << e.what() << "\n";
        return 2;
    }

    if (!case_name_filter.empty()) {
        map<int, vector<SpecialCase>> filtered;
        for (auto& entry : cases_by_dim) {
            vector<SpecialCase> kept;
            for (auto& c : entry.second) {
                if (c.name == case_name_filter) {
                    kept.push_back(move(c));
                }
            }
            if (!kept.empty()) {
                filtered[entry.first] = move(kept);
            }
        }
        cases_by_dim = move(filtered);
    }

    if (cases_by_dim.empty()) {
        cerr << "No cases to run after applying the dimension filter.\n";
        return 2;
    }

    filesystem::create_directories(output_dir);
    const filesystem::path csv_path = output_dir / "qk_special_cases.csv";
    const filesystem::path checkpoint_csv_path = output_dir / "qk_case_progress.csv";
    const filesystem::path beam_depth_csv_path = output_dir / "qk_beam_depth_progress.csv";
    const filesystem::path beam_candidate_trace_csv_path = output_dir / "qk_beam_candidate_trace.csv";
    const filesystem::path beam_disk_progress_csv_path = output_dir / "qk_beam_disk_progress.csv";
    ofstream csv(csv_path);
    csv << "dim,n,case_name,note,total_dist,misplaced,basic_lb,strong_lb,parity,"
        << "exact_status,exact_steps,exact_expanded,exact_states,exact_sec,exact_path_valid,"
        << "beam_visited_mode,beam_status,beam_steps,beam_expanded,beam_states,beam_sec,beam_path_valid,beam_gap_vs_exact,"
        << "batcher_success,batcher_swaps,batcher_compares,batcher_rounds,batcher_sec,batcher_path_valid,batcher_gap_vs_exact,"
        << "optimized_status,optimized_steps,optimized_improvement,optimized_word_reductions,"
        << "optimized_attempts,optimized_segments,"
        << "optimized_expanded,optimized_states,optimized_sec,optimized_path_valid,"
        << "state_perm,beam_path,optimized_path,batcher_path\n";

    ofstream checkpoint_csv(checkpoint_csv_path);
    checkpoint_csv << "completed_case,total_cases,dim,case_name,total_dist,strong_lb,"
                   << "exact_status,exact_steps,exact_sec,exact_path_valid,"
                   << "beam_visited_mode,beam_status,beam_steps,beam_sec,beam_path_valid,"
                   << "optimized_status,optimized_steps,optimized_improvement,optimized_word_reductions,"
                   << "optimized_sec,optimized_path_valid,"
                   << "batcher_success,batcher_swaps,batcher_sec,batcher_path_valid,"
                   << "elapsed_sec\n";

    ofstream beam_depth_csv(beam_depth_csv_path);
    beam_depth_csv << "case_index,total_cases,dim,case_name,depth,max_depth,"
                   << "expanded,layer_candidates,retained,best_total_dist,best_misplaced,best_max_dist,elapsed_sec\n";

    ofstream beam_candidate_trace_csv(beam_candidate_trace_csv_path);
    beam_candidate_trace_csv << "case_index,total_cases,dim,case_name,trace_kind,depth,candidate_id,"
                             << "total_dist,misplaced,max_dist,edge_id,path_len,state_perm,path\n";

    ofstream beam_disk_progress_csv(beam_disk_progress_csv_path);
    beam_disk_progress_csv << "case_index,total_cases,dim,case_name,depth,visited_states,"
                           << "pending_states,disk_bytes,worker_threads,generation_sec,"
                           << "visited_sec,selection_sec\n";

    cout << "Qk special-case benchmark\n";
    cout << "dims=Q" << min_dim << "..Q" << max_dim
         << ", beam_width=" << beam_width
         << ", max_depth=" << (max_depth > 0 ? to_string(max_depth) : string("auto"))
         << ", exact_max_dim=Q" << exact_max_dim
         << ", astar_cap=" << astar_cap
         << ", exact_time_sec=" << fixed << setprecision(0) << exact_time_sec
         << ", candidate_trace_mode=" << candidate_trace_mode_arg
         << ", beam_visited_mode=" << beam_visited_mode_name(beam_visited_mode)
         << ", worker_threads=" << worker_threads
         << ", disk_bloom_mb=" << disk_bloom_mb
         << ", disk_batch_size=" << disk_batch_size
         << ", disk_shards=" << disk_shards
         << ", path_opt_window=" << path_opt_window
         << ", path_opt_passes=" << path_opt_passes
         << ", path_opt_node_cap=" << path_opt_node_cap
         << ", path_opt_segment_time_sec=" << setprecision(3) << path_opt_segment_time_sec
         << setprecision(0)
         << ", path_opt_threads=" << path_opt_threads
         << ", path_opt_stride=" << path_opt_stride
         << ", path_opt_word_reduce=" << (path_opt_word_reduce ? 1 : 0)
         << ", layer_pool_width=" << layer_pool_width
         << ", layer_visited_window=" << layer_visited_window
         << ", layer_restart_max_width=" << layer_restart_max_width
         << ", layer_plateau_limit=" << layer_plateau_limit
         << ", layer_restart_growth=" << layer_restart_growth
         << ", layer_perturbation_ratio=" << fixed << setprecision(3) << layer_perturbation_ratio
         << ", output_dir=" << output_dir.generic_string() << "\n";
    if (use_custom_cases) {
        cout << "custom_cases=" << custom_cases_path.generic_string() << "\n";
    }
    if (!case_name_filter.empty()) {
        cout << "case_name_filter=" << case_name_filter << "\n";
    }

    int total_cases = 0;
    for (const auto& entry : cases_by_dim) {
        total_cases += static_cast<int>(entry.second.size());
    }
    int completed_cases = 0;
    auto run_start = Clock::now();

    for (const auto& dim_cases : cases_by_dim) {
        int dim = dim_cases.first;
        const auto es = edges(dim);
        const auto& cases = dim_cases.second;
        int dim_max_depth = max_depth > 0 ? max_depth : dim * node_count(dim);
        int exact_solved = 0;
        int beam_solved = 0;
        int batcher_solved = 0;

        cout << "\nQ" << dim << " cases=" << cases.size()
             << ", beam_max_depth=" << dim_max_depth << "\n";

        for (const auto& c : cases) {
            ProgressContext progress{true, completed_cases + 1, total_cases, dim, c.name};
            int n = node_count(dim);
            int td = total_distance(c.state);
            int misplaced = misplaced_count(c.state);
            int basic_lb = basic_lower_bound(c.state);
            int strong_lb = strong_lower_bound(c.state);
            int parity = permutation_parity(c.state);

            PathResult exact;
            bool run_exact = dim <= exact_max_dim;
            if (run_exact) {
                auto deadline = Clock::now() + chrono::milliseconds(static_cast<long long>(exact_time_sec * 1000.0));
                print_progress_line(progress, "strong_astar_time", 0, max<long long>(1, static_cast<long long>(exact_time_sec * 1000.0)), 0);
                exact = astar_exact(c.state, es, astar_cap, deadline, true, progress, exact_time_sec);
            }

            PathResult beam;
            if (beam_visited_mode == BeamVisitedMode::Fingerprint128Disk) {
                filesystem::path visited_database_path =
                    output_dir /
                    (
                        "q" + to_string(dim) + "_" +
                        safe_filename(c.name) +
                        "_beam_visited.sqlite3"
                    );
                beam = beam_search_disk(
                    c.state,
                    es,
                    beam_width,
                    dim_max_depth,
                    true,
                    worker_threads,
                    disk_bloom_mb,
                    disk_batch_size,
                    disk_shards,
                    visited_database_path,
                    progress,
                    &beam_depth_csv,
                    &beam_candidate_trace_csv,
                    &beam_disk_progress_csv,
                    candidate_trace_mode
                );
            } else {
                int active_beam_width = beam_width;
                PathResult combined_beam;
                bool has_combined_beam = false;

                while (true) {
                    PathResult attempt = beam_search(
                        c.state,
                        es,
                        active_beam_width,
                        dim_max_depth,
                        true,
                        progress,
                        &beam_depth_csv,
                        &beam_candidate_trace_csv,
                        candidate_trace_mode,
                        beam_visited_mode,
                        worker_threads,
                        layer_pool_width,
                        layer_visited_window,
                        layer_plateau_limit,
                        layer_perturbation_ratio
                    );

                    string attempt_status = attempt.status;
                    long long attempt_expanded = attempt.expanded;
                    double attempt_sec = attempt.sec;

                    if (!has_combined_beam) {
                        combined_beam = move(attempt);
                        has_combined_beam = true;
                    } else {
                        combined_beam.expanded += attempt.expanded;
                        combined_beam.reached_states += attempt.reached_states;
                        combined_beam.sec += attempt.sec;
                        combined_beam.steps = attempt.steps;
                        combined_beam.status = move(attempt.status);
                        combined_beam.swaps = move(attempt.swaps);
                    }

                    bool can_restart =
                        beam_visited_mode == BeamVisitedMode::LayerOnly &&
                        layer_plateau_limit > 0 &&
                        layer_restart_max_width > active_beam_width &&
                        attempt_status == "plateau";
                    if (!can_restart) {
                        beam = move(combined_beam);
                        break;
                    }

                    int next_beam_width = static_cast<int>(min<long long>(
                        layer_restart_max_width,
                        max<long long>(
                            static_cast<long long>(active_beam_width) + 1,
                            static_cast<long long>(active_beam_width) * layer_restart_growth
                        )
                    ));
                    clear_progress_line();
                    cout << "    beam plateau: width=" << active_beam_width
                         << ", expanded=" << attempt_expanded
                         << ", sec=" << fixed << setprecision(3) << attempt_sec
                         << "; restart width=" << next_beam_width << "\n";
                    active_beam_width = next_beam_width;
                    trim_process_memory();
                }
            }
            trim_process_memory();

            PathOptimizerResult optimized;
            if (path_optimizer_options.enabled && beam.status == "solved") {
                print_progress_line(
                    progress,
                    "path_opt",
                    0,
                    max<long long>(1, beam.steps),
                    beam.expanded
                );
                optimized = optimize_path_shortcuts(
                    c.state,
                    es,
                    beam.swaps,
                    path_optimizer_options
                );
                trim_process_memory();
            } else {
                optimized.status = path_optimizer_options.enabled ? "skipped" : "disabled";
                optimized.original_steps = beam.steps;
                optimized.optimized_steps = beam.steps;
                if (beam.status == "solved") {
                    optimized.swaps = beam.swaps;
                }
            }

            print_progress_line(progress, "batcher", 1, 1, 0);
            BatcherResult batcher = batcher_baseline(c.state, true);

            bool exact_valid = exact.steps == 0 || (exact.steps > 0 && path_reaches_goal(c.state, exact.swaps, dim));
            if (!run_exact) {
                exact_valid = false;
            }
            bool beam_valid = beam.steps == 0 || (beam.steps > 0 && path_reaches_goal(c.state, beam.swaps, dim));
            bool optimized_valid =
                path_optimizer_options.enabled &&
                beam.status == "solved" &&
                (optimized.optimized_steps == 0 ||
                 (optimized.optimized_steps > 0 && path_reaches_goal(c.state, optimized.swaps, dim)));
            bool batcher_valid = batcher.success && path_reaches_goal(c.state, batcher.path, dim);

            if (exact.status == "solved") {
                exact_solved++;
            }
            if (beam.status == "solved") {
                beam_solved++;
            }
            if (batcher.success) {
                batcher_solved++;
            }

            int beam_gap = (exact.status == "solved" && beam.status == "solved") ? beam.steps - exact.steps : 0;
            int batcher_gap = (exact.status == "solved" && batcher.success) ? batcher.swaps - exact.steps : 0;

            completed_cases++;
            clear_progress_line();
            cout << "  " << left << setw(30) << c.name
                 << " lb=" << right << setw(3) << strong_lb
                 << " exact=" << setw(10) << exact.status
                 << "(" << setw(3) << exact.steps << ")"
                 << " beam=" << setw(7) << beam.status
                 << "(" << setw(3) << beam.steps << ")"
                 << " opt=" << setw(14) << optimized.status
                 << "(" << setw(3) << optimized.optimized_steps << ",-" << optimized.improvement << ")"
                 << " batcher=" << (batcher.success ? "ok" : "fail")
                 << "(" << batcher.swaps << ")\n";

            csv << dim << "," << n << ","
                << csv_escape(c.name) << "," << csv_escape(c.note) << ","
                << td << "," << misplaced << "," << basic_lb << "," << strong_lb << "," << parity << ","
                << exact.status << "," << exact.steps << "," << exact.expanded << "," << exact.reached_states << ","
                << fixed << setprecision(6) << exact.sec << "," << (exact_valid ? 1 : 0) << ","
                << beam_visited_mode_name(beam_visited_mode) << ","
                << beam.status << "," << beam.steps << "," << beam.expanded << "," << beam.reached_states << ","
                << beam.sec << "," << (beam_valid ? 1 : 0) << "," << beam_gap << ","
                << (batcher.success ? 1 : 0) << "," << batcher.swaps << "," << batcher.compares << "," << batcher.rounds << ","
                << batcher.sec << "," << (batcher_valid ? 1 : 0) << "," << batcher_gap << ","
                << optimized.status << "," << optimized.optimized_steps << "," << optimized.improvement << ","
                << optimized.word_reductions << ","
                << optimized.attempts << "," << optimized.improved_segments << ","
                << optimized.expanded << "," << optimized.reached_states << "," << optimized.sec << ","
                << (optimized_valid ? 1 : 0) << ","
                << csv_escape(state_perm(c.state)) << ","
                << csv_escape(path_string(beam.swaps)) << ","
                << csv_escape(
                    path_optimizer_options.enabled && beam.status == "solved"
                        ? path_string(optimized.swaps)
                        : string()
                ) << ","
                << csv_escape(path_string(batcher.path)) << "\n";
            csv.flush();

            double elapsed_sec = chrono::duration<double>(Clock::now() - run_start).count();
            checkpoint_csv << completed_cases << "," << total_cases << ","
                           << dim << "," << csv_escape(c.name) << ","
                           << td << "," << strong_lb << ","
                           << exact.status << "," << exact.steps << ","
                           << fixed << setprecision(6) << exact.sec << "," << (exact_valid ? 1 : 0) << ","
                           << beam_visited_mode_name(beam_visited_mode) << ","
                           << beam.status << "," << beam.steps << "," << beam.sec << "," << (beam_valid ? 1 : 0) << ","
                           << optimized.status << "," << optimized.optimized_steps << "," << optimized.improvement << ","
                           << optimized.word_reductions << ","
                           << optimized.sec << "," << (optimized_valid ? 1 : 0) << ","
                           << (batcher.success ? 1 : 0) << "," << batcher.swaps << "," << batcher.sec << ","
                           << (batcher_valid ? 1 : 0) << "," << elapsed_sec << "\n";
            checkpoint_csv.flush();
            trim_process_memory();
        }

        cout << "Q" << dim << " summary: exact_solved=" << exact_solved << "/" << cases.size()
             << ", beam_solved=" << beam_solved << "/" << cases.size()
             << ", batcher_solved=" << batcher_solved << "/" << cases.size() << "\n";
    }

    cout << "\nCSV: " << csv_path.generic_string() << "\n";
    cout << "Checkpoint CSV: " << checkpoint_csv_path.generic_string() << "\n";
    cout << "Beam depth CSV: " << beam_depth_csv_path.generic_string() << "\n";
    cout << "Beam candidate trace CSV: " << beam_candidate_trace_csv_path.generic_string() << "\n";
    cout << "Beam disk progress CSV: " << beam_disk_progress_csv_path.generic_string() << "\n";
    return 0;
}
