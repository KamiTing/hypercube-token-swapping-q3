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

## 依賴

- C++17 編譯器（建議 g++）
- OpenMP
- Python 3.11+
- pandas, matplotlib（用於繪圖腳本）

## 備註

- `output/` 為正式實驗產出目錄。
- `legacy/` 保留早期單檔版本供參考。
