# Qk Special-Case Hypercube Token Swapping

This branch is the working branch for Qk hypercube special-case token swapping. It focuses on Q4+ fixed permutations, Beam Search, CUDA candidate generation, route validation, and preserved result artifacts. It is no longer the main branch for Q4 random benchmarking.

Current branch roles:

- `codex/sp`: special-case / CUDA Beam branch described here.
- `codex/q4-random-test`: dedicated Q4 full-random benchmark branch.

## Current Status

As of `2026-06-13`:

| Scope | Status | Main source |
|---|---|---|
| Q4 custom cases | 24/24 solved | `output/q4_q11_cuda_all_bw512_pool65536_win65536_restart2048_plateau2048_perturb010_cub_20260613_113055/` |
| Q5-Q11 special cases | 2/2 solved for each dimension | same source |
| Q12 | `q12_case1` solved; `q12_case2` is not an official completed result yet | `output/q12_cuda_gpu_retained_bw1024_pool262144_win65536_restart4096_plateau1536_perturb015_cub_path_20260613_132118/` |
| Q13-Q14 | cases are present but unsolved in official artifacts | `custom_qk_cases.csv` |
| Q15 | one case is present but unsolved in official artifacts | `custom_qk_cases.csv` |
| Route workbook | Q4-Q12 sheets exist; Q12 currently contains `q12_case1` only | `output/qk_special_cases_routes.xlsx` |

`custom_qk_cases.csv` currently contains 24 Q4 cases, 2 cases for each Q5-Q14 dimension, and 1 Q15 case. Preserved artifacts and cleanup rules are documented in this README.

## Problem Model

For `Qd` hypercube token swapping:

- Node count: `N = 2^d`
- Nodes: `0..N-1`
- Legal edge: two node labels differ in exactly one bit
- State: `state[node] = token`
- Goal: identity permutation, `state[i] = i`
- Move: swap two tokens along one hypercube edge

This is a zero-buffer model: no extra holding nodes and no token stacking.

## Module Layout

| Module | Responsibility |
|---|---|
| `qk_common` | state types, swap steps, fingerprints, packed keys |
| `qk_hypercube` | edges, distances, lower bounds, path replay validation |
| `qk_search` | Strong A*, Beam Search, disk visited, layer-only visited, path back-pointers |
| `qk_cuda_candidate_generator` | CUDA candidate generation, CUB Top-K, GPU diverse preselection |
| `qk_batcher` | deterministic Batcher baseline |
| `qk_cases` | built-in cases and `custom_qk_cases.csv` parsing |
| `qk_io` | CSV escaping, progress output, path formatting |
| `qk_path_optimizer` | retained path post-processing tool; disabled in official results |
| `qk_special_cases.cpp` | CLI runner and CSV output orchestration |

Older Q3 and Q4 random benchmark code remains in the repository for reference, but it is not the center of this branch.

## Lower Bound

The reported `strong_lb` is:

```text
basic_lb = ceil(total_hamming_distance / 2)
max_packet_distance = max token-to-target Hamming distance
cycle_lower_bound = N - number_of_cycles
strong_lb = parity_adjust(max(basic_lb, max_packet_distance, cycle_lower_bound))
```

`parity_adjust` makes the lower bound match the permutation parity. This is a lower bound, not an expected Beam path length; Q12 still sits far above the bound, so the search strategy still has room to improve.

## Search Modes

### Strong A*

`astar_exact()` uses `strong_lb` as an admissible heuristic and is useful for small dimensions. Official special-case runs mostly skip exact search above Q4.

### Batcher Baseline

`batcher_baseline()` is a deterministic compare-exchange route. It is stable and valid, but usually much longer than Beam.

### Beam Search

Beam is the current main solver. Each layer expands retained states over all hypercube edges, scores candidates, and keeps a bounded set for the next layer.

The main candidate ordering is:

```text
total_dist
max_dist
misplaced
depth
edge_id
fingerprint / packed key / generation order
```

Beam is heuristic and does not guarantee optimal paths. Official routes are accepted only after replay validation.

## Visited Modes

| Mode | Purpose |
|---|---|
| `exact` | stores full packed states; suitable up to Q8 |
| `fingerprint128` | stores 128-bit fingerprints in RAM |
| `fingerprint128_disk` | stores fingerprints in SQLite shards; used by older Q9 official runs |
| `layer_only` | keeps only the current beam and the previous K retained fingerprint layers |

Current high-dimensional CUDA runs use `layer_only`. It avoids SQLite visited databases, keeps `qk_beam_candidate_trace.csv` at 0 bytes for official large runs, and reconstructs the full path from parent back-pointers after a solution is found.

### Perturbation Setting

`layer_perturbation_ratio` only affects retained selection in `layer_only` Beam. With `0.10`, about 10% of retained slots are selected from the candidate pool by a deterministic hash perturbation instead of pure greedy score; with `0.15`, that share rises to about 15%. This is not random search: the same input and parameters remain reproducible. The Q4-Q11 official all-run uses `0.10`, while Q12 case1 uses `0.15`, mainly to keep more non-local-best directions alive near high-dimensional tail plateaus.

