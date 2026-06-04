#include "report.h"

#include <fstream>
#include <iomanip>
#include <iostream>

#include "config.h"

using namespace std;

double safe_div(double a, double b) {
    return (b == 0.0) ? 0.0 : (a / b);
}

double percent(long long part, long long total) {
    if (total == 0) {
        return 0.0;
    }
    return 100.0 * static_cast<double>(part) / static_cast<double>(total);
}

double weighted_average_steps(const array<long long, cfg::MAX_STEP_BUCKET>& counter) {
    long long total_count = 0;
    long long weighted_sum = 0;
    for (int step = 0; step < cfg::MAX_STEP_BUCKET; ++step) {
        total_count += counter[step];
        weighted_sum += static_cast<long long>(step) * counter[step];
    }
    return safe_div(static_cast<double>(weighted_sum), static_cast<double>(total_count));
}

void write_distribution_csv(
    const array<long long, cfg::MAX_STEP_BUCKET>& bfs_counter,
    const array<long long, cfg::MAX_STEP_BUCKET>& astar_counter,
    const array<long long, cfg::MAX_STEP_BUCKET>& strong_astar_counter,
    const array<long long, cfg::MAX_STEP_BUCKET>& beam_counter,
    const array<long long, cfg::MAX_STEP_BUCKET>& batcher_counter,
    const string& filename
) {
    ofstream fout(filename);
    fout << "steps,bfs_count,astar_count,strong_astar_count,beam_count,batcher_count\n";

    for (int step = 0; step < cfg::MAX_STEP_BUCKET; ++step) {
        if (bfs_counter[step] == 0 && astar_counter[step] == 0 &&
            strong_astar_counter[step] == 0 && beam_counter[step] == 0 &&
            batcher_counter[step] == 0) {
            continue;
        }

        fout << step << ","
             << bfs_counter[step] << ","
             << astar_counter[step] << ","
             << strong_astar_counter[step] << ","
             << beam_counter[step] << ","
             << batcher_counter[step] << "\n";
    }
}

