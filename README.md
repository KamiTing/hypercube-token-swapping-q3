# Q3-Q10 Hypercube Token Swapping

這個專案實作並驗證 hypercube 上的 zero-buffer token swapping，範圍包含：

- **Minimum Token Swapping on Q3 Hypercube**
- **Zero-buffer edge-swap routing shortest path**
- **Q4 full-random benchmark**
- **Q4-Q10 special-case Beam Search**

在三維超立方體 `Q3`（8 個節點）上，每一步只能沿合法邊交換兩端 token，目標是將任意初始 permutation 轉為 `(0,1,2,3,4,5,6,7)` 並最小化交換步數。

## 問題模型

- 節點：`0..7`，對應 3-bit binary label。
- 邊：兩節點 binary 只差 1 bit 才有邊。
- 狀態表示：`state[node] = token`。
- 操作：每次只能做一條 hypercube 邊上的 swap。

這符合 zero-buffer routing：沒有額外暫存、沒有 token 疊放。

## 方法

- **BFS**：真值表（最短步數 baseline）
- **A\***：精確搜尋（admissible heuristic）
- **Beam Search**：heuristic 搜尋
- **Batcher baseline**：固定 compare-exchange sorting-network

A\* heuristic：

`h(state) = ceil(total_hamming_distance(state) / 2)`

因為一次 swap 最多讓兩個 token 各靠近目標一步，所以總距離最多下降 2，故此 heuristic 不高估。

## Beam Search 設計重點

### 核心概念

Greedy search 每一步只保留單一路徑，容易因為局部最佳而繞路。  
Beam Search 改為「逐層保留多條高分候選路徑」：每層展開後只留下前 `beam_width` 個狀態，兼顧效率與穩定性。

本專案設定：

- `BEAM_WIDTH = 14`
- `MAX_DEPTH = 12`

### 狀態與候選資訊

每個候選節點（`BeamItem`）保存：

- `state`：目前排列
- `depth`：已使用交換步數
- `last_edge_id`：上一條交換邊（避免立即反悔）
- `used_edges`：每條邊已使用次數（用於重複邊懲罰）

### 每層展開方式

對當前 beam 內每個狀態，嘗試所有合法 hypercube 邊交換，產生下一層候選：

`next_state = swap_nodes(state, e.u, e.v)`

Q3 共有 12 條邊，因此每個狀態每層最多展開 12 個候選（扣除剪枝與禁忌邊）。

### 去重與剪枝

使用 `visited_best_depth[state]` 記錄狀態最早到達深度。若新路徑深度不更好則略過，避免重複搜尋與無效繞圈。

另外，`last_edge_id` 會阻止「立刻用同一條邊反向交換」的無效動作。

### 評分函數（排序優先序）

候選依 `BeamScore` 做 tuple-like 比較，越小越優先，依序為：

1. `total_dist`：所有 token 到目標的總 Hamming distance
2. `misplaced`：錯位 token 數
3. `max_dist`：最遠 token 距離
4. `repeat_penalty`：重複使用邊的懲罰
5. `-improvement`：本步對總距離改善量（改善越大越優）
6. `-local_improvement`：被交換兩個 token 的局部改善量
7. `-dir_score`：維度方向偏好（熱門修正 bit 方向優先）
8. `-touched_max_dist`：優先處理較遠 token
9. `depth`：平手時偏好較淺層路徑

### 終止條件

- 初始即目標：回傳 `0`
- 生成候選時到達目標：回傳當前 `depth`
- 搜尋達 `MAX_DEPTH`：停止並回傳失敗（`-1`）

### 方法定位

Beam Search 是 heuristic search，理論上不保證 optimal；但在本專案 Q3 全排列（40320 states）測試中，使用上述評分與參數可達成與 BFS true table 完全一致的最短步數結果。

## 實驗範圍

- 全排列測試：`8! = 40320` states。
- 對每個初始狀態計算到目標狀態的步數，並比較不同方法。

## 目前參數（推薦）

在本專案目前實作下，經全狀態掃描回推：

- `BEAM_WIDTH = 14`
- `MAX_DEPTH = 12`

這組在 Q3 上可維持與 BFS 一致的最優結果，且比寬鬆設定（例如 100/30）更快。

## 專案結構

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
│   ├── q9_case2_disk_bw256_path_20260609_170556/
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

## 建置與執行

### 1) 編譯 C++ 主程式（MinGW g++）

```powershell
g++ -std=c++17 -O2 -fopenmp src/main.cpp src/hypercube.cpp src/search.cpp src/batcher.cpp src/report.cpp -Iinclude -o hypercube_refactor.exe
```

### 2) 執行實驗

```powershell
.\hypercube_refactor.exe
```

輸出會寫入 `output/`：

- `output/hypercube_report.txt`
- `output/step_distribution.csv`

