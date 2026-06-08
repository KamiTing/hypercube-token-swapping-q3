#include "qk/batcher.h"

#include "qk/common.h"

#include <algorithm>
#include <chrono>

using namespace std;

namespace qk {

BatcherResult batcher_baseline(const State& init, bool record_path) {
    auto t0 = Clock::now();
    State s = init;
    int n = static_cast<int>(s.size());
    if (s == target_state(n)) {
        auto t1 = Clock::now();
        return {
            true,
            0,
            0,
            0,
            chrono::duration<double>(t1 - t0).count(),
            {}
        };
    }

    int compares = 0;
    int swaps = 0;
    int rounds = 0;
    vector<SwapStep> path;

    for (int k = 2; k <= n; k <<= 1) {
        for (int j = k >> 1; j > 0; j >>= 1) {
            rounds++;
            for (int i = 0; i < n; ++i) {
                int p = i ^ j;
                if (p <= i) {
                    continue;
                }
                bool asc = ((i & k) == 0);
                bool should_swap = asc ? (s[i] > s[p]) : (s[i] < s[p]);
                compares++;
                if (should_swap) {
                    swap(s[i], s[p]);
                    swaps++;
                    if (record_path) {
                        path.push_back({i, p});
                    }
                }
            }
        }
    }

    auto t1 = Clock::now();
    return {
        s == target_state(n),
        swaps,
        compares,
        rounds,
        chrono::duration<double>(t1 - t0).count(),
        path
    };
}

} // namespace qk
