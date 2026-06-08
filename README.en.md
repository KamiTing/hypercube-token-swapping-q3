# Q3-Q9 Hypercube Token Swapping

This project implements and validates zero-buffer token swapping on hypercubes, including:

- **Minimum Token Swapping on Q3 Hypercube**
- **Zero-buffer edge-swap routing shortest path**
- **Q4 full-random benchmark**
- **Q4-Q9 special-case Beam Search**

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
│   ├── report.h
│   └── qk/
│       ├── common.h
│       ├── hypercube.h
│       ├── search.h
│       ├── batcher.h
│       ├── cases.h
│       └── io.h
├── src/
│   ├── main.cpp
│   ├── hypercube.cpp
│   ├── search.cpp
│   ├── batcher.cpp
│   ├── report.cpp
│   ├── qk_common.cpp
│   ├── qk_hypercube.cpp
│   ├── qk_search.cpp
│   ├── qk_batcher.cpp
│   ├── qk_cases.cpp
│   ├── qk_io.cpp
│   └── qk_special_cases.cpp
├── output/
│   ├── q4_path_selected_10000_basic_no_path_20260604_205233/
│   ├── qk_custom_cases_20260605_133247/
│   ├── q8_case1_trim_20260605_154219/
│   ├── q8_case2_trim_20260605_154927/
│   ├── q9_case1_disk_bw256_20260607_113040/
│   └── qk_special_cases_routes.xlsx
├── tools/
│   └── build_qk_special_cases_workbook.mjs
├── custom_qk_cases.csv
├── Q9_BEAM_SEARCH_EVOLUTION.md
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

## Q4 Full-Random Benchmark