### 3) 產生統計表與圖

```powershell
.\.venv\Scripts\python.exe plot_distribution.py
```

會產生：

- `output/step_distribution_summary.csv`
- `output/bfs_step_distribution.png`
- `output/method_step_distribution_compare.png`

## 主要結果摘要（Q3 全測）

- A\* 與 BFS 全部一致（40320/40320）。
- Beam（14/12）與 BFS 全部一致（40320/40320）。
- Batcher baseline 可解但非最短路，最優率約 `1.87%`。

## Q4 完全隨機測試

Q4 (`DIM=4`) full-random benchmark 的每個 case 由 Fisher-Yates shuffle 均勻抽樣自全部 `16!` token placements，不使用從目標狀態 random walk 的 `scramble_steps`。

- 程式：`src/q4_random_benchmark.cpp`
- 可視化：`plot_q4_random.py`
- 比較方法：Basic A*、Strong A*、Beam Search、Batcher's baseline

Basic A* heuristic：

`h_basic = ceil(total_hamming_distance / 2)`

Strong A* heuristic：

`h_strong = parity_adjust(max(ceil(total_hamming_distance / 2), max_packet_distance, cycle_lower_bound))`

其中 `cycle_lower_bound = N - number_of_cycles`，`parity_adjust` 會將 lower bound 調整到與目前 permutation parity 相同的步數奇偶性。

### 執行 Q4 benchmark

```powershell
g++ -std=c++17 -O2 src/q4_random_benchmark.cpp -o q4_random_benchmark_run.exe
.\q4_random_benchmark_run.exe 10000 128 30 42 0 1 1 output\q4_path_selected_10000_basic_no_path_YYYYMMDD_HHMMSS
```

參數順序：

- `samples beam_width max_depth seed astar_cap parallel_methods record_paths [output_dir]`
- `astar_cap = 0` 表示不限制 A* visited-state 數量
- `parallel_methods = 1` 表示同一個 case 的四種方法用平行執行
- `record_paths = 1` 會輸出 detailed routing paths；若大型測試要節省記憶體可設為 `0`
- 為了避免 Q4 hard case 的記憶體爆量，Basic A* 不輸出 path；`astar_path` 會留空，只保留 `astar_steps`、expanded nodes 與時間。Strong A*、Beam、Batcher 會輸出完整 path。
- `output_dir` 可省略；預設為 `output`。建議 path-enabled 測試指定獨立資料夾，避免覆蓋既有大型 benchmark。

### 產生 Q4 圖表

```powershell
.\.venv\Scripts\python.exe plot_q4_random.py output\q4_path_selected_10000_basic_no_path_YYYYMMDD_HHMMSS
```

### Q4 輸出檔案

- `output_dir/q4_random_benchmark.csv`
- `output_dir/q4_random_routing_paths.csv`
- `output_dir/q4_random_summary.csv`
- `output_dir/q4_steps_hist_compare.png`
- `output_dir/q4_time_boxplot.png`
- `output_dir/q4_gap_vs_batcher_hist.png`

`q4_random_routing_paths.csv` 是逐 selected permutation 的 detailed routing path 紀錄。每列包含初始 state、Basic A*/Strong A*/Beam/Batcher 的步數；其中 Basic A* 的 `astar_path` 為空，Strong A*、Beam、Batcher 會輸出實際 edge-swap sequence。Q4 節點包含 `10..15`，因此路徑格式使用 `u-v`，例如 `0-1 10-14`。

### 目前大型測試（10000 full-random samples）

- 輸出資料夾：`output/q4_path_selected_10000_basic_no_path_20260604_205233`
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
- Routing path 驗證：`astar_path` 全部留空；Strong A*、Beam、Batcher path 皆抵達 Q4 goal，且 path 長度與 step/swap 欄位一致。

### 測試數據分析

1. Strong A* 保持與 Basic A* 相同步數。
在 10000 筆 full-random case 中，兩者皆成功且 `10000/10000` 步數一致；因兩者都是 admissible A*，這代表目前樣本中 Strong heuristic 沒有改變 optimal 解，只降低搜尋成本。

2. Strong heuristic 明顯減少展開量。
平均 expanded nodes 從 `87167.9328` 降到 `4287.0348`，約為 `20.3x` 展開量改善；平均時間從 `0.841673s` 降到 `0.073471s`，約為 `11.5x` 改善。

3. Beam 仍是最快的 heuristic search。
Beam 平均 `0.018573s`，但它是剪枝式 heuristic，不保證 optimal；本次 `10000/10000` 皆成功，平均步數 `17.2270`，略高於 Basic A*/Strong A* 的 `17.1852`。

4. Batcher baseline 速度最快但交換次數最多。
Batcher 是固定 compare-exchange network，不做搜尋；平均 `39.9200` swaps，約為 Strong A* 平均最短步數的 `2.32x`。

