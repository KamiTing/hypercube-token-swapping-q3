# Q3/Q4 Hypercube Token Swapping 綜合實驗報表

產生時間：2026-06-04

本報表整理目前專案中已完成的主要實驗，包含 Q3 全排列 exact baseline、Q4 exact 驗證嘗試、Basic A* 與 Strong A* heuristic 比較、Beam Search 參數放大前後，以及四方法平行測試。

## 1. 方法定位

| 方法 | 定位 | 是否保證最短 | 備註 |
|---|---|---:|---|
| BFS | exact true table / exact verifier | 是 | Q3 可全排列；Q4 只能小量驗證 |
| Basic A* | A* + `ceil(total_hamming_distance / 2)` | 搜尋完成時可回傳最短路 | 本報表避免稱為 weak A*，統一稱 Basic A* |
| Strong A* | A* + 較強 admissible lower bound | 搜尋完成時可回傳最短路 | 與 Basic A* 在目前 Q4 測試中步數完全一致，但展開量大幅下降 |
| Beam Search | heuristic search | 否 | 快速、可調參，但會剪枝 |
| Batcher baseline | deterministic compare-exchange network | 否 | 極快但交換數遠高於搜尋法 |

Strong A* heuristic：

```text
h_strong = parity_adjust(max(
    ceil(total_hamming_distance / 2),
    max_packet_distance,
    cycle_lower_bound
))
```

其中 `cycle_lower_bound = N - number_of_cycles`，`parity_adjust` 會將 lower bound 調整到與 permutation parity 相同的步數奇偶性。

## 2. Q3 全排列結果

實驗範圍：

- 圖：Q3 hypercube
- 節點數：8
- 狀態數：`8! = 40320`
- Beam 參數：`BEAM_WIDTH = 14`, `MAX_DEPTH = 12`

| 指標 | 結果 |
|---|---:|
| BFS true table states | `40320` |
| Q3 worst-case shortest steps | `12` |
| Basic A* vs BFS match | `40320/40320` |
| Beam `14/12` vs BFS match | `40320/40320` |
| Beam failure | `0` |
| Batcher success | `40320/40320` |
| Batcher optimal vs BFS | `755/40320 = 1.872520%` |
| Batcher average swaps | `12.000000` |

Q3 結論：

- Q3 狀態空間足夠小，可以完整建立 BFS true table。
- Basic A* 和 Beam `14/12` 在 Q3 full permutation 上皆與 BFS 完全一致。
- Batcher baseline 雖然每個狀態都能完成，但只有約 `1.87%` 達到最短交換數。

## 3. Q4 exact 驗證嘗試

Q4 狀態空間為 `16!`，遠大於 Q3，因此 BFS/DFS exact 驗證只適合小量 case 或較容易 case。

### 3.1 Bidirectional BFS vs A*

資料來源：`output/q4_bfs_astar_check.csv`

| 指標 | 結果 |
|---|---:|
| 嘗試 case 數 | `15` |
| 成功 exact verified | `4` |
| 達到 exact cap | `11` |
| verified cases mismatch | `0` |
| verified shortest steps | `13, 14, 15` |

解讀：

- 在能被 exact BFS 完成的 case 中，A* 步數與 exact BFS 一致。
- 多數 full-random case 會觸及 exact cap，顯示 Q4 exact verification 主要瓶頸是狀態空間與記憶體。

### 3.2 DFS proof 嘗試

資料來源：`output/q4_astar_dfs_verify.csv`

| 指標 | 結果 |
|---|---:|
| 嘗試 case 數 | `4` |
| skipped low-step cases | `3` |
| proof time-limit cases | `1` |
| hard case state | `0x723F4C6B0D1AE985` |
| hard case A* steps | `20` |
| DFS proof depth | `19` |
| DFS expanded | `30,462,902,272` |
| DFS time | `3599.999646s` |

解讀：

- 對較難 Q4 case，即使用 DFS proof，也可能在 1 小時內無法完成「不存在更短解」的證明。
- 因此後續大規模 Q4 實驗主要定位為方法比較與經驗驗證，而不是 full exact proof。

## 4. Basic A* vs Strong A*

資料來源：`output/q4_astar_heuristic_compare.csv`

測試設定：

- case 數：`100`
- generator：full-random permutation
- 比較：Basic A* vs Strong A*