## CUDA Backend

The CUDA backend accelerates the `layer_only` Beam candidate stage. It is not a separate solver.

Current split:

- GPU: candidate generation, score computation, recent fingerprint checks, CUB DeviceTopK preselection.
- GPU: diverse preselection for greedy, max-distance, misplaced, edge-bit, and perturbation keys.
- CPU: final exact comparator ordering, retained deduplication, parent path-node writes, and retained state materialization.

`cuda_topk_mode` values:

| Mode | Meaning |
|---|---|
| `cub` | recommended; CUB DeviceTopK with refined tie handling |
| `tiled` | tile-level GPU selection followed by CPU merge |
| `full_sort` | old full GPU sort, kept for diagnostics or fallback |

`topk_mode=2` in `qk_cuda_timing.csv` is the expected CUB path. `topk_mode=0` means full sort and is undesirable for large official runs.

## Build

### CPU / disk mode

```powershell
g++ -std=c++17 -O2 -Wall -Wextra -pedantic -Iinclude `
  src\qk_common.cpp src\qk_hypercube.cpp src\qk_io.cpp `
  src\qk_search.cpp src\qk_cuda_candidate_generator.cpp `
  src\qk_path_optimizer.cpp src\qk_batcher.cpp src\qk_cases.cpp `
  src\qk_special_cases.cpp -lsqlite3 -o qk_special_cases.exe
```

Use this build for CPU Beam or `fingerprint128_disk` runs.

### CUDA layer-only build

Verified Windows direct build:

```powershell
cmd.exe /d /s /c '"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul && nvcc -std=c++17 -DQK_ENABLE_CUDA=1 -Itools -Iinclude -Xcompiler "/EHsc /utf-8 /Zc:preprocessor" src\qk_common.cpp src\qk_hypercube.cpp src\qk_io.cpp src\qk_search.cpp src\qk_cuda_candidate_generator.cu src\qk_path_optimizer.cpp src\qk_batcher.cpp src\qk_cases.cpp src\qk_special_cases.cpp tools\sqlite3_stub.cpp -o output\qk_special_cases_cuda.exe'
```

This direct CUDA build uses `tools\sqlite3_stub.cpp`, so it is intended for `layer_only` CUDA runs, not disk visited mode.

## CLI

```text
qk_special_cases.exe
  [min_dim=4] [max_dim=6] [beam_width=256] [max_depth=0]
  [exact_max_dim=4] [astar_cap=2000000] [exact_time_sec=30]
  [output_dir] [custom_cases_csv] [candidate_trace_mode=2]
  [case_name_filter] [beam_visited_mode=exact] [worker_threads=24]
  [disk_bloom_mb=1024] [disk_batch_size=1000000] [disk_shards=16]
  [path_opt_window=0] [path_opt_passes=1] [path_opt_node_cap=200000]
  [path_opt_segment_time_sec=0.25] [path_opt_threads=worker_threads]
  [path_opt_stride=0] [path_opt_word_reduce=1]
  [layer_pool_width=0] [layer_visited_window=0]
  [layer_restart_max_width=0] [layer_plateau_limit=0]
  [layer_restart_growth=2] [layer_perturbation_ratio=0.0]
  [candidate_backend=cpu] [beam_selection_policy=greedy] [cuda_topk_mode=cub]
```

Useful settings:

- `candidate_trace_mode=0`: large official runs.
- `case_name_filter=-`: no case filter.
- `exact_max_dim=0`: skip Strong A* entirely.
- `candidate_backend=cuda`: require CUDA support.
- `beam_selection_policy=diverse`: retain a more diverse beam.

## Official Run Examples

### Q4-Q11 CUDA all-run

```powershell
.\output\qk_special_cases_cuda.exe 4 11 512 0 0 2000000 30 `
  output\q4_q11_cuda_all_bw512_pool65536_win65536_restart2048_plateau2048_perturb010_cub_YYYYMMDD_HHMMSS `
  .\custom_qk_cases.csv 0 - `
  layer_only 24 1024 1000000 16 `
  0 1 200000 0.25 24 0 1 `
  65536 65536 2048 2048 2 0.10 cuda diverse cub
```

### Q12 case1 CUDA run

```powershell
.\output\qk_special_cases_cuda.exe 12 12 1024 1000000 0 2000000 30 `
  output\q12_cuda_gpu_retained_bw1024_pool262144_win65536_restart4096_plateau1536_perturb015_cub_path_YYYYMMDD_HHMMSS `
  .\custom_qk_cases.csv 0 q12_case1 `
  layer_only 24 1024 1000000 16 `
  0 1 200000 0.25 24 0 1 `
  262144 65536 4096 1536 2 0.15 cuda diverse cub
```

The preserved Q12 directory contains only official completion for `q12_case1`. A watchdog stopped the run before `q12_case2` could become an official result.

## Official Result Summary

### Q4-Q11 CUDA all-run

Source:

```text
output/q4_q11_cuda_all_bw512_pool65536_win65536_restart2048_plateau2048_perturb010_cub_20260613_113055/
```

