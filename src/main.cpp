#include <array>
#include <iomanip>
#include <iostream>
#include <filesystem>

#include <omp.h>

#include "batcher.h"
#include "config.h"
#include "hypercube.h"
#include "report.h"
#include "search.h"

using namespace std;

int main() {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);
    std::filesystem::create_directories("output");

    const vector<Edge> edges = generate_hypercube_edges();
    const vector<uint64_t> states = generate_all_states();
    const int total_cases = static_cast<int>(states.size());

    cout << "========================================\n";
    cout << "Q3 Hypercube Full Permutation Test\n";
    cout << "========================================\n";
    cout << "Total states : " << total_cases << "\n";
    cout << "Edges        : " << edges.size() << "\n";
    cout << "OpenMP max threads : " << omp_get_max_threads() << "\n";
    cout << "RUN_BFS      : " << cfg::RUN_BFS << "\n";
    cout << "RUN_ASTAR    : " << cfg::RUN_ASTAR << "\n";
    cout << "RUN_BEAM     : " << cfg::RUN_BEAM << "\n";
    cout << "RUN_BATCHER  : " << cfg::RUN_BATCHER << "\n";
    cout << "Beam width   : " << cfg::BEAM_WIDTH << "\n";
    cout << "Max depth    : " << cfg::MAX_DEPTH << "\n\n";

    array<long long, cfg::MAX_STEP_BUCKET> bfs_counter{};
    array<long long, cfg::MAX_STEP_BUCKET> astar_counter{};
    array<long long, cfg::MAX_STEP_BUCKET> beam_counter{};
    array<long long, cfg::MAX_STEP_BUCKET> batcher_counter{};

    long long astar_match_bfs = 0, astar_mismatch = 0;
    long long beam_success = 0, beam_match_bfs = 0, beam_not_optimal = 0, beam_failed = 0;
    long long batcher_success = 0, batcher_failed = 0, batcher_match_bfs = 0, batcher_not_optimal = 0;
    long long total_batcher_compares = 0, total_batcher_swaps = 0, total_batcher_rounds = 0;

    double total_bfs_time = 0.0, total_astar_time = 0.0, total_beam_time = 0.0, total_batcher_time = 0.0;
    double start_all = omp_get_wtime();
    int progress_counter = 0;

#pragma omp parallel
    {
        array<long long, cfg::MAX_STEP_BUCKET> local_bfs_counter{};
        array<long long, cfg::MAX_STEP_BUCKET> local_astar_counter{};
        array<long long, cfg::MAX_STEP_BUCKET> local_beam_counter{};
        array<long long, cfg::MAX_STEP_BUCKET> local_batcher_counter{};

        long long local_astar_match_bfs = 0, local_astar_mismatch = 0;
        long long local_beam_success = 0, local_beam_match_bfs = 0, local_beam_not_optimal = 0, local_beam_failed = 0;
        long long local_batcher_success = 0, local_batcher_failed = 0, local_batcher_match_bfs = 0, local_batcher_not_optimal = 0;
        long long local_total_batcher_compares = 0, local_total_batcher_swaps = 0, local_total_batcher_rounds = 0;

        double local_bfs_time = 0.0, local_astar_time = 0.0, local_beam_time = 0.0, local_batcher_time = 0.0;

#pragma omp for schedule(dynamic, 64)
        for (int i = 0; i < total_cases; ++i) {
            uint64_t state = states[i];
            int bfs_steps = -1, astar_steps = -1, beam_steps = -1;
            BatcherResult batcher_result{0, 0, 0, false};

            if constexpr (cfg::RUN_BFS) {
                double t0 = omp_get_wtime();
                bfs_steps = bfs_min_steps(state, edges);
                local_bfs_time += (omp_get_wtime() - t0);
                if (bfs_steps >= 0 && bfs_steps < cfg::MAX_STEP_BUCKET) local_bfs_counter[bfs_steps]++;
            }

            if constexpr (cfg::RUN_ASTAR) {
                double t0 = omp_get_wtime();
                astar_steps = astar_min_steps(state, edges);
                local_astar_time += (omp_get_wtime() - t0);
                if (astar_steps >= 0 && astar_steps < cfg::MAX_STEP_BUCKET) local_astar_counter[astar_steps]++;
            }

            if constexpr (cfg::RUN_BEAM) {
                double t0 = omp_get_wtime();
                beam_steps = beam_search_steps(state, edges, cfg::BEAM_WIDTH, cfg::MAX_DEPTH);
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
                batcher_result = batcher_merge_sort_baseline(state);
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

            if constexpr (cfg::RUN_BFS && cfg::RUN_ASTAR) {
                if (astar_steps == bfs_steps) local_astar_match_bfs++; else local_astar_mismatch++;
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
                beam_counter[step] += local_beam_counter[step];
                batcher_counter[step] += local_batcher_counter[step];
            }

            astar_match_bfs += local_astar_match_bfs;
            astar_mismatch += local_astar_mismatch;
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
    cout << "A* match BFS count          : " << astar_match_bfs << "\n";
    cout << "A* mismatch count           : " << astar_mismatch << "\n";
    cout << "Beam optimal vs BFS count   : " << beam_match_bfs << "\n";
    cout << "Beam not optimal count      : " << beam_not_optimal << "\n";
    cout << "Batcher optimal vs BFS count: " << batcher_match_bfs << "\n";

    print_ascii_distribution(bfs_counter);

    write_distribution_csv(
        bfs_counter,
        astar_counter,
        beam_counter,
        batcher_counter,
        "output/step_distribution.csv"
    );
    write_text_report(
        "output/hypercube_report.txt",
        total_cases,
        static_cast<int>(edges.size()),
        omp_get_max_threads(),
        total_elapsed,
        total_bfs_time,
        total_astar_time,
        total_beam_time,
        total_batcher_time,
        astar_match_bfs,
        astar_mismatch,
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
        beam_counter,
        batcher_counter
    );

    cout << "\nCSV written to: output/step_distribution.csv\n";
    cout << "Text report written to: output/hypercube_report.txt\n";

    return 0;
}