| 指標 | Basic A* | Strong A* | 比值 |
|---|---:|---:|---:|
| solved cases | `100/100` | `100/100` | - |
| same steps | `100/100` | `100/100` | - |
| avg expanded | `37103.03` | `3401.39` | Basic / Strong = `10.91x` |
| avg sec | `0.236453` | `0.024930` | Basic / Strong = `9.48x` |

結論：

- Strong A* 在此 100 筆測試中與 Basic A* 回傳完全相同的步數。
- Strong heuristic 將平均 expanded nodes 降到約 Basic A* 的 `1/10.91`。
- Strong A* 的平均時間約比 Basic A* 快 `9.48x`。

## 5. Q4 full-random benchmark：Beam 參數放大前後

### 5.1 5000 筆：Beam `32/24`

資料來源：`output/saved_runs/q4_full_random_5000_basic_strong_beam32_depth24_seed42_20260604_110619/`

設定：

```text
samples = 5000
beam_width = 32
max_depth = 24
seed = 42
astar_cap = 0
mode = sequential_methods
```

| 方法 | success rate | avg steps/swaps | avg sec | avg expanded |
|---|---:|---:|---:|---:|
| Basic A* | `100.00%` | `17.1638` | `0.706867` | `88782.4556` |
| Strong A* | `100.00%` | `17.1638` | `0.030566` | `4384.9472` |
| Beam `32/24` | `99.98%` | `17.3651` | `0.001365` | N/A |
| Batcher | `100.00%` | `39.9234` | `0.00000097` | N/A |

觀察：

- Beam `32/24` 在 5000 筆中有 `1` 筆 failure。
- Beam 成功 case 的平均步數比 Basic/Strong A* 高約 `0.2013`。
- Strong A* 與 Basic A* 步數 `5000/5000` 一致。

### 5.2 10000 筆：Beam `128/30`

資料來源：`output/q4_random_benchmark_parallel_10000.csv`

設定：

```text
samples = 10000
beam_width = 128
max_depth = 30
seed = 42
astar_cap = 0
mode = parallel_methods
```

總 wall time：

- `9701s`
- 約 `161.7` 分鐘

| 方法 | success rate | avg steps/swaps | avg sec | avg expanded |
|---|---:|---:|---:|---:|
| Basic A* | `100.00%` | `17.1852` | `0.737449` | `87167.9328` |
| Strong A* | `100.00%` | `17.1852` | `0.039652` | `4287.0348` |
| Beam `128/30` | `100.00%` | `17.2270` | `0.007956` | `48680.0116` |
| Batcher | `100.00%` | `39.9200` | `0.00000156` | N/A |

額外指標：

| 指標 | 結果 |
|---|---:|
| Basic A* / Strong A* same steps | `10000/10000` |
| Strong expanded <= Basic expanded | `8747/10000` |
| avg case wall sec | `0.969894` |
| Basic A* avg gap vs Batcher | `-22.7348` |
| Strong A* avg gap vs Batcher | `-22.7348` |
| Beam avg gap vs Batcher | `-22.6930` |

觀察：

- Beam 從 `32/24` 放大到 `128/30` 後，failure 從 `1/5000` 降到 `0/10000`。
- Beam 平均步數也更接近 Basic/Strong A*：從差約 `0.2013` 降到差約 `0.0418`。
- 代價是 Beam 平均時間從 `0.001365s` 提高到 `0.007956s`，但仍遠快於 Basic A* 和 Strong A*。

## 6. 四方法平行模式測試

目的：

- 測試同一個 case 內，Basic A*、Strong A*、Beam、Batcher 各用一個 thread 是否能加速。
- 不是多 case 平行；因此比 16 worker 方案更安全。

資料來源：

- `output/q4_random_benchmark_sequential_500.csv`
- `output/q4_random_benchmark_parallel_500.csv`

設定：

```text
samples = 500
beam_width = 128
max_depth = 30
seed = 42
astar_cap = 0
```

| 模式 | avg case wall sec | total case wall sec | 相對加速 |
|---|---:|---:|---:|
| sequential_methods | `1.119237` | `559.62s` | baseline |
| parallel_methods | `1.105012` | `552.51s` | `1.013x` |

各方法平均耗時：

