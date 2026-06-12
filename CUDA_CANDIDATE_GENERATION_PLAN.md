# CUDA Candidate Generation Plan

## Goal

Move the expensive Beam candidate generation stage onto CUDA while preserving the current search semantics.

The CUDA path should be an optional backend for the existing `layer_only` Beam mode. It must not change:

- hypercube edge swap legality
- immediate reverse-edge skip rule
- layer visited-window filtering
- score ordering
- deterministic perturbation behavior
- parent back-pointer path reconstruction
- final path replay validation

CPU behavior remains the reference implementation and fallback.

## Local GPU Baseline

Measured on 2026-06-12 before implementation:

```text
GPU: NVIDIA GeForce RTX 5070 Ti Laptop GPU
Driver: 610.47
CUDA UMD: 13.3
CUDA Toolkit: nvcc 13.3.33
Compute capability: 12.0
CUDA devices: 1
Streaming multiprocessors: 46
Global memory: 12226-12227 MiB
Free memory at idle: about 11000 MiB
Memory bus width: 192-bit
L2 cache: 37748736 bytes, about 36 MiB
Warp size: 32
Max threads per block: 1024
Max resident threads per SM: 1536
Shared memory per block: 48 KiB
Shared memory per SM: 100 KiB
Max graphics/SM clock: 3090 MHz
Max memory clock: 14001 MHz
Default power limit: 65 W
Max power limit: 140 W
Driver model: WDDM
```

Build environment notes:

- `nvidia-smi` works.
- `nvcc` is installed at `C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.3\bin\nvcc.exe`.
- `cl.exe` is not available in the default PowerShell `PATH`.
- CUDA compilation works after loading Visual Studio Build Tools with `vcvars64.bat`.
- `nvcc` emits path/code-page warnings because the workspace path contains non-ASCII characters. For the CUDA implementation, prefer a short ASCII build directory or suppress harmless diagnostic `#192` during local probes.

## Q9 CUDA Smoke Result

Measured on 2026-06-12 with a standalone Q9 case1 smoke harness calling the same `generate_layer_only_candidates_cuda()` backend.

Command shape:

```text
q9_cuda_smoke.exe custom_qk_cases.csv q9_case1 256 256 4
```

Result:

```text
case=q9_case1 dim=Q9 nodes=512 edges=2304 beam_width=256 pool_width=256 depths=4
depth=1 used_cuda=1 layer_candidates=2304 returned=256 best_total_dist=2302 best_misplaced=504 sec=0.402236
depth=2 used_cuda=1 layer_candidates=589568 returned=256 best_total_dist=2300 best_misplaced=504 sec=0.0049185
depth=3 used_cuda=1 layer_candidates=589568 returned=256 best_total_dist=2298 best_misplaced=504 sec=0.004698
depth=4 used_cuda=1 layer_candidates=589568 returned=256 best_total_dist=2296 best_misplaced=504 sec=0.0050365
```

`used_cuda=1` confirms that the CUDA backend was invoked. The first depth includes CUDA runtime initialization; later depths are the useful steady-state smoke timing.

## Q9 CUDA Full Run Record

Measured on 2026-06-12 with the CUDA-enabled `qk_special_cases` binary.

Important comparison caveat: this run uses `beam_visited_mode=layer_only` with `candidate_backend=cuda`, while the old saved Q9 baseline uses `beam_visited_mode=fingerprint128_disk` on CPU. The runtime delta therefore includes both the removal of SQLite disk visited bookkeeping and CUDA candidate generation.

### Build

PowerShell did not have `cmake` available, so the binary was built directly with `nvcc` after loading Visual Studio Build Tools. A local SQLite stub was used only to satisfy unused disk-mode symbols; the run itself uses `layer_only` and does not call SQLite.

