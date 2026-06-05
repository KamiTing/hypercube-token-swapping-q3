# Q3 Hypercube Token Swapping

這個專案實作並驗證以下問題：

- **Minimum Token Swapping on Q3 Hypercube**
- **Zero-buffer edge-swap routing shortest path**

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

## Q4 完全隨機測試（本分支重點）

此分支（`codex/q4-random-test`）新增 Q4 (`DIM=4`) 的 full-random benchmark。每個 case 由 Fisher-Yates shuffle 均勻抽樣自全部 `16!` token placements，不再使用從目標狀態 random walk 的 `scramble_steps`。

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
.\q4_random_benchmark_run.exe 10000 128 24 42 0 0 1 output\q4_path_selected_10000_basic_no_path_YYYYMMDD_HHMMSS
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
- `beam_width=128, max_depth=24, seed=42, astar_cap=0, parallel_methods=0, record_paths=1`
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

## 依賴

- C++17 編譯器（建議 g++）
- OpenMP
- Python 3.11+
- pandas, matplotlib（用於繪圖腳本）

## 備註

- `output/` 只保留目前正式 Q4 大型測試結果。
- `legacy/` 保存舊版與過時資料，包含 Q3 輸出、舊 Q4 測試、失敗或中止的 path run、smoke tests、verification experiments、舊報告與 saved runs。