| 方法 | sequential | parallel |
|---|---:|---:|
| Basic A* | `0.847270s` | `0.861333s` |
| Strong A* | `0.035089s` | `0.043220s` |
| Beam | `0.004388s` | `0.006791s` |
| Batcher | `0.000001s` | `0.000002s` |

結論：

- 四方法平行的總 wall time 只改善約 `1.3%`。
- Basic A* 是主要瓶頸；平行後 Basic A* 自身反而略慢，推測原因是 CPU/cache/記憶體競爭。
- 平行模式可以保留，但不是主要加速來源。

## 7. 綜合比較與結論

### 7.1 Strong A* 的價值

Strong A* 是目前最重要的改進：

- 在 10000 筆 full-random benchmark 中，Strong A* 與 Basic A* 步數 `10000/10000` 一致。
- 平均 expanded nodes 從 `87167.9328` 降到 `4287.0348`，約 `20.33x` 減少。
- 平均耗時從 `0.737449s` 降到 `0.039652s`，約 `18.60x` 加速。

因此若需要高品質解，Strong A* 比 Basic A* 更適合作為主要 A* 版本；Basic A* 可保留作為對照組。

### 7.2 Beam Search 的定位

Beam Search 不是 exact solver，但在 Q4 full-random 大樣本中非常實用：

- `Beam 32/24`：5000 筆中 `1` 筆 failure。
- `Beam 128/30`：10000 筆中 `0` 筆 failure。
- `Beam 128/30` 平均步數 `17.2270`，只比 Basic/Strong A* 的 `17.1852` 高 `0.0418`。
- 平均耗時 `0.007956s`，仍比 Strong A* 快約 `4.98x`。

因此 Beam `128/30` 可作為大規模 heuristic baseline。

### 7.3 Batcher baseline 的定位

Batcher baseline 極快、穩定、deterministic，但不追求最短交換數：

- Q4 10000 筆平均 swaps：`39.9200`
- Strong A* 平均 steps：`17.1852`
- Batcher 平均交換數約為 Strong A* 的 `2.32x`

因此 Batcher 適合作為 deterministic routing baseline，而不是 shortest-path 方法。

### 7.4 平行化策略

目前測試的四方法平行只有約 `1.013x` 加速，原因是：

- Basic A* dominates runtime。
- Strong A*、Beam、Batcher 本來就相對快。
- 同 case 內平行會增加 CPU/cache/記憶體競爭。

若要追求真正加速，更有效的方向可能是：

- 以 Strong A* 取代 Basic A* 作為主要高品質搜尋。
- Basic A* 僅保留在抽樣驗證或對照實驗中。
- 對 Beam 做參數掃描，找出比 `128/30` 更低成本但仍 0 fail 的設定。

## 8. 建議用於報告的最終敘述

建議在正式報告中採用以下定位：

> 本研究在 Q3 上使用 BFS 建立完整 true table，確認 Basic A* 與 Beam Search 在全排列狀態上皆可達到最短交換步數。進入 Q4 後，由於 `16!` 狀態空間過大，BFS/DFS exact verification 僅能用於小量 case 或較容易 case；對較難 case，exact proof 會受到時間與記憶體限制。因此 Q4 實驗以 full-random benchmark 比較不同方法的解品質與執行成本。Strong A* 在 10000 筆 full-random case 中與 Basic A* 步數完全一致，同時將平均展開量降低約 20 倍；Beam Search 在放大至 `beam_width=128, max_depth=30` 後達到 10000 筆 0 failure，平均步數僅略高於 A*，但速度更快；Batcher baseline 則提供極快但非最短的 deterministic 對照。

## 9. 主要輸出檔案

- `output/q4_random_benchmark_parallel_10000.csv`
- `output/q4_random_summary.csv`
- `output/q4_steps_hist_compare.png`
- `output/q4_time_boxplot.png`
- `output/q4_gap_vs_batcher_hist.png`
- `output/saved_runs/q4_full_random_5000_basic_strong_beam32_depth24_seed42_20260604_110619/`

## 10. 圖表

目前圖表對應最新 10000 筆 full-random benchmark：

- ![Q4 step distribution](q4_steps_hist_compare.png)
- ![Q4 runtime boxplot](q4_time_boxplot.png)
- ![Q4 gap to Batcher](q4_gap_vs_batcher_hist.png)
