#include "qk/batcher.h"
#include "qk/cases.h"
#include "qk/hypercube.h"
#include "qk/io.h"
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
         << " [disk_bloom_mb=1024] [disk_batch_size=1000000] [disk_shards=16]\n"
         << "max_depth=0 uses auto depth dim * 2^dim for each Qdim.\n";
    cerr << "Use custom_cases_csv=- to select built-in cases while passing later options.\n";
    cerr << "candidate_trace_mode: 0=none, 1=retained beam only, 2=all new candidates.\n";
    cerr << "beam_visited_mode: exact/0 stores full packed states (Q1..Q8); "
         << "fingerprint128/1 stores fingerprints in RAM; "
         << "fingerprint128_disk/2 stores exact fingerprints in SQLite.\n";
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
    } catch (const exception& e) {
        cerr << "Argument parse error: " << e.what() << "\n";
        print_usage(argv[0]);
        return 2;
    }

    if (min_dim < 1 || max_dim < min_dim || max_dim > 9) {
        cerr << "Dimensions must satisfy 1 <= min_dim <= max_dim <= 9.\n";
        return 2;
    }
    if (exact_max_dim < 0 || exact_max_dim > 8) {
        cerr << "exact_max_dim must satisfy 0 <= exact_max_dim <= 8.\n";
        return 2;
    }
    if (beam_visited_mode == BeamVisitedMode::ExactPacked && max_dim > 8) {
        cerr << "exact Beam visited supports only Q1..Q8; use fingerprint128 for Q9.\n";
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
    if (candidate_trace_mode_arg < 0 || candidate_trace_mode_arg > 2) {
        cerr << "candidate_trace_mode must be 0, 1, or 2.\n";
        return 2;
    }
    CandidateTraceMode candidate_trace_mode = static_cast<CandidateTraceMode>(candidate_trace_mode_arg);

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
        << "state_perm,beam_path,batcher_path\n";

    ofstream checkpoint_csv(checkpoint_csv_path);
    checkpoint_csv << "completed_case,total_cases,dim,case_name,total_dist,strong_lb,"
                   << "exact_status,exact_steps,exact_sec,exact_path_valid,"
                   << "beam_visited_mode,beam_status,beam_steps,beam_sec,beam_path_valid,"
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
                beam = beam_search(
                    c.state,
                    es,
                    beam_width,
                    dim_max_depth,
                    true,
                    progress,
                    &beam_depth_csv,
                    &beam_candidate_trace_csv,
                    candidate_trace_mode,
                    beam_visited_mode
                );
            }
            trim_process_memory();
            print_progress_line(progress, "batcher", 1, 1, 0);
            BatcherResult batcher = batcher_baseline(c.state, true);

            bool exact_valid = exact.steps == 0 || (exact.steps > 0 && path_reaches_goal(c.state, exact.swaps, dim));
            if (!run_exact) {
                exact_valid = false;
            }
            bool beam_valid = beam.steps == 0 || (beam.steps > 0 && path_reaches_goal(c.state, beam.swaps, dim));
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
                << csv_escape(state_perm(c.state)) << ","
                << csv_escape(path_string(beam.swaps)) << ","
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
