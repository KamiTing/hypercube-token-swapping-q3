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
- **Basic A\***：精確搜尋，使用 `ceil(total_hamming_distance / 2)`
- **Strong A\***：精確搜尋，使用更強的 admissible lower bound
- **Beam Search**：heuristic 搜尋
- **Batcher baseline**：固定 compare-exchange sorting-network

Basic A\* heuristic：

`h(state) = ceil(total_hamming_distance(state) / 2)`

因為一次 swap 最多讓兩個 token 各靠近目標一步，所以總距離最多下降 2，故此 heuristic 不高估。

Strong A\* heuristic：

`h_strong = parity_adjust(max(ceil(total_hamming_distance / 2), max_packet_distance, cycle_lower_bound))`

其中 `cycle_lower_bound = N - number_of_cycles`，而 `parity_adjust` 會將 lower bound 調整到與目前 permutation parity 相同的步數奇偶性。這些項目皆為 lower bound，因此 Strong A\* 仍可保證 optimal。

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
│   ├── q3_routing_paths.csv
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
- `output/q3_routing_paths.csv`

`q3_routing_paths.csv` 是逐 permutation 的 detailed routing path 紀錄。每列包含初始 state、BFS/Basic A*/Strong A*/Beam/Batcher 的步數，以及實際 edge-swap sequence。例如 `01 13 37` 代表依序交換 `(0,1)`, `(1,3)`, `(3,7)`。

### 3) 產生統計表與圖

```powershell
.\.venv\Scripts\python.exe plot_distribution.py
```

會產生：

- `output/step_distribution_summary.csv`
- `output/bfs_step_distribution.png`
- `output/method_step_distribution_compare.png`

## 主要結果摘要（Q3 全測）

- Basic A\* 與 BFS 全部一致（40320/40320）。
- Strong A\* 與 BFS 全部一致（40320/40320）。
- Beam（14/12）與 BFS 全部一致（40320/40320）。
- Batcher baseline 可解但非最短路，最優率約 `1.87%`。
- BFS / Basic A\* / Strong A\* / Beam 平均最短步數為 `6.606349`。
- Batcher baseline 平均 swap 數為 `12.000000`。

## 依賴

- C++17 編譯器（建議 g++）
- OpenMP
- Python 3.11+
- pandas, matplotlib（用於繪圖腳本）

## 備註

- `output/` 為正式實驗產出目錄。
- `legacy/` 保留早期單檔版本供參考。