5. 研究定位。
Basic A* 與 Strong A* 都是使用 admissible heuristic 的 A* 變體；Beam 適合作為快速 heuristic baseline；Batcher 適合作為 deterministic routing baseline。

備註：輸出 CSV 中的 `astar_*` 欄位對應 Basic A*。

## Q4-Q10 Special Cases（`codex/sp`）

SP 分支加入模組化的 `qk` special-case runner，用來執行 Q4 以上指定 permutation。Q4 可執行 Strong A* 精確驗證；Q5 以上只跑 Beam Search 與 Batcher baseline，避免 Basic A* 的狀態空間爆炸。

重構後的 special-case runner 分成：

- `qk_common`：共用 state、swap、fingerprint 與 packed key。
- `qk_hypercube`：hypercube edges、distance、lower bound 與 path validation。
- `qk_search`：Strong A*、Beam Search、RAM fingerprint visited、SQLite disk fingerprint visited、layer-only RAM Beam。
- `qk_batcher`：Batcher baseline。
- `qk_cases`：內建 case、custom CSV parsing 與 permutation validation。
- `qk_io`：CSV escaping、progress display 與記憶體整理 helper。
- `qk_special_cases.cpp`：CLI 參數、輸出檔案與整體 runner 流程。

`custom_qk_cases.csv` 目前包含 Q4-Q15 special cases；Q15 只有一個 case，其餘 Q4-Q14 各有兩個 case。正式保存到路徑 Excel 的最新大型結果是 Q10 case1。

### Beam visited 模式

程式保留四種 visited 實作，可由命令列選擇：

| 模式 | 說明 |
|---|---|
| `exact` | 保存完整 packed state，適用 Q1-Q8 |
| `fingerprint128` | 128-bit fingerprint 保存在 RAM |
| `fingerprint128_disk` | 128-bit fingerprint 分批保存在 SQLite |
| `layer_only` | 只保留當前 Beam 與前 K 層 retained fingerprints，不保存全域 visited set |

Q9 使用 `fingerprint128_disk`。Bloom filter 只負責快速判斷「一定沒出現過」；若可能存在，仍會檢查 RAM pending set 與 SQLite primary key，因此 Bloom false positive 不會直接刪除候選。

Q10 case1 使用 `layer_only`。這個模式把 RAM 用在「近期 retained layer fingerprint window」與 per-layer candidate pool，不建立 SQLite visited database，也不記錄 candidate trace。完整 path 仍以 parent back-pointer 寫入暫存 path store，找到解後再回溯重建。

### Beam 簡化排序

目前 `qk_special_cases` 保留的是 Q8/Q9 成功使用的簡化 Beam 排序。每一層候選先依下列欄位由小到大保留前 `beam_width` 個：

```text
total_dist
max_dist
misplaced
depth
edge_id
packed_key / fingerprint / order
```

這版不再使用舊 Beam 的九欄 tie-breaker，例如 `repeat_penalty`、`improvement`、`local_improvement`、`dir_score`、`touched_max_dist`。這樣做的重點是降低 per-candidate 狀態與路徑歷史成本，讓 Q8-Q10 可以搭配 fingerprint visited、disk visited 或 layer-only window 穩定執行。排序本身仍是 heuristic Beam，不保證最短路徑；正確性由最後輸出的 path replay 驗證。

### Q9/Q10 記憶體與平行化改進

- Path 使用 parent back-pointer，只在找到解時 backtracking 重建。
- Fingerprint 可在 swap 後 O(1) 增量更新。
- Total distance、misplaced count 與 max distance 使用增量計算。
- 每層只替最後保留的 Beam candidates 建立完整 512-token state。
- Visited fingerprints 分成 16 個 SQLite shards。
- 24 workers 平行產生候選與執行分片查重。
- Candidate array 使用固定 parent/edge index，最後依原始順序提交，保持決定性搜尋順序。
- 程序在 Windows 使用 `BelowNormal` priority，降低對前景操作的影響。
- 正式 Q9 run 關閉 candidate trace，只保留深度進度、磁碟進度與最終 path。
- Q10 RAM run 使用 `layer_only`，保留前 K 層 retained fingerprints，並以 deterministic perturbation 從 candidate pool 中抽取少量候選，增加搜尋多樣性。

完整設計與驗證紀錄請參考 [Q9_BEAM_SEARCH_EVOLUTION.md](Q9_BEAM_SEARCH_EVOLUTION.md)。

### 編譯 special-case runner

直接使用 MinGW g++：

```powershell
g++ -std=c++17 -O2 -Wall -Wextra -pedantic -Iinclude `
  src\qk_common.cpp src\qk_hypercube.cpp src\qk_io.cpp `
  src\qk_search.cpp src\qk_batcher.cpp src\qk_cases.cpp `
  src\qk_special_cases.cpp -lsqlite3 -o qk_special_cases.exe
