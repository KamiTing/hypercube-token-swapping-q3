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

## Q4 隨機測試（本分支重點）

此分支（`codex/q4-random-test`）新增 Q4 (`DIM=4`) 的隨機 benchmark：

- 程式：`src/q4_random_benchmark.cpp`
- 可視化：`plot_q4_random.py`

### 執行 Q4 benchmark

```powershell
g++ -std=c++17 -O2 src/q4_random_benchmark.cpp -o q4_random_benchmark_run.exe
.\q4_random_benchmark_run.exe 5000 20 32 24 42
```

參數順序：

- `samples scramble_steps beam_width max_depth seed`

### 產生 Q4 圖表

```powershell
.\.venv\Scripts\python.exe plot_q4_random.py
```

### Q4 輸出檔案

- `output/q4_random_benchmark.csv`
- `output/q4_random_summary.csv`
- `output/q4_steps_hist_compare.png`
- `output/q4_time_boxplot.png`
- `output/q4_gap_vs_batcher_hist.png`

### 最近一次大測試（5000 samples）

- `scramble_steps=20, beam_width=32, max_depth=24, seed=42`
- `A* failures = 242/5000`（success rate `95.16%`）
- `Beam failures = 0/5000`（success rate `100%`）
- `Batcher failures = 0/5000`
- `A* <= Batcher swaps = 4758/5000`
- `Beam <= Batcher swaps = 5000/5000`
- `avg_astar_steps = 12.7209`
- `avg_beam_steps = 12.9256`
- `avg_batcher_swaps = 38.6944`
- `avg_astar_sec = 0.008878`
- `avg_beam_sec = 0.001056`
- `avg_batcher_sec = 0.00000086`

### 測試數據分析

1. 穩定性：Beam 最穩定。  
`Beam failures = 0/5000`，代表在這組 Q4 隨機測試與參數下，Beam 沒有出現搜尋失敗；A* 則有 `242/5000` 失敗。

2. 路徑品質：A* 成功時步數略優於 Beam。  
`avg_astar_steps (12.72)` 小於 `avg_beam_steps (12.93)`，表示 A* 在成功解出的樣本上通常能找到更短的交換序列。

3. 與 Batcher baseline 對比：兩種搜尋法都大幅優於固定網路。  
`avg_batcher_swaps = 38.69`，而 A*/Beam 的平均步數約 13 左右，顯示 heuristic search 對交換次數有明顯優勢。

4. 工程實用性：Beam 的吞吐與可用性最佳。  
在大樣本下，Beam 同時具備「高成功率」與「低單筆耗時」，是目前 Q4 隨機測試的主要建議方法；A* 適合作為高品質解的輔助比較。

5. 平均耗時分析：  
在 `5000` 筆測試中，平均耗時為 `A* = 0.008878s`、`Beam = 0.001056s`、`Batcher = 0.00000086s`。  
也就是 Beam 約比 A* 快 `8.4x`，而 Batcher 仍因固定網路特性最快，但不追求最短步數。

6. 研究定位：Batcher 適合當 deterministic baseline。  
Batcher 幾乎不失敗且速度極快，但不是最短路方法，主要用途是提供固定規則參考下限。

## 依賴

- C++17 編譯器（建議 g++）
- OpenMP
- Python 3.11+
- pandas, matplotlib（用於繪圖腳本）

## 備註

- `output/` 為正式實驗產出目錄。
- `legacy/` 保留早期單檔版本供參考。
