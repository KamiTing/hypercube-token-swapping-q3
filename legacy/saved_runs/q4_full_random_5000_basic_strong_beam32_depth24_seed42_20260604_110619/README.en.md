# Q3 Hypercube Token Swapping

This project implements and validates the following problem:

- **Minimum Token Swapping on Q3 Hypercube**
- **Zero-buffer edge-swap routing shortest path**

On a 3-dimensional hypercube `Q3` (8 nodes), each step can only swap the two tokens on a legal edge. The goal is to transform any initial permutation into `(0,1,2,3,4,5,6,7)` with the minimum number of swaps.

## Problem Model

- Nodes: `0..7`, represented by 3-bit binary labels.
- Edges: two nodes are connected iff their labels differ in exactly 1 bit.
- State representation: `state[node] = token`.
- Operation: each move swaps tokens on one hypercube edge.

This is a zero-buffer routing model: no extra buffers, no token stacking.

## Methods

- **BFS**: exact shortest-path true table
- **A\***: exact search with admissible heuristic
- **Beam Search**: heuristic search
- **Batcher baseline**: fixed compare-exchange sorting network

A\* heuristic:

`h(state) = ceil(total_hamming_distance(state) / 2)`

One swap can reduce the total Hamming distance by at most 2, so this heuristic does not overestimate.

## Beam Search Design Details

### Core Idea

A greedy search keeps only one path per step and can be trapped by local choices.  
Beam Search keeps multiple high-potential paths at each depth: after expansion, only the top `beam_width` states are retained.

Current project setting:

- `BEAM_WIDTH = 14`
- `MAX_DEPTH = 12`

### Candidate State Contents

Each `BeamItem` stores:

- `state`: current permutation
- `depth`: number of swaps used so far
- `last_edge_id`: previously used edge (to avoid immediate undo)
- `used_edges`: per-edge usage counts (for repeat-edge penalty)

### Layer Expansion

For each state in the current beam, all legal hypercube edge swaps are tried:

`next_state = swap_nodes(state, e.u, e.v)`

Q3 has 12 edges, so each state can generate up to 12 candidates per layer (before pruning).

### Deduplication and Pruning

`visited_best_depth[state]` stores the earliest depth at which a state was reached. If a state is revisited at an equal or greater depth, it is skipped.

Also, `last_edge_id` prevents immediate reversal on the same edge.

### Scoring Function (Tie-Break Order)

Candidates are ranked by `BeamScore` in tuple-like ascending order (smaller is better):

1. `total_dist`: total Hamming distance to target
2. `misplaced`: number of misplaced tokens
3. `max_dist`: max single-token distance to target
4. `repeat_penalty`: penalty for repeated edge usage
5. `-improvement`: global distance reduction by this swap
6. `-local_improvement`: local improvement on swapped pair
7. `-dir_score`: directional preference (hot correction bit)
8. `-touched_max_dist`: prioritize touching farther tokens
9. `depth`: prefer shallower path in full tie

### Termination Conditions

- Initial state is target: return `0`
- Target found during candidate generation: return current `depth`
- Depth reaches `MAX_DEPTH`: stop and return failure (`-1`)

### Positioning

Beam Search is heuristic and is not theoretically guaranteed to be optimal in general. However, on Q3 full-permutation testing (40320 states), this implementation and parameter set matches the BFS true table exactly.

## Experiment Scope

- Full permutation test: `8! = 40320` states.
- For every initial state, compute the distance to target and compare methods.

## Recommended Parameters

Based on full-state parameter sweeps for the current implementation:

- `BEAM_WIDTH = 14`
- `MAX_DEPTH = 12`

This configuration keeps Beam Search fully optimal on Q3 while being much faster than loose settings (e.g., 100/30).

## Project Structure

```text
.
├── include/
│   ├── config.h
│   ├── hypercube.h
│   ├── search.h
│   ├── batcher.h
│   └── report.h
├── src/
│   ├── main.cpp
│   ├── hypercube.cpp
│   ├── search.cpp
│   ├── batcher.cpp
│   └── report.cpp
├── output/
│   ├── hypercube_report.txt
│   ├── step_distribution.csv
│   ├── step_distribution_summary.csv
│   ├── bfs_step_distribution.png
│   └── method_step_distribution_compare.png
├── legacy/
│   └── hypercube_test.cpp
├── plot_distribution.py
└── CMakeLists.txt
```

## Build and Run

### 1) Compile the C++ program (MinGW g++)

```powershell
g++ -std=c++17 -O2 -fopenmp src/main.cpp src/hypercube.cpp src/search.cpp src/batcher.cpp src/report.cpp -Iinclude -o hypercube_refactor.exe
```