```powershell
cmd.exe /d /s /c '"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul && nvcc -std=c++17 -DQK_ENABLE_CUDA=1 -Itools -Iinclude -Xcompiler "/EHsc /utf-8 /Zc:preprocessor" src\qk_common.cpp src\qk_hypercube.cpp src\qk_io.cpp src\qk_search.cpp src\qk_cuda_candidate_generator.cu src\qk_path_optimizer.cpp src\qk_batcher.cpp src\qk_cases.cpp src\qk_special_cases.cpp tools\sqlite3_stub.cpp -o output\qk_special_cases_cuda.exe'
```

### Run

`Start-Process` failed in this shell because the environment contains duplicate `Path`/`PATH` keys, so the background process was launched through `cmd start /b`.

```powershell
cmd.exe /d /s /c 'start "" /b output\qk_special_cases_cuda.exe 9 9 256 0 0 2000000 30 output\q9_cuda_layer_bw256_pool4096_win4096_path_20260612_205946 .\custom_qk_cases.csv 0 - layer_only 24 1024 1000000 16 0 1 200000 0.25 24 0 1 4096 4096 0 0 2 0.0 cuda > output\q9_cuda_layer_bw256_pool4096_win4096_path_20260612_205946\stdout.log 2> output\q9_cuda_layer_bw256_pool4096_win4096_path_20260612_205946\stderr.log'
```

Run parameters:

- `beam_width=256`
- `max_depth=auto`, so Q9 uses `4608`
- `beam_visited_mode=layer_only`
- `layer_pool_width=4096`
- `layer_visited_window=4096`
- `layer_restart_max_width=0`
- `layer_plateau_limit=0`
- `layer_perturbation_ratio=0.0`
- `candidate_backend=cuda`
- `candidate_trace_mode=0`
- `path_opt_window=0`

### Result

| case | old CPU mode | old sec | CUDA mode | CUDA sec | speedup | old steps | CUDA steps | old expanded | CUDA expanded | path valid |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| q9_case1 | fingerprint128_disk | 14307.567 | layer_only | 72.977 | 196.06x | 1558 | 1578 | 909749194 | 929437315 | yes |
| q9_case2 | fingerprint128_disk | 10559.491 | layer_only | 65.780 | 160.53x | 1563 | 1445 | 914621697 | 851100798 | yes |

### Chunked Top-K Rerun

After adding chunked exact Top-K, Q9 was rerun with the same search parameters:

```text
output/q9_cuda_chunked_bw256_pool4096_win4096_path_20260612_212329
```

The rerun produced the same retained-search result as the previous CUDA run:

| case | status | steps | expanded | sec | path valid | versus old CPU disk |
|---|---:|---:|---:|---:|---:|---:|
| q9_case1 | solved | 1578 | 929437315 | 79.592 | yes | 179.76x |
| q9_case2 | solved | 1445 | 851100798 | 77.407 | yes | 136.41x |

Compared with the pre-chunk CUDA run, `steps` and `expanded` are identical for both Q9 cases, confirming that chunked Top-K preserved the search ordering for this run. Runtime is slightly slower on Q9 because the layer size is small enough that chunking mostly adds overhead; the change is intended for Q13+ and high-beam Q10/Q11 scaling.

## Current Hot Path

The current `layer_only` Beam loop in `src/qk_search.cpp` expands one layer at a time:

1. Iterate retained beam states.
2. Try every hypercube edge except the previous edge.
3. Swap the two tokens.
4. Compute candidate fingerprint.
5. Reject candidates found in the retained-layer visited window.
6. Score candidates by:
   - total distance
   - max packet distance
   - misplaced count
   - depth
   - edge id
   - packed key when available
   - fingerprint
   - deterministic order
7. Keep the best `layer_pool_width` candidates.
8. Retain `beam_width` candidates, optionally mixing in deterministic perturbation slots.
9. Append path back-pointers only for retained candidates.

The existing disk-mode helper already uses incremental score updates from parent distance histograms. The current `layer_only` parallel path still materializes a swapped state and recomputes several scores, so it is a good target for GPU acceleration and a small CPU-side refactor.