```

或透過 CMake 建置 `qk_special_cases` target。CMake 需要 `Threads` 與 `SQLite3`。

### 命令列參數

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
```

`candidate_trace_mode`：

- `0`：不記錄 candidates
- `1`：只記錄 retained Beam
- `2`：記錄所有新 candidates

### 最終 Q9 指令

```powershell
.\qk_special_cases.exe 9 9 256 4608 0 2000000 30 `
  output\q9_case1_disk_bw256_20260607_113040 `
  .\custom_qk_cases.csv 0 q9_case1 `
  fingerprint128_disk 24 1024 1000000 16

.\qk_special_cases.exe 9 9 256 4608 0 2000000 30 `
  output\q9_case2_disk_bw256_path_20260609_170556 `
  .\custom_qk_cases.csv 0 q9_case2 `
  fingerprint128_disk 24 1024 1000000 16
```

### Q10 case1 layer-only RAM 指令

Q10 case1 使用 `beam_width=512`、`layer_pool_width=4096`、`layer_visited_window=4096`、`layer_restart_max_width=512`、`layer_plateau_limit=512`、`layer_perturbation_ratio=0.10`。這輪不使用 SQLite visited database，`qk_beam_candidate_trace.csv` 維持 0 bytes。

```powershell
.\qk_special_cases.exe 10 10 512 0 0 2000000 30 `
  output\q10_ram_layer_bw512_pool4096_win4096_restart512_plateau512_perturb010_path_20260612_184936 `
  .\custom_qk_cases.csv 0 q10_case1 `
  layer_only 24 1024 1000000 16 `
  0 1 200000 0.25 24 0 1 `
  4096 4096 512 512 2 0.10
```

### Special-case 結果

所有下列 Beam 與 Batcher path 均通過 hypercube edge replay 驗證：

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
| Q9 | case2 | 1059 | 1563 | 914,621,697 | 10,559.491 | 5199 |
| Q10 | case1 | 2560 | 3308 | 8,663,587,275 | 2,660.163 | 13,998 |

Q9 case1：

- Beam width：`256`
- 解深度：`1558`
- Visited states：`909,749,195`
- 執行時間：約 `3 小時 58 分 28 秒`
- Peak RAM：約 `1.56 GB`
- 搜尋完成時 SQLite visited：約 `19.75 GB`
- Beam 比 Batcher 少約 `73.0%` swaps
- `beam_path_valid=1`

搜尋完成後 SQLite visited shards 已刪除以釋放空間；最終 path、每層進度與結果 CSV 仍保留。

Q9 case2：

- Beam width：`256`
- 解深度：`1563`
- Visited states：`914,621,698`
- 執行時間：約 `2 小時 56 分 0 秒`
- 搜尋完成時 SQLite visited：約 `21.32 GB`（約 `19.86 GiB`）
- Beam 比 Batcher 少約 `69.9%` swaps
- `beam_path_valid=1`

Q10 case1：

- Beam mode：`layer_only`
- Beam width：`512`
- Layer pool width：`4096`
- Layer visited window：`4096`
- Perturbation ratio：`0.10`
- 解深度：`3308`
- Visited states：`8,663,587,276`
- 執行時間：約 `44 分 20 秒`
- Peak RAM：本輪監控約低於 `1 GB`
- Beam 比 Batcher 少約 `76.4%` swaps
- `beam_path_valid=1`

Q10 case2 在同一輪開始後曾產生 partial depth progress，但已依需求停止；目前不列為正式完成結果。

### 成果檔

- Q4-Q7：`output/qk_custom_cases_20260605_133247/`
- Q8 case1：`output/q8_case1_trim_20260605_154219/`
- Q8 case2：`output/q8_case2_trim_20260605_154927/`
- Q9 case1：`output/q9_case1_disk_bw256_20260607_113040/`
- Q9 case2：`output/q9_case2_disk_bw256_path_20260609_170556/`
- Q10 case1：`output/q10_ram_layer_bw512_pool4096_win4096_restart512_plateau512_perturb010_path_20260612_184936/`
- 完整路徑 Excel：`output/qk_special_cases_routes.xlsx`
- 自訂 cases：`custom_qk_cases.csv`

## 依賴

- C++17 編譯器（建議 g++）
- OpenMP
- Threads
- SQLite3
- Python 3.11+
- pandas, matplotlib（用於繪圖腳本）

## 備註

- `output/` 保留正式 Q4 benchmark、Q4-Q10 special-case 結果與完整路徑 Excel。
- `legacy/` 保存舊版與過時資料，包含 Q3 輸出、舊 Q4 測試、失敗或中止的 path run、smoke tests、verification experiments、舊報告與 saved runs。
