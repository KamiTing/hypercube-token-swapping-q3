#include <array>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include <omp.h>

#include "batcher.h"
#include "config.h"
#include "hypercube.h"
#include "report.h"
#include "search.h"

using namespace std;

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

string state_to_hex(uint64_t state) {
    ostringstream out;
    out << "0x" << uppercase << hex << setw(8) << setfill('0') << state;
    return out.str();
}

string state_to_perm(uint64_t state) {
    ostringstream out;
    for (int node = 0; node < cfg::NODE_COUNT; ++node) {
        if (node > 0) {
            out << ' ';
        }
        out << get_packet(state, node);
    }
    return out.str();
}

string path_to_string(const vector<SwapStep>& swaps) {
    ostringstream out;
    for (size_t i = 0; i < swaps.size(); ++i) {
        if (i > 0) {
            out << ' ';
        }
        out << swaps[i].u << swaps[i].v;
    }
    return out.str();
}

string make_routing_path_row(
    int case_id,
    uint64_t state,
    const PathResult& bfs_path,
    const PathResult& astar_path,
    const PathResult& strong_astar_path,
    const PathResult& beam_path,
    const BatcherPathResult& batcher_path
) {
    ostringstream row;
    row << case_id << ","
        << state_to_hex(state) << ","
        << csv_escape(state_to_perm(state)) << ","
        << bfs_path.steps << "," << csv_escape(path_to_string(bfs_path.swaps)) << ","
        << astar_path.steps << "," << csv_escape(path_to_string(astar_path.swaps)) << ","
        << strong_astar_path.steps << "," << csv_escape(path_to_string(strong_astar_path.swaps)) << ","
        << beam_path.steps << "," << csv_escape(path_to_string(beam_path.swaps)) << ","
        << batcher_path.swap_count << "," << csv_escape(path_to_string(batcher_path.swaps)) << ","
        << (batcher_path.success ? 1 : 0);
    return row.str();
}

void write_routing_paths_csv(const vector<string>& rows, const string& filename) {
    ofstream fout(filename);
    fout << "case_id,state_hex,state_perm,"
         << "bfs_steps,bfs_path,"
         << "astar_steps,astar_path,"
         << "strong_astar_steps,strong_astar_path,"
         << "beam_steps,beam_path,"
         << "batcher_swaps,batcher_path,batcher_success\n";

    for (const string& row : rows) {
        fout << row << "\n";
    }
}

int main() {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);
    std::filesystem::create_directories("output");

    const vector<Edge> edges = generate_hypercube_edges();
    const vector<uint64_t> states = generate_all_states();
    const int total_cases = static_cast<int>(states.size());
    vector<string> routing_path_rows(total_cases);

    cout << "========================================\n";
    cout << "Q3 Hypercube Full Permutation Test\n";
    cout << "========================================\n";
    cout << "Total states : " << total_cases << "\n";
    cout << "Edges        : " << edges.size() << "\n";
    cout << "OpenMP max threads : " << omp_get_max_threads() << "\n";
    cout << "RUN_BFS      : " << cfg::RUN_BFS << "\n";
    cout << "RUN_ASTAR    : " << cfg::RUN_ASTAR << "\n";
    cout << "RUN_STRONG_ASTAR : " << cfg::RUN_STRONG_ASTAR << "\n";
    cout << "RUN_BEAM     : " << cfg::RUN_BEAM << "\n";
    cout << "RUN_BATCHER  : " << cfg::RUN_BATCHER << "\n";
    cout << "Beam width   : " << cfg::BEAM_WIDTH << "\n";
    cout << "Max depth    : " << cfg::MAX_DEPTH << "\n\n";

    array<long long, cfg::MAX_STEP_BUCKET> bfs_counter{};
    array<long long, cfg::MAX_STEP_BUCKET> astar_counter{};
    array<long long, cfg::MAX_STEP_BUCKET> strong_astar_counter{};
    array<long long, cfg::MAX_STEP_BUCKET> beam_counter{};
    array<long long, cfg::MAX_STEP_BUCKET> batcher_counter{};

    long long astar_match_bfs = 0, astar_mismatch = 0;
    long long strong_astar_match_bfs = 0, strong_astar_mismatch = 0;
    long long beam_success = 0, beam_match_bfs = 0, beam_not_optimal = 0, beam_failed = 0;
    long long batcher_success = 0, batcher_failed = 0, batcher_match_bfs = 0, batcher_not_optimal = 0;
    long long total_batcher_compares = 0, total_batcher_swaps = 0, total_batcher_rounds = 0;

    double total_bfs_time = 0.0, total_astar_time = 0.0, total_strong_astar_time = 0.0;
    double total_beam_time = 0.0, total_batcher_time = 0.0;
    double start_all = omp_get_wtime();
    int progress_counter = 0;