Each Q4 (`DIM=4`) full-random case is sampled uniformly from all `16!` token placements with a Fisher-Yates shuffle, rather than generated by a random walk from the target state.

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
.\q4_random_benchmark_run.exe 10000 128 30 42 0 1 1 output\q4_path_selected_10000_basic_no_path_YYYYMMDD_HHMMSS
```

Argument order:

- `samples beam_width max_depth seed astar_cap parallel_methods record_paths [output_dir]`
- `astar_cap = 0` means no visited-state cap for A*
- `parallel_methods = 1` runs the four methods for each case concurrently
- `record_paths = 1` writes detailed routing paths; use `0` for large runs if memory usage matters
- To avoid excessive memory use on hard Q4 cases, Basic A* does not write a path; `astar_path` is left empty while `astar_steps`, expanded nodes, and runtime are still recorded. Strong A*, Beam, and Batcher write full paths.
- `output_dir` is optional and defaults to `output`. For path-enabled runs, use a separate directory to avoid overwriting an existing large benchmark.

### Generate Q4 visualizations

```powershell
.\.venv\Scripts\python.exe plot_q4_random.py output\q4_path_selected_10000_basic_no_path_YYYYMMDD_HHMMSS
```

### Q4 output files

- `output_dir/q4_random_benchmark.csv`
- `output_dir/q4_random_routing_paths.csv`
- `output_dir/q4_random_summary.csv`
- `output_dir/q4_steps_hist_compare.png`
- `output_dir/q4_time_boxplot.png`
- `output_dir/q4_gap_vs_batcher_hist.png`

`q4_random_routing_paths.csv` records detailed routing paths for every selected permutation. Each row includes the initial state and Basic A*/Strong A*/Beam/Batcher step counts. Basic A* leaves `astar_path` empty; Strong A*, Beam, and Batcher write the actual edge-swap sequence. Because Q4 nodes include `10..15`, the path format uses `u-v`, for example `0-1 10-14`.

### Current large run (10000 full-random samples)

- Output directory: `output/q4_path_selected_10000_basic_no_path_20260604_205233`
- `beam_width=128, max_depth=30, seed=42, astar_cap=0, parallel_methods=1, record_paths=1`
- `elapsed = 11295s`
- `Basic A* failures = 0/10000`
- `Strong A* failures = 0/10000`
- `Beam failures = 0/10000`
- `Batcher failures = 0/10000`
- `Basic A* and Strong A* same steps = 10000/10000`
- `Strong A* expanded <= Basic A* = 8747/10000`
- `Basic A* avg steps = 17.1852`
- `Strong A* avg steps = 17.1852`
- `Beam avg steps = 17.2270`
- `Batcher avg swaps = 39.9200`
- `Basic A* avg expanded = 87167.9328`
- `Strong A* avg expanded = 4287.0348`
- `Beam avg expanded = 48680.0116`
- `Basic A* avg sec = 0.841673`
- `Strong A* avg sec = 0.073471`
- `Beam avg sec = 0.018573`
- `Batcher avg sec = 0.000022`
- `avg case wall sec = 1.129430`
- Routing path verification: `astar_path` is empty for every row; Strong A*, Beam, and Batcher paths all reach the Q4 goal, and path lengths match their step/swap columns.

### Result Analysis

1. Strong A* preserves the same solution length as Basic A*.
Across 10000 full-random cases, both A* variants solved all cases and matched steps on `10000/10000` samples. Since both are admissible A* searches, this run shows that the stronger heuristic reduced cost without changing the optimal result.

2. The stronger heuristic greatly reduces expansion.
Average expanded nodes dropped from `87167.9328` to `4287.0348`, about a `20.3x` expansion reduction. Average runtime dropped from `0.841673s` to `0.073471s`, about an `11.5x` improvement.

3. Beam remains the fastest heuristic search.
Beam averaged `0.018573s`, but it is a pruning-based heuristic and does not guarantee optimality. This run solved `10000/10000` cases; its average step count was `17.2270`, slightly above the Basic A*/Strong A* average of `17.1852`.

4. Batcher is fastest but uses many more swaps.
Batcher is a fixed compare-exchange network, not a search algorithm. It averaged `39.9200` swaps, about `2.32x` the Strong A* average shortest-path length.

5. Method positioning.
Basic A* and Strong A* are A* variants with admissible heuristics; Beam is a fast heuristic baseline; Batcher is a deterministic routing baseline.

Note: the `astar_*` columns in CSV outputs correspond to Basic A*.

## Q4-Q9 Special Cases (`codex/sp`)

The SP branch adds a modular `qk` special-case runner for specified Q4-Q9 permutations. Q4 can use Strong A* for exact verification. Q5 and above run only Beam Search and the Batcher baseline to avoid Basic A* state-space explosion.

After the refactor, the special-case runner is split into:

- `qk_common`: shared state, swap, fingerprint, and packed-key types.
- `qk_hypercube`: hypercube edges, distances, lower bounds, and path validation.
- `qk_search`: Strong A*, Beam Search, RAM fingerprint visited storage, and SQLite disk fingerprint visited storage.
- `qk_batcher`: Batcher baseline.
- `qk_cases`: built-in cases, custom CSV parsing, and permutation validation.
- `qk_io`: CSV escaping, progress display, and memory-trimming helpers.
- `qk_special_cases.cpp`: CLI arguments, output files, and the top-level runner flow.

`custom_qk_cases.csv` can hold Q10 case data for future experiments. The current runner still executes only Q1-Q9; Q10 needs later search-strategy and execution-limit changes.

### Beam visited modes

Three visited implementations remain selectable from the command line:

| Mode | Description |
|---|---|
| `exact` | Stores the full packed state and supports Q1-Q8 |
| `fingerprint128` | Stores 128-bit fingerprints in RAM |
| `fingerprint128_disk` | Stores 128-bit fingerprints in batched SQLite databases |

Q9 uses `fingerprint128_disk`. The Bloom filter is only a fast negative filter. Possible hits are still checked against the in-memory pending set and the SQLite primary key, so Bloom false positives do not directly discard candidates.

### Simplified Beam ordering

`qk_special_cases` currently keeps the simplified Beam ordering used by the successful Q8/Q9 runs. At each depth, candidates are retained by the following ascending keys:

```text
total_dist
max_dist
misplaced
depth
edge_id
packed_key / fingerprint / order
```

This version no longer uses the older nine-field Beam tie-breaker, such as `repeat_penalty`, `improvement`, `local_improvement`, `dir_score`, and `touched_max_dist`. The goal is to reduce per-candidate state and path-history overhead so Q8/Q9 can run reliably with fingerprint visited storage, disk visited storage, and parallel candidate generation. The ordering is still a heuristic Beam policy and does not guarantee shortest paths; final correctness is checked by replaying the output path.

### Q9 memory and parallel improvements

- Paths use parent back-pointers and are reconstructed only after a solution is found.
- Fingerprints are updated in O(1) after a swap.
- Total distance, misplaced count, and maximum distance are updated incrementally.
- A full 512-token state is materialized only for candidates retained by the Beam.
- Visited fingerprints are distributed across 16 SQLite shards.
- 24 workers generate candidates and process sharded lookups in parallel.
- Fixed parent/edge indices and an original-order commit pass preserve deterministic search ordering.
- The Windows process runs at `BelowNormal` priority to preserve desktop responsiveness.
- The final Q9 run disables candidate tracing and records only depth progress, disk progress, and the final path.

See [Q9_BEAM_SEARCH_EVOLUTION.md](Q9_BEAM_SEARCH_EVOLUTION.md) for the complete design and validation record.

### Build the special-case runner

Direct MinGW g++ build:

```powershell
g++ -std=c++17 -O2 -Wall -Wextra -pedantic -Iinclude `
  src\qk_common.cpp src\qk_hypercube.cpp src\qk_io.cpp `
  src\qk_search.cpp src\qk_batcher.cpp src\qk_cases.cpp `
  src\qk_special_cases.cpp -lsqlite3 -o qk_special_cases.exe
```

