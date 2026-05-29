#include "batcher.h"

#include "config.h"
#include "hypercube.h"

BatcherResult batcher_merge_sort_baseline(uint64_t initial_state) {
    uint64_t state = initial_state;
    int compare_count = 0;
    int swap_count = 0;
    int round_count = 0;

    const int N = cfg::NODE_COUNT;

    for (int k = 2; k <= N; k <<= 1) {
        for (int j = k >> 1; j > 0; j >>= 1) {
            round_count++;

            for (int i = 0; i < N; ++i) {
                int partner = i ^ j;
                if (partner <= i) {
                    continue;
                }

                if (!is_hypercube_edge(i, partner)) {
                    continue;
                }

                bool ascending = ((i & k) == 0);
                int pi = get_packet(state, i);
                int pj = get_packet(state, partner);
                compare_count++;

                bool need_swap = ascending ? (pi > pj) : (pi < pj);
                if (need_swap) {
                    state = swap_nodes(state, i, partner);
                    swap_count++;
                }
            }
        }
    }

    return {compare_count, swap_count, round_count, is_solved(state)};
}