## Design Principles

- Preserve exact ordering first, optimize second.
- Do not use per-parent caps unless they can be proven equivalent to global Top-K.
- Do not transfer full candidate states back to CPU.
- Keep candidate records compact.
- Keep path reconstruction on CPU.
- Keep a CPU backend for all unsupported platforms and for correctness comparison.
- Add instrumentation before trusting speedups.

## Proposed Backend Switch

Add a new optional CLI argument at the end of `qk_special_cases`:

```text
candidate_backend=cpu|cuda|auto
```

Default stays `cpu` to avoid changing existing runs. `auto` uses CUDA only when:

- the binary was built with CUDA support
- the selected Beam mode is `layer_only`
- candidate trace mode is `none`
- token type supports the current Q dimension
- estimated VRAM usage fits the configured budget

If any condition fails, `auto` falls back to CPU and logs the fallback reason.

## Data Layout

### Parent Beam Data

Introduce a compact internal parent item for layer-only expansion:

```cpp
struct LayerBeamItem {
    State state;
    int last_edge;
    int path_node;
    Fingerprint128 fingerprint;
    int total_dist;
    int misplaced;
    std::array<uint16_t, 16> dist_hist;
};
```

The CPU backend can also use this structure, replacing repeated full-state score recomputation with incremental score updates.

For CUDA transfer:

- flatten states into a contiguous token array
- use `uint16_t` tokens for Q15 and below
- keep `uint32_t` as the planned fallback for Q16 and above
- copy edge endpoints as two compact arrays `edge_u[]`, `edge_v[]`
- copy parent metadata arrays separately for coalesced reads

### Candidate Record

CUDA should produce compact candidate metadata, not full states:

```cpp
struct GpuCandidate {
    int total_dist;
    int max_dist;
    int misplaced;
    int parent_index;
    int edge_id;
    Fingerprint128 fingerprint;
    uint64_t order;
};
```

The CPU materializes only retained states by copying the selected parent state and applying the selected edge swap.

## CUDA Work Split

### Kernel 1: Candidate Scoring

One logical candidate is `(parent_index, edge_id)`.

For each candidate:

1. Skip if `edge_id == parent.last_edge`.
2. Read `token_u` and `token_v` from the parent state.
3. Compute old/new hypercube distances.
4. Update total distance incrementally.
5. Update misplaced count incrementally.
6. Update a local copy of the 16-bin distance histogram and derive max distance.
7. Compute `fingerprint_after_swap` using the same `splitmix64` logic as CPU.
8. Reject candidate if it appears in the recent retained-layer visited set.
9. Emit compact candidate metadata.

### Kernel 2: Exact Top-K Per Chunk

A full Q15 layer can be very large:

```text
beam_width 512 * Q15 edges 245760 = 125829120 logical candidates
```

Do not sort or transfer all candidates at once. Instead:

1. Split the logical candidate range into chunks sized by VRAM budget.
2. For each chunk, generate valid candidate records.
3. Select exact Top-K for that chunk, where `K = layer_pool_width`.
4. Copy only the chunk Top-K records back to CPU or to a second GPU merge buffer.
5. Merge all chunk Top-K lists with the same comparator.

This is exact for global Top-K: a candidate that is not in its chunk Top-K cannot be in the global Top-K, because at least K candidates in the same chunk already rank better or equal.

### Kernel 3: Optional GPU Merge

MVP can merge chunk Top-K lists on CPU because the merged list is small:

```text
num_chunks * layer_pool_width
```

If CPU merge becomes visible in timing, move the final merge to CUDA later.

## Recent Visited Window on GPU

The current layer-only mode rejects candidates whose fingerprint is in the retained fingerprints from the previous K layers.

### Phase 1: Exact Copy-And-Filter

For initial implementation:

