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

## Q4 Random Benchmark (Branch Focus)

This branch (`codex/q4-random-test`) adds a Q4 (`DIM=4`) random benchmark:

- Benchmark program: `src/q4_random_benchmark.cpp`
- Visualization script: `plot_q4_random.py`

### Run Q4 benchmark

```powershell
g++ -std=c++17 -O2 src/q4_random_benchmark.cpp -o q4_random_benchmark_run.exe
.\q4_random_benchmark_run.exe 5000 20 32 24 42
```

Argument order:

- `samples scramble_steps beam_width max_depth seed`

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

### Latest large run (5000 samples)

- `scramble_steps=20, beam_width=32, max_depth=24, seed=42`
- `A* failures = 242/5000` (success rate `95.16%`)
- `Beam failures = 0/5000` (success rate `100%`)
- `Batcher failures = 0/5000`
- `A* <= Batcher swaps = 4758/5000`
- `Beam <= Batcher swaps = 5000/5000`
- `avg_astar_steps = 12.7209`
- `avg_beam_steps = 12.9256`
- `avg_batcher_swaps = 38.6944`

### Result Analysis

1. Robustness: Beam is the most stable.  
`Beam failures = 0/5000`, while A* fails on `242/5000` samples under this Q4 setting.

2. Solution quality: A* is slightly better when it succeeds.  
`avg_astar_steps (12.72)` is smaller than `avg_beam_steps (12.93)`, so successful A* runs tend to produce shorter swap sequences.

3. Against Batcher baseline: both search methods are much better in swap count.  
`avg_batcher_swaps = 38.69`, whereas A*/Beam are around 13 steps on average, showing a clear improvement over fixed-network routing.

4. Practical takeaway: Beam is currently the best default for large random Q4 tests.  
It combines high success rate and low per-sample runtime; A* is useful as a higher-quality comparator when it converges.

5. Method positioning: Batcher is a deterministic baseline, not an optimal solver.  
It is very fast and stable, but it does not target minimum swap count.

## Dependencies

- C++17 compiler (g++ recommended)
- OpenMP
- Python 3.11+
- pandas, matplotlib (for plotting script)

## Notes

- `output/` is the official output directory.
- `legacy/` keeps the original single-file implementation for reference.