#pragma omp parallel
    {
        array<long long, cfg::MAX_STEP_BUCKET> local_bfs_counter{};
        array<long long, cfg::MAX_STEP_BUCKET> local_astar_counter{};
        array<long long, cfg::MAX_STEP_BUCKET> local_strong_astar_counter{};
        array<long long, cfg::MAX_STEP_BUCKET> local_beam_counter{};
        array<long long, cfg::MAX_STEP_BUCKET> local_batcher_counter{};

        long long local_astar_match_bfs = 0, local_astar_mismatch = 0;
        long long local_strong_astar_match_bfs = 0, local_strong_astar_mismatch = 0;
        long long local_beam_success = 0, local_beam_match_bfs = 0, local_beam_not_optimal = 0, local_beam_failed = 0;
        long long local_batcher_success = 0, local_batcher_failed = 0, local_batcher_match_bfs = 0, local_batcher_not_optimal = 0;
        long long local_total_batcher_compares = 0, local_total_batcher_swaps = 0, local_total_batcher_rounds = 0;

        double local_bfs_time = 0.0, local_astar_time = 0.0, local_strong_astar_time = 0.0;
        double local_beam_time = 0.0, local_batcher_time = 0.0;

#pragma omp for schedule(dynamic, 64)
        for (int i = 0; i < total_cases; ++i) {
            uint64_t state = states[i];
            int bfs_steps = -1, astar_steps = -1, strong_astar_steps = -1, beam_steps = -1;
            PathResult bfs_path;
            PathResult astar_path;
            PathResult strong_astar_path;
            PathResult beam_path;
            BatcherPathResult batcher_result;

            if constexpr (cfg::RUN_BFS) {
                double t0 = omp_get_wtime();
                bfs_path = bfs_shortest_path(state, edges);
                bfs_steps = bfs_path.steps;
                local_bfs_time += (omp_get_wtime() - t0);
                if (bfs_steps >= 0 && bfs_steps < cfg::MAX_STEP_BUCKET) local_bfs_counter[bfs_steps]++;
            }

            if constexpr (cfg::RUN_ASTAR) {
                double t0 = omp_get_wtime();
                astar_path = astar_shortest_path(state, edges);
                astar_steps = astar_path.steps;
                local_astar_time += (omp_get_wtime() - t0);
                if (astar_steps >= 0 && astar_steps < cfg::MAX_STEP_BUCKET) local_astar_counter[astar_steps]++;
            }

            if constexpr (cfg::RUN_STRONG_ASTAR) {
                double t0 = omp_get_wtime();
                strong_astar_path = strong_astar_shortest_path(state, edges);
                strong_astar_steps = strong_astar_path.steps;
                local_strong_astar_time += (omp_get_wtime() - t0);
                if (strong_astar_steps >= 0 && strong_astar_steps < cfg::MAX_STEP_BUCKET) {
                    local_strong_astar_counter[strong_astar_steps]++;
                }
            }

            if constexpr (cfg::RUN_BEAM) {
                double t0 = omp_get_wtime();
                beam_path = beam_search_path(state, edges, cfg::BEAM_WIDTH, cfg::MAX_DEPTH);
                beam_steps = beam_path.steps;
                local_beam_time += (omp_get_wtime() - t0);
                if (beam_steps >= 0) {
                    local_beam_success++;
                    if (beam_steps < cfg::MAX_STEP_BUCKET) local_beam_counter[beam_steps]++;
                } else {
                    local_beam_failed++;
                }
            }

            if constexpr (cfg::RUN_BATCHER) {
                double t0 = omp_get_wtime();
                batcher_result = batcher_merge_sort_path(state);
                local_batcher_time += (omp_get_wtime() - t0);

                local_total_batcher_compares += batcher_result.compare_count;
                local_total_batcher_swaps += batcher_result.swap_count;
                local_total_batcher_rounds += batcher_result.round_count;

                if (batcher_result.success) {
                    local_batcher_success++;
                    if (batcher_result.swap_count >= 0 && batcher_result.swap_count < cfg::MAX_STEP_BUCKET) {
                        local_batcher_counter[batcher_result.swap_count]++;
                    }
                } else {
                    local_batcher_failed++;
                }
            }

            routing_path_rows[i] = make_routing_path_row(
                i,
                state,
                bfs_path,
                astar_path,
                strong_astar_path,
                beam_path,
                batcher_result
            );

            if constexpr (cfg::RUN_BFS && cfg::RUN_ASTAR) {
                if (astar_steps == bfs_steps) local_astar_match_bfs++; else local_astar_mismatch++;
            }
            if constexpr (cfg::RUN_BFS && cfg::RUN_STRONG_ASTAR) {
                if (strong_astar_steps == bfs_steps) local_strong_astar_match_bfs++; else local_strong_astar_mismatch++;
            }
            if constexpr (cfg::RUN_BFS && cfg::RUN_BEAM) {
                if (beam_steps >= 0) {
                    if (beam_steps == bfs_steps) local_beam_match_bfs++; else local_beam_not_optimal++;
                }
            }
            if constexpr (cfg::RUN_BFS && cfg::RUN_BATCHER) {
                if (batcher_result.success) {
                    if (batcher_result.swap_count == bfs_steps) local_batcher_match_bfs++; else local_batcher_not_optimal++;
                }
            }

#pragma omp atomic
            progress_counter++;

            if (progress_counter % 5000 == 0) {
#pragma omp critical
                cout << "Checked " << progress_counter << "/" << total_cases << " states...\n";
            }
        }

#pragma omp critical
        {
            for (int step = 0; step < cfg::MAX_STEP_BUCKET; ++step) {
                bfs_counter[step] += local_bfs_counter[step];
                astar_counter[step] += local_astar_counter[step];
                strong_astar_counter[step] += local_strong_astar_counter[step];
                beam_counter[step] += local_beam_counter[step];
                batcher_counter[step] += local_batcher_counter[step];
            }

            astar_match_bfs += local_astar_match_bfs;
            astar_mismatch += local_astar_mismatch;
            strong_astar_match_bfs += local_strong_astar_match_bfs;
            strong_astar_mismatch += local_strong_astar_mismatch;
            beam_success += local_beam_success;
            beam_match_bfs += local_beam_match_bfs;
            beam_not_optimal += local_beam_not_optimal;
            beam_failed += local_beam_failed;
            batcher_success += local_batcher_success;
            batcher_failed += local_batcher_failed;
            batcher_match_bfs += local_batcher_match_bfs;
            batcher_not_optimal += local_batcher_not_optimal;
            total_batcher_compares += local_total_batcher_compares;
            total_batcher_swaps += local_total_batcher_swaps;
            total_batcher_rounds += local_total_batcher_rounds;
            total_bfs_time += local_bfs_time;
            total_astar_time += local_astar_time;
            total_strong_astar_time += local_strong_astar_time;
            total_beam_time += local_beam_time;
            total_batcher_time += local_batcher_time;
        }
    }

    double total_elapsed = omp_get_wtime() - start_all;

    cout << "\n========================================\n";
    cout << "Full Test Result\n";
    cout << "========================================\n";
    cout << fixed << setprecision(6);
    cout << "Total elapsed wall time     : " << total_elapsed << " sec\n";
    cout << "Basic A* match BFS count    : " << astar_match_bfs << "\n";
    cout << "Basic A* mismatch count     : " << astar_mismatch << "\n";
    cout << "Strong A* match BFS count   : " << strong_astar_match_bfs << "\n";
    cout << "Strong A* mismatch count    : " << strong_astar_mismatch << "\n";
    cout << "Beam optimal vs BFS count   : " << beam_match_bfs << "\n";
    cout << "Beam not optimal count      : " << beam_not_optimal << "\n";
    cout << "Batcher optimal vs BFS count: " << batcher_match_bfs << "\n";

    print_ascii_distribution(bfs_counter);

    write_distribution_csv(
        bfs_counter,
        astar_counter,
        strong_astar_counter,
        beam_counter,
        batcher_counter,
        "output/step_distribution.csv"
    );
    write_routing_paths_csv(routing_path_rows, "output/q3_routing_paths.csv");
    write_text_report(
        "output/hypercube_report.txt",
        total_cases,
        static_cast<int>(edges.size()),
        omp_get_max_threads(),
        total_elapsed,
        total_bfs_time,
        total_astar_time,
        total_strong_astar_time,
        total_beam_time,
        total_batcher_time,
        astar_match_bfs,
        astar_mismatch,
        strong_astar_match_bfs,
        strong_astar_mismatch,
        beam_success,
        beam_failed,
        beam_match_bfs,
        beam_not_optimal,
        batcher_success,
        batcher_failed,
        batcher_match_bfs,
        batcher_not_optimal,
        total_batcher_compares,
        total_batcher_swaps,
        total_batcher_rounds,
        bfs_counter,
        astar_counter,
        strong_astar_counter,
        beam_counter,
        batcher_counter
    );

    cout << "\nCSV written to: output/step_distribution.csv\n";
    cout << "Routing paths written to: output/q3_routing_paths.csv\n";
    cout << "Text report written to: output/hypercube_report.txt\n";

    return 0;
}