### 2) Run the full experiment

```powershell
.\hypercube_refactor.exe
```

Outputs will be written to `output/`:

- `output/hypercube_report.txt`
- `output/step_distribution.csv`

### 3) Generate summary table and plots

```powershell
.\.venv\Scripts\python.exe plot_distribution.py
```

This generates:

- `output/step_distribution_summary.csv`
- `output/bfs_step_distribution.png`
- `output/method_step_distribution_compare.png`

## Key Results (Q3 Full Test)

- A\* matches BFS on all states (40320/40320).
- Beam Search (14/12) matches BFS on all states (40320/40320).
- Batcher baseline is always solvable but rarely optimal (~`1.87%`).

## Q4 Full-Random Benchmark (Branch Focus)

This branch (`codex/q4-random-test`) adds a Q4 (`DIM=4`) full-random benchmark. Each case is sampled uniformly from all `16!` token placements with a Fisher-Yates shuffle, rather than generated by a random walk from the target state.

- Benchmark program: `src/q4_random_benchmark.cpp`
- Visualization script: `plot_q4_random.py`
- Compared methods: Basic A*, Strong A*, Beam Search, and Batcher's baseline

Basic A* heuristic:

`h_basic = ceil(total_hamming_distance / 2)`

Strong A* heuristic:

`h_strong = parity_adjust(max(ceil(total_hamming_distance / 2), max_packet_distance, cycle_lower_bound))`

Here `cycle_lower_bound = N - number_of_cycles`, and `parity_adjust` moves the lower bound to the nearest value with the same parity as the current permutation.

### Run Q4 benchmark

```powershell
g++ -std=c++17 -O2 src/q4_random_benchmark.cpp -o q4_random_benchmark_run.exe
.\q4_random_benchmark_run.exe 5000 32 24 42 0
```

Argument order:

- `samples beam_width max_depth seed astar_cap`
- `astar_cap = 0` means no visited-state cap for A*

### Generate Q4 visualizations

```powershell
.\.venv\Scripts\python.exe plot_q4_random.py
```

### Q4 output files

- `output/q4_random_benchmark.csv`
- `output/q4_random_summary.csv`
- `output/q4_steps_hist_compare.png`
- `output/q4_time_boxplot.png`
- `output/q4_gap_vs_batcher_hist.png`

### Current large run (5000 full-random samples)

- `beam_width=32, max_depth=24, seed=42, astar_cap=0`
- `elapsed = 4729s`
- `Basic A* failures = 0/5000`
- `Strong A* failures = 0/5000`
- `Beam failures = 1/5000`
- `Batcher failures = 0/5000`
- `Basic A* and Strong A* same steps = 5000/5000`
- `Basic A* avg steps = 17.1638`
- `Strong A* avg steps = 17.1638`
- `Beam avg steps = 17.3651`
- `Batcher avg swaps = 39.9234`
- `Basic A* avg expanded = 88782.4556`
- `Strong A* avg expanded = 4384.9472`
- `Basic A* avg sec = 0.706867`
- `Strong A* avg sec = 0.030566`
- `Beam avg sec = 0.001365`
- `Batcher avg sec = 0.00000097`

### Result Analysis

1. Strong A* preserves the same solution length as Basic A*.
Across 5000 full-random cases, both A* variants solved all cases and matched steps on `5000/5000` samples. Since both are admissible A* searches, this run shows that the stronger heuristic reduced cost without changing the optimal result.

2. The stronger heuristic greatly reduces expansion.
Average expanded nodes dropped from `88782.4556` to `4384.9472`, about a `20.2x` expansion reduction. Average runtime dropped from `0.706867s` to `0.030566s`, about a `23.1x` improvement.

3. Beam remains the fastest heuristic search.
Beam averaged `0.001365s`, but it is a pruning-based heuristic and does not guarantee optimality. One case out of `5000` was not solved within `max_depth=24`; among successful cases, its average step count was `17.3651`, slightly above the Basic A*/Strong A* average of `17.1638`.

4. Batcher is fastest but uses many more swaps.
Batcher is a fixed compare-exchange network, not a search algorithm. It averaged `39.9234` swaps, about `2.33x` the Strong A* average shortest-path length.

5. Method positioning.
Basic A* and Strong A* are A* variants with admissible heuristics; Beam is a fast heuristic baseline; Batcher is a deterministic routing baseline.

Note: the `astar_*` columns in CSV outputs correspond to Basic A*.

## Dependencies

- C++17 compiler (g++ recommended)
- OpenMP
- Python 3.11+
- pandas, matplotlib (for plotting script)

## Notes

- `output/` is the official output directory.
- `legacy/` keeps the original single-file implementation for reference.