1. Keep the CPU `LayerVisitedWindow` as source of truth.
2. Export its unique fingerprints each depth.
3. Copy them to GPU.
4. Build a GPU lookup table for the current depth.
5. Filter candidates on GPU before Top-K.

This preserves search logic exactly. It may cost extra copy/setup time, but Q10/Q11 should be enough to validate correctness and measure whether candidate scoring is the true bottleneck.

### Phase 2: Persistent GPU Window

After correctness is stable:

1. Maintain retained fingerprints in GPU memory.
2. Add each retained layer to a GPU hash table.
3. Remove expired layers when the window exceeds K.
4. Keep CPU and GPU counts synchronized for debug builds.

This removes per-depth visited-window rebuild overhead.

## Candidate Ordering

CUDA and CPU must use the same total order:

1. lower `total_dist`
2. lower `max_dist`
3. lower `misplaced`
4. lower `depth`
5. lower `edge_id`
6. lower packed key when available
7. lower fingerprint
8. lower deterministic order

For Q9 and above, packed key is not used by the current code. For Q8 and below, keep CPU fallback first, then optionally implement packed-key tie support only if CUDA comparison testing needs exact Q8 parity.

Important: all CUDA comparisons must be integer-only. No floating point, no approximate scoring, no nondeterministic race-based tie break.

## Perturbation

Keep perturbation selection on CPU for the first CUDA version:

1. CUDA returns the exact sorted candidate pool.
2. CPU runs the existing `select_layer_only_retained`.
3. CPU appends path back-pointers.
4. CPU updates the retained-layer visited window.

This avoids changing deterministic perturbation semantics.

Later, perturbation can move to CUDA after candidate ordering and Top-K are proven identical.

## Build Structure

Add files:

```text
include/qk/cuda_candidate_generator.h
src/qk_cuda_candidate_generator.cpp
src/qk_cuda_candidate_generator.cu
```

The `.cpp` file exposes a normal C++ interface and contains the no-CUDA stub. The `.cu` file is compiled only when CUDA is enabled.

Add CMake option:

```cmake
option(QK_ENABLE_CUDA "Enable CUDA candidate generation backend" OFF)
```

When enabled:

- enable CUDA language or use `find_package(CUDAToolkit REQUIRED)`
- compile `.cu` source
- link CUDA runtime
- define `QK_ENABLE_CUDA=1`

When disabled:

- build exactly as today
- `candidate_backend=cuda` returns a clear error
- `candidate_backend=auto` falls back to CPU

## Instrumentation

Add a CUDA progress CSV:

```text
qk_beam_cuda_progress.csv
```

Columns:

```text
case_index,total_cases,dim,case_name,depth,
backend,parent_count,edge_count,logical_candidates,
h2d_sec,visited_build_sec,generation_sec,topk_sec,d2h_sec,
cpu_merge_sec,materialize_sec,selected,device_bytes,elapsed_sec
```

This matters because GPU speedups can disappear if host-device transfer, visited-window build, or CPU merge dominates.

## Correctness Plan

### Unit-Level Comparator

Add a debug mode that runs CPU and CUDA candidate generation for the same layer and compares:

- valid candidate count
- Top-K candidate ordering
- selected retained list
- solved-candidate detection
- parent index and edge id
- fingerprint
- score tuple

Use this before full Q runs.

### Smoke Tests

Run with `candidate_backend=cpu` and `candidate_backend=cuda`:

1. Q5 case1
2. Q6 case1
3. Q8 case1
4. Q9 case1

Expected:

- same `beam_status`
- same or intentionally explained `beam_steps`
- same path validity fields
- same retained ordering in compare mode

Q8 is useful because packed-key tie behavior may expose ordering differences. Q9 is useful because it exercises the large-state path where packed key is disabled.

### Performance Tests

After correctness:

1. Q10 case1 with `beam_width=512`
2. Q10 case1 with larger `layer_pool_width`
3. Q11 case1 dry run with early depth cap

