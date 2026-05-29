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
¢u¢w¢w include/
¢x   ¢u¢w¢w config.h
¢x   ¢u¢w¢w hypercube.h
¢x   ¢u¢w¢w search.h
¢x   ¢u¢w¢w batcher.h
¢x   ¢|¢w¢w report.h
¢u¢w¢w src/
¢x   ¢u¢w¢w main.cpp
¢x   ¢u¢w¢w hypercube.cpp
¢x   ¢u¢w¢w search.cpp
¢x   ¢u¢w¢w batcher.cpp
¢x   ¢|¢w¢w report.cpp
¢u¢w¢w output/
¢x   ¢u¢w¢w hypercube_report.txt
¢x   ¢u¢w¢w step_distribution.csv
¢x   ¢u¢w¢w step_distribution_summary.csv
¢x   ¢u¢w¢w bfs_step_distribution.png
¢x   ¢|¢w¢w method_step_distribution_compare.png
¢u¢w¢w legacy/
¢x   ¢|¢w¢w hypercube_test.cpp
¢u¢w¢w plot_distribution.py
¢|¢w¢w CMakeLists.txt
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

## Dependencies

- C++17 compiler (g++ recommended)
- OpenMP
- Python 3.11+
- pandas, matplotlib (for plotting script)

## Notes

- `output/` is the official output directory.
- `legacy/` keeps the original single-file implementation for reference.
