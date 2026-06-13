# Qk Special-Case Hypercube Token Swapping

本分支目前的主軸是 `Qk` hypercube 上的 special-case token swapping solver。它不再是 Q3/Q4 隨機 benchmark 的主要分支，而是用來維護 Q4 以上指定 permutation、Beam Search、CUDA candidate generation、完整路徑輸出與正式結果紀錄。

目前工作分支：

- `codex/sp`：本 README 對應的 special-case / CUDA Beam 分支。
- `codex/q4-random-test`：Q4 full-random benchmark 已有自己的分支；本分支只保留一份歷史 Q4 random output 作為參考。

## 目前狀態

截至 `2026-06-13`，本分支的正式狀態如下：

| 範圍 | 狀態 | 主要資料來源 |
|---|---|---|
| Q4 custom cases | 24/24 solved | `output/q4_q11_cuda_all_bw512_pool65536_win65536_restart2048_plateau2048_perturb010_cub_20260613_113055/` |
| Q5-Q11 special cases | 每個維度 2/2 solved | 同上 |
| Q12 | `q12_case1` solved，`q12_case2` 尚未列為正式完成 | `output/q12_cuda_gpu_retained_bw1024_pool262144_win65536_restart4096_plateau1536_perturb015_cub_path_20260613_132118/` |
| Q13-Q14 | cases 已加入 `custom_qk_cases.csv`，尚未正式完成 | `custom_qk_cases.csv` |
| Q15 | 1 個 case 已加入，尚未正式完成 | `custom_qk_cases.csv` |
| 完整路徑 Excel | Q4-Q12 分頁已建立，Q12 目前只有 `q12_case1` | `output/qk_special_cases_routes.xlsx` |

`custom_qk_cases.csv` 目前包含：

| Dimension | Case count |
|---|---:|
| Q4 | 24 |
| Q5-Q14 | 2 each |
| Q15 | 1 |

結果目錄的保留規則與目前官方資料來源整理在 [QK_RESULTS_INDEX.md](QK_RESULTS_INDEX.md)。

## 問題模型

對於 `Qd` hypercube：

- 節點數：`N = 2^d`
- 節點 label：`0..N-1`
- 合法邊：兩個節點的 binary label 只差 1 bit
- 狀態：`state[node] = token`
- 目標：identity permutation，也就是 `state[i] = i`
- 操作：每一步只能沿一條 hypercube edge swap 兩端 token

這是 zero-buffer token swapping：沒有額外暫存節點，沒有 token 疊放。

## 程式模組

核心 special-case solver 位於 `include/qk/` 與 `src/qk_*.cpp`：

| 模組 | 責任 |
|---|---|
| `qk_common` | state、edge、swap step、fingerprint、packed key |
| `qk_hypercube` | hypercube edges、distance、lower bound、path replay validation |
| `qk_search` | Strong A*、Beam Search、disk visited、layer-only visited、path back-pointer |
| `qk_cuda_candidate_generator` | CUDA layer-only candidate generation、CUB Top-K、GPU diverse preselection |
| `qk_batcher` | deterministic Batcher baseline |
| `qk_cases` | built-in cases 與 `custom_qk_cases.csv` parser |
| `qk_io` | CSV escaping、progress output、path formatting |
| `qk_path_optimizer` | 已保留的 path post-processing 工具，目前正式結果未啟用 |
| `qk_special_cases.cpp` | CLI、runner、CSV output、overall orchestration |

Q3 baseline 與 Q4 random benchmark 的舊程式仍在 repository 中，但不再是本分支 README 的主體。

## Lower Bound

程式輸出的 `strong_lb` 由下列三個 lower bounds 取最大值，再做 permutation parity 調整：

```text
basic_lb = ceil(total_hamming_distance / 2)
max_packet_distance = max token-to-target Hamming distance
cycle_lower_bound = N - number_of_cycles
strong_lb = parity_adjust(max(basic_lb, max_packet_distance, cycle_lower_bound))
```

`parity_adjust` 會把 lower bound 調整到與 permutation parity 相同的步數奇偶性。這是 lower bound，不代表 Beam 必須接近它才算正常；Q12 目前仍明顯超過 lower bound，表示搜尋仍有改進空間。

## 搜尋方法

### Strong A*

`astar_exact()` 使用 `strong_lb` 作為 heuristic，可在小維度做精確驗證。實務上本分支主要只讓 Q4 以內使用 exact search；Q5 以上狀態空間過大，正式 special-case run 以 Beam 與 Batcher 為主。

### Batcher Baseline

`batcher_baseline()` 是 deterministic compare-exchange routing baseline。它通常一定能產生合法路徑，但 swap 數遠高於 Beam。它的定位是穩定 baseline，不是最佳化搜尋。

### Beam Search

Beam 是目前主 solver。每一層從 retained beam states 出發，嘗試所有 hypercube edge swap，產生下一層 candidates，然後保留有限數量的候選繼續搜尋。

候選排序的主要分數是：