Compare:

- seconds per depth
- logical candidates per second
- best total distance drop per depth
- CPU utilization
- GPU utilization
- VRAM usage
- host RAM usage

## Implementation Phases

## Current Implementation Status

Implemented first on 2026-06-12:

- Added `candidate_backend=cpu|cuda|auto` to `qk_special_cases`.
- Added `BeamCandidateBackend` and parser/name helpers.
- Added an optional CUDA candidate generator interface.
- Added a non-CUDA CPU-build stub, so the existing MinGW/g++ build remains the default path.
- Added `QK_ENABLE_CUDA` CMake option.
- Added `src/qk_cuda_candidate_generator.cu` MVP source.
- `candidate_backend=auto` falls back to CPU when the binary was not built with CUDA.
- `candidate_backend=cuda` now fails early with a clear message when CUDA is not built.
- The `.cu` source compiles to an object with `nvcc 13.3` after loading Visual Studio Build Tools and passing `/Zc:preprocessor`.
- Small smoke tests verified CPU fallback and explicit CUDA preflight behavior; temporary smoke outputs were removed.
- Added chunked exact Top-K on 2026-06-12. The CUDA backend no longer fails when one layer exceeds the old 16M logical-candidate MVP cap. It splits the layer into candidate chunks, sorts each chunk on GPU, copies each chunk Top-K to CPU, then merges the chunk Top-K lists with the same ordering comparator.
- Chunk size is selected from available VRAM with a 2 GiB reserve and a conservative sort-temporary multiplier. `QK_CUDA_CHUNK_CANDIDATES` can override the chunk size for smoke tests or tuning.
- Forced-chunk Q9 smoke test passed with `QK_CUDA_CHUNK_CANDIDATES=65536`, `q9_case1`, `max_depth=2`; temporary smoke outputs were removed.
- Added Phase 2 persistent-buffer optimization on 2026-06-12:
  - CUDA device buffers are reused across depths instead of allocating/freeing every layer.
  - Hypercube edge arrays stay resident on GPU unless dimension/edge count changes.
  - The recent fingerprint table stays resident on GPU. It is rebuilt only after eviction/resize, otherwise new retained fingerprints are inserted incrementally.
  - CUDA event timing is written to `qk_cuda_timing.csv`.
  - CUDA retained selection now keeps compact candidate records first and materializes full states only for retained candidates.

Validated optimized results:

| case | backend | beam_width | pool_width | steps | expanded | sec | path valid | comparison |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| q9_case1 | cuda | 256 | 4096 | 1578 | 929437315 | 24.423 | yes | same steps/expanded as chunked CUDA, 3.26x faster than 79.592s |
| q10_case1 | cuda | 512 | 4096 | 3308 | 8666113160 | 298.732 | yes | same steps/expanded as previous CUDA bw512, 2.39x faster than 712.812s |

For `q10_case1`, `qk_cuda_timing.csv` contains 3308 depth rows. The measured generation timing sums are:

| stage | sum sec | avg sec/depth |
|---|---:|---:|
| host_prepare | 5.305 | 0.001604 |
| device_alloc | 0.008 | 0.000002 |
| H2D | 2.176 | 0.000658 |
| recent_update | 0.140 | 0.000042 |
| kernel | 2.548 | 0.000770 |
| sort | 55.766 | 0.016858 |
| D2H | 0.531 | 0.000161 |
| CPU merge | 0.944 | 0.000286 |
| retained_select | 2.947 | 0.000891 |
| materialize | 1.014 | 0.000307 |
| measured total | 72.441 | 0.021899 |

Added a follow-up overhead fix on 2026-06-12:

- The CPU search loop no longer rebuilds the complete recent fingerprint vector on every CUDA layer.
- The host side now tracks whether the resident GPU recent table is ready and how many slots it has.
- Full recent snapshots are built only for first use, visited-window eviction, or table growth.
- Normal layers pass only the retained-layer delta fingerprints.
- `qk_cuda_timing.csv` now includes outer timing columns:
  - `outer_parent_prepare_sec`
  - `outer_recent_snapshot_sec`
  - `outer_cuda_call_sec`
  - `outer_candidate_convert_sec`
  - `outer_progress_write_sec`
  - `outer_recent_add_sec`
  - `outer_layer_sec`
- CPU perturbation selection now uses a heap to read choices in the same deterministic key order without sorting the whole choice list.
- An exact GPU Top-K replacement was attempted with `thrust::nth_element` / `thrust::partial_sort`, but the local CUDA 13.3 CCCL/Thrust headers do not expose those algorithms.
- Added selectable CUDA Top-K modes on 2026-06-12:
  - default `full_sort`: original full `thrust::sort` path.
  - `QK_CUDA_TOPK_MODE=tiled`: exact tiled sort scaffold. Each tile keeps K candidates, then CPU merges tile Top-K lists exactly. This is correctness scaffolding and can be slower because it copies K candidates per tile.
  - `QK_CUDA_TOPK_MODE=cub`: exact CUB prefix-filter path. CUB `DeviceTopK::MinPairs` finds a prefix threshold, then all candidates with `prefix_key <= threshold` are selected, copied back, sorted with the original full comparator, and trimmed to K. If the prefix tie set is too large, the mode falls back to full sort.
  - `QK_CUDA_TOPK_TIE_CAP` controls the maximum selected prefix-tie set before CUB mode falls back to full sort. Default is `max(8 * layer_pool_width, 1048576)`.
- `qk_cuda_timing.csv` now reports `topk_mode`, `topk_tiles`, `topk_tile_candidates`, and `topk_select_sec`.
  - `topk_mode=0`: full sort.
  - `topk_mode=1`: tiled exact Top-K.
  - `topk_mode=2`: CUB exact prefix-filter.
  - `topk_mode=3`: CUB requested but full-sort fallback was used.

Follow-up validation:

| case | params | status | steps | expanded | old sec | new sec | speedup | path valid |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| q9_case1 | bw256 pool4096 win4096 perturb0.00 | solved | 1578 | 929437315 | 24.423 | 10.682 | 2.29x | yes |
| q10_case2 | bw512 pool32768 win32768 restart1024 plateau1536 perturb0.10 | solved | 3077 | 8060925432 | 340.545 | 117.697 | 2.89x | yes |

For the optimized `q10_case2`, the new outer timing shows:

| stage | sum sec |
|---|---:|
| outer_recent_snapshot | 0.135 |
| outer_cuda_call | 76.587 |
| outer_layer | 117.324 |
| sort | 51.794 |
| retained_select | 7.166 |
| CPU merge | 10.381 |
| measured total | 84.587 |

Additional Top-K mode validation:

| case | params | mode | status | steps | expanded | sec | path valid | notes |
|---|---|---|---:|---:|---:|---:|---:|---|
| q9_case1 | bw256 pool4096 win4096 perturb0.00 | full_sort | solved | 1578 | 929437315 | 10.682 | yes | reference after persistent-buffer fixes |
| q9_case1 | bw256 pool4096 win4096 perturb0.00 | tiled | solved | 1578 | 929437315 | 15.878 | yes | exact but slower on Q9 |
| q9_case1 | bw256 pool4096 win4096 perturb0.00 | cub | solved | 1578 | 929437315 | 7.905 | yes | 1577 CUB layers, 1 startup fallback |

Q10 case2 depth-300 smoke validation:

| mode | depth | expanded | best_total_dist | best_misplaced | best_max_dist | elapsed_sec |
|---|---:|---:|---:|---:|---:|---:|
| full_sort reference | 300 | 783561488 | 4008 | 932 | 8 | 9.777 |
| cub | 300 | 783561488 | 4008 | 932 | 8 | 8.429 |