The `qk_special_cases` CMake target can also be used. CMake requires `Threads` and `SQLite3`.

### Command-line arguments

```text
qk_special_cases.exe
  [min_dim=4] [max_dim=6] [beam_width=256] [max_depth=0]
  [exact_max_dim=4] [astar_cap=2000000] [exact_time_sec=30]
  [output_dir] [custom_cases_csv] [candidate_trace_mode=2]
  [case_name_filter] [beam_visited_mode=exact] [worker_threads=24]
  [disk_bloom_mb=1024] [disk_batch_size=1000000] [disk_shards=16]
```

`candidate_trace_mode`:

- `0`: no candidate trace
- `1`: retained Beam candidates only
- `2`: all new candidates

### Final Q9 case1 command

```powershell
.\qk_special_cases.exe 9 9 256 4608 0 2000000 30 `
  output\q9_case1_disk_bw256_20260607_113040 `
  .\custom_qk_cases.csv 0 q9_case1 `
  fingerprint128_disk 24 1024 1000000 16
```

### Special-case results

Every Beam and Batcher path below passed hypercube-edge replay validation:

| Dimension | Case | Strong LB | Beam steps | Expanded | Beam sec | Batcher swaps |
|---|---|---:|---:|---:|---:|---:|
| Q5 | case1 | 40 | 42 | 738,848 | 0.592 | 124 |
| Q5 | case2 | 40 | 44 | 765,473 | 0.698 | 108 |
| Q6 | case1 | 96 | 106 | 4,790,899 | 6.669 | 344 |
| Q6 | case2 | 84 | 92 | 4,200,618 | 5.561 | 318 |
| Q7 | case1 | 224 | 254 | 27,793,523 | 50.056 | 906 |
| Q7 | case2 | 196 | 240 | 26,463,332 | 49.953 | 808 |
| Q8 | case1 | 512 | 686 | 88,551,994 | 207.723 | 2318 |
| Q8 | case2 | 438 | 562 | 72,584,496 | 153.829 | 2102 |
| Q9 | case1 | 1152 | 1558 | 909,749,194 | 14,307.567 | 5774 |

Q9 case1:

- Beam width: `256`
- Solution depth: `1558`
- Visited states: `909,749,195`
- Runtime: about `3 hours 58 minutes 28 seconds`
- Peak RAM: about `1.56 GB`
- SQLite visited size at completion: about `19.75 GB`
- Beam uses about `73.0%` fewer swaps than Batcher
- `beam_path_valid=1`

The SQLite visited shards were deleted after completion to reclaim disk space. The final path, per-depth progress, and result CSV remain available.

### Artifacts

- Q4-Q7: `output/qk_custom_cases_20260605_133247/`
- Q8 case1: `output/q8_case1_trim_20260605_154219/`
- Q8 case2: `output/q8_case2_trim_20260605_154927/`
- Q9 case1: `output/q9_case1_disk_bw256_20260607_113040/`
- Complete route workbook: `output/qk_special_cases_routes.xlsx`
- Custom cases: `custom_qk_cases.csv`

## Dependencies

- C++17 compiler (g++ recommended)
- OpenMP
- Threads
- SQLite3
- Python 3.11+
- pandas, matplotlib (for plotting script)

## Notes

- `output/` keeps the official Q4 benchmark, Q4-Q9 special-case results, and the complete route workbook.
- `legacy/` stores older and outdated data, including Q3 outputs, previous Q4 runs, failed or aborted path runs, smoke tests, verification experiments, old reports, and saved runs.