```text
total_dist
max_dist
misplaced
depth
edge_id
fingerprint / packed key / generation order
```

這是 heuristic search，不保證 optimal。所有正式結果都會做 path replay 驗證，確認每一步都是合法 hypercube edge swap，且最終抵達 identity。

## Beam Visited Modes

CLI 參數 `beam_visited_mode` 目前支援四種模式：

| 模式 | 用途 |
|---|---|
| `exact` | 儲存完整 packed state，適合 Q8 以內 |
| `fingerprint128` | 128-bit fingerprint 儲存在 RAM |
| `fingerprint128_disk` | 128-bit fingerprint 分片寫入 SQLite，曾用於 Q9 舊正式解 |
| `layer_only` | 只保留當前 beam 與前 K 層 retained fingerprints，不保存全域 visited set |

目前 CUDA 與高維測試都以 `layer_only` 為主。這個模式的重點是：

- 不建立 SQLite visited database。
- `qk_beam_candidate_trace.csv` 通常設為 0 bytes，避免巨量 I/O。
- path 以 parent back-pointer 記錄，搜尋結束後才回溯完整路徑。
- `layer_visited_window` 控制前 K 層 retained fingerprints 的近期去重。
- `layer_pool_width` 控制每層先保留的 candidate pool，再降到 `beam_width`。
- `layer_perturbation_ratio` 用 deterministic perturbation 保留一部分非 greedy 候選，降低 tail 階段卡在 local best 的機率。

## CUDA Backend

CUDA backend 是 `layer_only` Beam 的候選產生加速器，不是獨立 solver。

目前真實分工是：

- GPU：candidate generation、score 計算、近期 fingerprint table 檢查、CUB DeviceTopK preselection。
- GPU：`diverse` policy 下可做多個 key mode 的 Top-K preselection，包括 greedy、max-distance、misplaced、edge-bit 與 perturbation。
- CPU：最後小集合的 exact comparator 精排、retained 去重、parent path node 寫入與 retained state materialize。

所以 `candidate_backend=cuda` 代表候選產生和 Top-K 前段會用 CUDA；它不代表整個 Beam layer 完全不經 CPU。

`cuda_topk_mode`：

| 模式 | 說明 |
|---|---|
| `cub` | 目前推薦；使用 CUB DeviceTopK 與 refined tie handling |
| `tiled` | 分 tile 選候選後 CPU merge，主要保留作比較 |
| `full_sort` | 舊版整層 GPU full sort，現在主要作診斷或 fallback |

在 timing CSV 中：

- `topk_mode=2` 代表 CUB/refined Top-K，是目前期待狀態。
- `topk_mode=0` 代表 full sort，通常不希望在大型正式 run 中出現。
- `outer_layer_sec` 是整個 Beam layer 的 wall time，包含 GPU call、CPU retained selection、materialize、progress write 等。

## 建置方式

### CPU / disk mode build

需要 C++17、Threads、SQLite3。MinGW 範例：

```powershell
g++ -std=c++17 -O2 -Wall -Wextra -pedantic -Iinclude `
  src\qk_common.cpp src\qk_hypercube.cpp src\qk_io.cpp `
  src\qk_search.cpp src\qk_cuda_candidate_generator.cpp `
  src\qk_path_optimizer.cpp src\qk_batcher.cpp src\qk_cases.cpp `
  src\qk_special_cases.cpp -lsqlite3 -o qk_special_cases.exe
```

這個 build 適合 CPU Beam、`fingerprint128_disk` 與 SQLite visited runs。

### CUDA layer-only build

Windows 上目前驗證過的直接 build 方式如下。這會輸出 `output\qk_special_cases_cuda.exe`：

```powershell
cmd.exe /d /s /c '"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul && nvcc -std=c++17 -DQK_ENABLE_CUDA=1 -Itools -Iinclude -Xcompiler "/EHsc /utf-8 /Zc:preprocessor" src\qk_common.cpp src\qk_hypercube.cpp src\qk_io.cpp src\qk_search.cpp src\qk_cuda_candidate_generator.cu src\qk_path_optimizer.cpp src\qk_batcher.cpp src\qk_cases.cpp src\qk_special_cases.cpp tools\sqlite3_stub.cpp -o output\qk_special_cases_cuda.exe'
```

這個 direct CUDA build 使用 `tools\sqlite3_stub.cpp`，目標是跑 `layer_only` CUDA，不適合拿來跑 `fingerprint128_disk`。

CMake 也保留 `QK_ENABLE_CUDA`：

```powershell
cmake -S . -B build -DQK_ENABLE_CUDA=ON
cmake --build build --config Release --target qk_special_cases
```

## CLI 參數

`qk_special_cases` 參數順序：

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

常用值：

- `candidate_trace_mode=0`：正式大型 run 使用，不記錄 candidates。
- `case_name_filter=-`：不過濾 case。
- `exact_max_dim=0`：完全不跑 Strong A*，只跑 Beam 與 Batcher。
- `candidate_backend=cuda`：強制使用 CUDA backend，若 binary 不支援 CUDA 會直接失敗。
- `beam_selection_policy=diverse`：使用多樣性 retained selection。