| Dimension | Cases solved | Beam step range | Total expanded | Total Beam sec | Valid paths |
|---|---:|---:|---:|---:|---:|
| Q4 | 24/24 | 6-21 | 4,616,321 | 4.061 | 24/24 |
| Q5 | 2/2 | 40-44 | 3,205,037 | 2.607 | 2/2 |
| Q6 | 2/2 | 94-110 | 19,547,494 | 14.315 | 2/2 |
| Q7 | 2/2 | 244-268 | 116,436,146 | 38.269 | 2/2 |
| Q8 | 2/2 | 556-634 | 621,749,910 | 98.801 | 2/2 |
| Q9 | 2/2 | 1351-1464 | 3,315,878,036 | 265.926 | 2/2 |
| Q10 | 2/2 | 3019-3350 | 16,685,211,272 | 605.560 | 2/2 |
| Q11 | 2/2 | 6845-8048 | 85,866,033,678 | 1431.354 | 2/2 |

### Important Q8-Q12 cases

| Case | Strong LB | Beam steps | Expanded | Beam sec | Batcher swaps | Path valid |
|---|---:|---:|---:|---:|---:|---:|
| q8_case1 | 512 | 634 | 331,291,728 | 52.381 | 2,318 | yes |
| q8_case2 | 438 | 556 | 290,458,182 | 46.419 | 2,102 | yes |
| q9_case1 | 1152 | 1464 | 1,724,512,530 | 137.966 | 5,774 | yes |
| q9_case2 | 1059 | 1351 | 1,591,365,506 | 127.960 | 5,199 | yes |
| q10_case1 | 2560 | 3350 | 8,776,263,750 | 314.983 | 13,998 | yes |
| q10_case2 | 2305 | 3019 | 7,908,947,522 | 290.577 | 13,113 | yes |
| q11_case1 | 5632 | 8048 | 46,401,393,019 | 760.112 | 33,484 | yes |
| q11_case2 | 5023 | 6845 | 39,464,640,659 | 671.242 | 31,209 | yes |
| q12_case1 | 12288 | 28814 | 725,041,947,494 | 7892.204 | 79,014 | yes |

## Outputs

Typical special-case output files:

| File | Meaning |
|---|---|
| `qk_special_cases.csv` | final result, state, Beam path, Batcher path |
| `qk_case_progress.csv` | one row per completed case |
| `qk_beam_depth_progress.csv` | per-depth Beam progress |
| `qk_cuda_timing.csv` | CUDA timing breakdown |
| `qk_beam_candidate_trace.csv` | candidate trace, usually 0 bytes in large official runs |
| `qk_beam_disk_progress.csv` | disk visited progress only |

Route workbook:

```text
output/qk_special_cases_routes.xlsx
```

The workbook contains Q4-Q12 sheets. Q12 currently contains `q12_case1` only. Q12 was written with [tools/add_q12_routes_sheet_fast.py](tools/add_q12_routes_sheet_fast.py), which edits the workbook OpenXML directly to avoid loading the whole large workbook through a spreadsheet library.

## Preserved Artifact Directories

Key directories currently kept:

- `output/q4_q11_cuda_all_bw512_pool65536_win65536_restart2048_plateau2048_perturb010_cub_20260613_113055/`
- `output/q12_cuda_gpu_retained_bw1024_pool262144_win65536_restart4096_plateau1536_perturb015_cub_path_20260613_132118/`
- `output/q11_cuda_cub_bw512_pool65536_win65536_restart2048_plateau2048_perturb010_path_20260612_231014/`
- `output/q9_cuda_diverse_bw256_pool4096_win4096_perturb010_20260613_090936/`
- `output/q8_case1_trim_20260605_154219/`
- `output/q8_case2_trim_20260605_154927/`
- `output/q9_case1_disk_bw256_20260607_113040/`
- `output/q9_case2_disk_bw256_path_20260609_170556/`
- `output/cuda_opt_verify_q10_bw512_case1/`
- `output/cuda_fix_verify_q10_case2/`
- `output/q10_cuda_bw512_pool32768_win32768_restart1024_plateau1536_perturb010_case2_20260612_222245/`
- `output/qk_custom_cases_20260605_133247/`
- `output/q4_path_selected_10000_basic_no_path_20260604_205233/`

The last Q4 random benchmark directory is a historical artifact. New Q4 random work belongs on `codex/q4-random-test`.

## Cleanup Policy

Delete after small tests unless explicitly promoted:

- empty run directories
- aborted runs
- preview workbook renders
- `verify_*` smoke outputs
- stale `qk_beam_path_*.bin`
- small test outputs superseded by official runs

Do not delete automatically:

- official result directories listed in this README
- `output/qk_special_cases_routes.xlsx`
- user-requested partial progress
- tracked artifacts referenced by README

## Current Limits

- Q12 case2 is not officially solved yet.
- Q13-Q15 cases exist but have no official solved artifacts yet.
- CUDA accelerates candidate generation and Top-K preselection, but final exact retained selection and materialization still have CPU work.
- Beam is still heuristic. For Q12+ the main challenge is avoiding local best plateaus, not only increasing `beam_width`.