The Q10 depth-300 curves match exactly, so the CUB prefix-filter path did not change retained search order for this smoke test.

MVP limits:

- The CUDA backend currently supports Q9+ layer-only mode only; Q8 and below still need packed-key tie ordering for exact CPU parity.
- The CUDA source uses persistent layer buffers plus selectable Top-K backends. The old 16M hard failure cap has been replaced by chunked exact Top-K.
- Full `qk_special_cases` CUDA linking through CMake is still not validated because `cmake` is not on PATH. The current validated Windows CUDA build uses `nvcc` with MSVC Build Tools and a local SQLite stub for unused disk-mode symbols; CUDA runs must use `layer_only`, not `fingerprint128_disk`.
- CUDA progress timing is written to `qk_cuda_timing.csv`.

### Phase 0: CPU Refactor and Metrics

- Add `LayerBeamItem` with cached scores and distance histogram.
- Make CPU layer-only generation use incremental scoring.
- Add generation/selection/materialization timing to progress output.
- Verify Q8/Q9/Q10 results do not change.

This phase may already speed up CPU runs and gives a cleaner CUDA boundary.

### Phase 1: CUDA MVP, CPU Merge

- Add optional CUDA backend and build flag.
- Copy beam states and metadata to GPU each depth.
- Copy recent visited fingerprints to GPU each depth.
- Generate compact candidates on GPU.
- Do exact chunk Top-K on GPU.
- Copy chunk Top-K records to CPU.
- Merge and retain on CPU.
- Materialize retained states on CPU.

Target: correctness and measurable Q10 speedup.

### Phase 2: Persistent GPU Buffers

- Keep edge arrays resident on GPU. Implemented.
- Reuse device candidate buffers across depths. Implemented.
- Keep recent fingerprint table resident and apply incremental inserts. Implemented.
- Add CUDA event timing for H2D, recent update, kernel, sort, D2H, CPU merge, retained selection, and materialization. Implemented.
- Use pinned host memory for transfer. Not implemented.
- Double-buffer beam state transfer when useful. Not implemented.
- Add VRAM budget based chunk sizing. Implemented in the chunked Top-K path.

Target: reduce per-depth overhead and CPU idle gaps.

### Phase 3: GPU Recent-Window Table

- Keep recent retained fingerprints resident on GPU.
- Update layer additions and expirations incrementally.
- Keep debug mode to compare CPU/GPU visited decisions.

Target: avoid rebuilding/copying the visited window every depth.

### Phase 4: Larger Q Scaling

- Tune chunk size for 12 GB VRAM.
- Test Q11 and Q12 with early stopping/depth caps first.
- Increase beam/pool only after throughput is known.
- Decide whether Q13-Q15 need larger beam width, larger pool width, or different search strategy.

## Risk List

- CUDA Top-K can accidentally change search ordering if tie keys are incomplete.
- Recent visited filtering must happen before Top-K to preserve exact logic.
- Copying the recent visited window every depth may become the new bottleneck.
- Full candidate sorting for Q15 can exceed VRAM; chunked exact Top-K is required.
- Packed-key tie support for Q8 and below is awkward on GPU; CPU fallback may be cleaner.
- Windows CUDA/CMake setup can be fragile, so CUDA must remain optional.
- GPU acceleration only helps candidate arithmetic/selection; it will not fix a search strategy that needs too many depths.

## Definition of Done

The CUDA candidate backend is acceptable when:

- CPU backend still builds and runs without CUDA.
- CUDA backend builds behind `QK_ENABLE_CUDA`.
- Q5/Q6/Q8/Q9 compare mode shows identical retained candidates for tested depths.
- Q10 case1 solves with a valid full path.
- `qk_cuda_timing.csv` shows where time is spent.
- `candidate_backend=auto` safely falls back when CUDA is unavailable.
- README documents build flags, runtime arguments, and the first validated CUDA result.