void write_text_report(
    const string& filename,
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
    const array<long long, cfg::MAX_STEP_BUCKET>& bfs_counter,
    const array<long long, cfg::MAX_STEP_BUCKET>& astar_counter,
    const array<long long, cfg::MAX_STEP_BUCKET>& strong_astar_counter,
    const array<long long, cfg::MAX_STEP_BUCKET>& beam_counter,
    const array<long long, cfg::MAX_STEP_BUCKET>& batcher_counter
) {
    ofstream fout(filename);
    fout << fixed << setprecision(6);

    fout << "========================================\n";
    fout << "Q3 Hypercube Full Permutation Test Report\n";
    fout << "========================================\n\n";

    fout << "[Experiment Settings]\n";
    fout << "Hypercube dimension              : " << cfg::DIM << "\n";
    fout << "Number of nodes                  : " << cfg::NODE_COUNT << "\n";
    fout << "Total states                     : " << total_cases << "\n";
    fout << "Hypercube edges                  : " << edge_count << "\n";
    fout << "OpenMP max threads               : " << max_threads << "\n";
    fout << "RUN_BFS                          : " << cfg::RUN_BFS << "\n";
    fout << "RUN_ASTAR                        : " << cfg::RUN_ASTAR << "\n";
    fout << "RUN_STRONG_ASTAR                 : " << cfg::RUN_STRONG_ASTAR << "\n";
    fout << "RUN_BEAM                         : " << cfg::RUN_BEAM << "\n";
    fout << "RUN_BATCHER                      : " << cfg::RUN_BATCHER << "\n";
    fout << "Beam width                       : " << cfg::BEAM_WIDTH << "\n";
    fout << "Max depth                        : " << cfg::MAX_DEPTH << "\n";
    fout << "Total elapsed wall time          : " << total_elapsed << " sec\n\n";

    if constexpr (cfg::RUN_BFS) {
        fout << "[BFS True Table]\n";
        fout << "Total BFS accumulated time       : " << total_bfs_time << " sec\n";
        fout << "Average BFS time per state       : " << total_bfs_time / total_cases << " sec\n";
        fout << "Average BFS shortest steps       : " << weighted_average_steps(bfs_counter) << "\n\n";
    }

    if constexpr (cfg::RUN_ASTAR) {
        fout << "[Basic A*]\n";
        fout << "Total Basic A* accumulated time  : " << total_astar_time << " sec\n";
        fout << "Average Basic A* time per state  : " << total_astar_time / total_cases << " sec\n";
        fout << "Average Basic A* steps           : " << weighted_average_steps(astar_counter) << "\n";
        if constexpr (cfg::RUN_BFS) {
            fout << "Basic A* match BFS count         : " << astar_match_bfs << "\n";
            fout << "Basic A* match BFS rate          : " << percent(astar_match_bfs, total_cases) << "%\n";
            fout << "Basic A* mismatch count          : " << astar_mismatch << "\n";
        }
        fout << "\n";
    }

    if constexpr (cfg::RUN_STRONG_ASTAR) {
        fout << "[Strong A*]\n";
        fout << "Total Strong A* accumulated time : " << total_strong_astar_time << " sec\n";
        fout << "Average Strong A* time per state : " << total_strong_astar_time / total_cases << " sec\n";
        fout << "Average Strong A* steps          : " << weighted_average_steps(strong_astar_counter) << "\n";
        if constexpr (cfg::RUN_BFS) {
            fout << "Strong A* match BFS count        : " << strong_astar_match_bfs << "\n";
            fout << "Strong A* match BFS rate         : " << percent(strong_astar_match_bfs, total_cases) << "%\n";
            fout << "Strong A* mismatch count         : " << strong_astar_mismatch << "\n";
        }
        fout << "\n";
    }

    if constexpr (cfg::RUN_BEAM) {
        fout << "[Beam Search]\n";
        fout << "Total Beam accumulated time      : " << total_beam_time << " sec\n";
        fout << "Average Beam time per state      : " << total_beam_time / total_cases << " sec\n";
        fout << "Beam success count               : " << beam_success << "\n";
        fout << "Beam success rate                : " << percent(beam_success, total_cases) << "%\n";
        fout << "Beam failed count                : " << beam_failed << "\n";
        fout << "Average Beam steps               : " << weighted_average_steps(beam_counter) << "\n";
        if constexpr (cfg::RUN_BFS) {
            fout << "Beam optimal vs BFS count        : " << beam_match_bfs << "\n";
            fout << "Beam optimal vs BFS rate         : " << percent(beam_match_bfs, total_cases) << "%\n";
            fout << "Beam not optimal count           : " << beam_not_optimal << "\n";
        }
        fout << "\n";
    }

    if constexpr (cfg::RUN_BATCHER) {
        fout << "[Batcher's Merge Sort Baseline]\n";
        fout << "Total Batcher accumulated time   : " << total_batcher_time << " sec\n";
        fout << "Average Batcher time per state   : " << total_batcher_time / total_cases << " sec\n";
        fout << "Batcher success count            : " << batcher_success << "\n";
        fout << "Batcher success rate             : " << percent(batcher_success, total_cases) << "%\n";
        fout << "Batcher failed count             : " << batcher_failed << "\n";
        fout << "Average Batcher compares         : " << safe_div(total_batcher_compares, total_cases) << "\n";
        fout << "Average Batcher swaps            : " << safe_div(total_batcher_swaps, total_cases) << "\n";
        fout << "Average Batcher rounds           : " << safe_div(total_batcher_rounds, total_cases) << "\n";
        if constexpr (cfg::RUN_BFS) {
            fout << "Batcher optimal vs BFS count     : " << batcher_match_bfs << "\n";
            fout << "Batcher optimal vs BFS rate      : " << percent(batcher_match_bfs, total_cases) << "%\n";
            fout << "Batcher not optimal count        : " << batcher_not_optimal << "\n";
        }
        fout << "\n";
    }

    fout << "========================================\n";
    fout << "Step Distribution, BFS exact baseline\n";
    fout << "========================================\n";

    for (int step = 0; step < cfg::MAX_STEP_BUCKET; ++step) {
        if (bfs_counter[step] > 0) {
            fout << "Steps = " << setw(2) << step
                 << " | Count = " << setw(6) << bfs_counter[step] << "\n";
        }
    }

    fout << "\n[Step Distribution Table]\n";
    fout << "steps,bfs_count,astar_count,strong_astar_count,beam_count,batcher_count\n";
    for (int step = 0; step < cfg::MAX_STEP_BUCKET; ++step) {
        if (bfs_counter[step] == 0 && astar_counter[step] == 0 &&
            strong_astar_counter[step] == 0 && beam_counter[step] == 0 &&
            batcher_counter[step] == 0) {
            continue;
        }
        fout << step << ","
             << bfs_counter[step] << ","
             << astar_counter[step] << ","
             << strong_astar_counter[step] << ","
             << beam_counter[step] << ","
             << batcher_counter[step] << "\n";
    }
}

void print_ascii_distribution(const array<long long, cfg::MAX_STEP_BUCKET>& counter) {
    long long max_count = 1;
    for (int step = 0; step < cfg::MAX_STEP_BUCKET; ++step) {
        if (counter[step] > max_count) {
            max_count = counter[step];
        }
    }

    cout << "\n========================================\n";
    cout << "Step Distribution, BFS exact baseline\n";
    cout << "========================================\n";

    for (int step = 0; step < cfg::MAX_STEP_BUCKET; ++step) {
        if (counter[step] == 0) {
            continue;
        }

        int bar_len = static_cast<int>((50.0 * counter[step]) / max_count);
        cout << "Steps = " << setw(2) << step << " | Count = " << setw(6) << counter[step] << " | ";
        for (int i = 0; i < bar_len; ++i) {
            cout << "#";
        }
        cout << "\n";
    }
}