## 正式 Run 範例

### Q4-Q11 CUDA all-run

目前 Q4-Q11 官方比較資料來自這組設定：

```powershell
.\output\qk_special_cases_cuda.exe 4 11 512 0 0 2000000 30 `
  output\q4_q11_cuda_all_bw512_pool65536_win65536_restart2048_plateau2048_perturb010_cub_YYYYMMDD_HHMMSS `
  .\custom_qk_cases.csv 0 - `
  layer_only 24 1024 1000000 16 `
  0 1 200000 0.25 24 0 1 `
  65536 65536 2048 2048 2 0.10 cuda diverse cub
```

### Q12 case1 CUDA GPU-retained run

Q12 case1 目前正式結果使用較大的 pool：

```powershell
.\output\qk_special_cases_cuda.exe 12 12 1024 1000000 0 2000000 30 `
  output\q12_cuda_gpu_retained_bw1024_pool262144_win65536_restart4096_plateau1536_perturb015_cub_path_YYYYMMDD_HHMMSS `
  .\custom_qk_cases.csv 0 q12_case1 `
  layer_only 24 1024 1000000 16 `
  0 1 200000 0.25 24 0 1 `
  262144 65536 4096 1536 2 0.15 cuda diverse cub
```

實際保存資料夾中曾有 watchdog 讓 process 在 `q12_case1` 完成後停止，因此 `q12_case2` 只留下前 17 層 partial depth/timing rows，不列入正式完成結果。

## 目前正式結果摘要

### Q4-Q11 CUDA all-run

來源：

```text
output/q4_q11_cuda_all_bw512_pool65536_win65536_restart2048_plateau2048_perturb010_cub_20260613_113055/
```

設定：

- `beam_width=512`
- `beam_visited_mode=layer_only`
- `layer_pool_width=65536`
- `layer_visited_window=65536`
- `layer_restart_max_width=2048`
- `layer_plateau_limit=2048`
- `layer_perturbation_ratio=0.10`
- `candidate_backend=cuda`
- `beam_selection_policy=diverse`
- `cuda_topk_mode=cub`

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

### Q8-Q12 important cases

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

Q12 case1 來源：

```text
output/q12_cuda_gpu_retained_bw1024_pool262144_win65536_restart4096_plateau1536_perturb015_cub_path_20260613_132118/
```

## 輸出檔案

每個 special-case run 通常產生：

| 檔案 | 說明 |
|---|---|
| `qk_special_cases.csv` | 最終結果，包含 state、Beam path、Batcher path |
| `qk_case_progress.csv` | 每完成一個 case 寫一列，適合監控 |
| `qk_beam_depth_progress.csv` | 每層 Beam progress |
| `qk_cuda_timing.csv` | CUDA backend timing breakdown |
| `qk_beam_candidate_trace.csv` | 候選 trace，正式大型 run 通常維持 0 bytes |
| `qk_beam_disk_progress.csv` | disk visited mode 才會有內容 |

完整路徑 Excel：

```text
output/qk_special_cases_routes.xlsx
```

目前 Excel 包含 Q4-Q12 sheets，其中 Q12 只有 `q12_case1`。Q12 sheet 由 [tools/add_q12_routes_sheet_fast.py](tools/add_q12_routes_sheet_fast.py) 直接寫 OpenXML，避免一般 Excel library 重新載入整本 workbook 時卡住。

一般 workbook 更新工具：

```text
tools/update_qk_routes_workbook.py
```

## 目前保留的主要資料夾

正式或仍有參考價值的資料夾：

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

`output/q4_path_selected_10000_basic_no_path_20260604_205233/` 是 Q4 random benchmark 的歷史 artifact。新的 Q4 random work 請到 `codex/q4-random-test` 分支。

## 清理政策

小規模測試後預設清理：

- 空資料夾
- aborted run
- preview workbook render
- `verify_*` smoke output
- stale `qk_beam_path_*.bin`
- 已被新正式結果取代的小測試資料

不要自動刪除：

- `QK_RESULTS_INDEX.md` 列出的正式結果
- `output/qk_special_cases_routes.xlsx`
- 使用者明確指定要保留的 partial progress
- Git 已追蹤且 README 正在引用的 artifact

## 目前限制與下一步

- Q12 case2 尚未正式完成。
- Q13-Q15 cases 已加入，但還沒有正式 solved artifact。
- CUDA 已把候選產生與 Top-K preselection 移到 GPU，但 final exact retained selection 和 materialization 仍有 CPU 成分。
- Beam 仍是 heuristic search。加寬 `beam_width` 可以增加機會，但高維 tail plateau 顯示主體搜尋策略仍需要改進，不能只靠加寬度。
- Q12 以上應優先研究更能避免 local best 的通用策略，而不是針對單一 permutation 寫特化解法。
