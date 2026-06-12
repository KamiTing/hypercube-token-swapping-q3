# Q5~Q8

Q5~Q8 的特殊 case 已經不再用 Basic A* 當主要測試方法。原因是 Q5 以上狀態空間快速膨脹，Basic A* 與完整 exact search 的記憶體和時間成本都不適合拿來跑所有特殊 case。現在保留 `strong_lb` 作為 lower bound 參考，實際求路徑主要靠 Beam Search，Batcher route 則作為 deterministic baseline。

Q5~Q8 沿用 Beam Search 的基本骨架：逐層展開目前 beam 內的狀態、對候選排序、每層保留前 `beam_width` 個。這個骨架本身不是這次相對舊 Beam 的主要差異。

Q5~Q8 相對舊做法的重點是：

1. 不跑 Basic A*，只保留 Beam Search 與 Batcher baseline。
2. Beam candidate ranking 從舊的九欄 tie-breaker 改成較簡化的距離排序。
3. visited 保持在記憶體內，避免同一狀態重複展開。
4. 最終輸出的 Beam path 會用 replay 驗證合法性與終點。

Q5~Q8 的 visited 使用記憶體內資料結構，以完整 packed state key 查重。因為最多到 Q8，這個做法仍可接受，也讓查重語意保持直觀。

目前 `qk_special_cases` 使用的是簡化 Beam 排序。每一層新候選會依下列 tuple 由小到大排序，保留前 `beam_width` 個：

```text
total_dist
max_dist
misplaced
depth
edge_id
packed_key / fingerprint / order
```

各欄位含義如下：

| 欄位 | 含義 |
|---|---|
| `total_dist` | 所有 token 到目標位置的 Hamming distance 總和，主導搜尋往整體更接近 identity 的方向前進。 |
| `max_dist` | 單一 token 的最大距離，優先降低最遠 token 的距離。 |
| `misplaced` | 還沒在正確位置上的 token 數量。 |
| `depth` | 目前路徑長度，作為 tie-breaker。 |
| `edge_id` | 固定 edge 順序，讓結果可重現。 |
| `packed_key / fingerprint / order` | 最後 tie-breaker，用來保持 deterministic ordering。Q5~Q8 若有完整 state key，優先用 `packed_key`；否則用 fingerprint 與生成順序補足穩定排序。 |

這版不再使用舊 Beam 的九欄排序，例如 `repeat_penalty`、`improvement`、`local_improvement`、`dir_score`、`touched_max_dist`。舊規則保存較多候選歷史資訊，單一候選比較重；新規則把排序集中在全域距離指標與 deterministic tie-breaker，讓 Q5~Q8 的候選選擇更容易維護並保持可重現。

目前保留的正式結果如下，所有 Beam path 與 Batcher path 都通過 replay 驗證：

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

重點結論是：Q5~Q8 的搜尋規則仍是通用 Beam solver，沒有針對特定 permutation 寫死結構；它只是把 exact A* 搜尋改成 Beam candidate ranking，並用完整狀態 visited set 避免重複展開。

# Q9

Q9 的排序規則和 Q5~Q8 一樣，仍然使用同一套簡化 Beam tuple：

```text
total_dist
max_dist
misplaced
depth
edge_id
packed_key / fingerprint / order
```

真正的差異不在 ranking，而在記憶體模型。Q9 有 512 個 token，完整 packed state visited 已經不適合放在 RAM 中；因此 Q9 改用 `fingerprint128_disk`：

1. 每個狀態用 128-bit fingerprint 表示。
2. swap 後 fingerprint 用 O(1) 增量更新，不重新掃整個 state。
3. visited fingerprints 分成多個 SQLite shard 存在磁碟。
4. Bloom filter 只用來快速判斷「一定沒出現過」。
5. 若 Bloom filter 顯示可能存在，仍會查 RAM pending set 與 SQLite primary key。
6. pending fingerprints 批次寫入 SQLite，降低單筆 I/O 成本。
7. 搜尋完成或遇到解時 flush pending fingerprints。

這個做法的目標是把 visited set 從 RAM 轉移到 disk，同時盡量保持原本 Beam 搜尋邏輯。也就是說：候選還是同樣從目前 beam 展開、同樣用 tuple 排序、同樣保留前 `beam_width` 個；改變的是查重資料結構與候選產生方式。

Q9 也加入了平行化與低記憶體 path 記錄：

| 改動 | 用途 |
|---|---|
| 平行候選產生 | 多 worker 對當前 beam 的 parent states 展開候選。 |
| 分片查重 | 依 fingerprint hash 分配到 SQLite shard，可平行查重與 flush。 |
| 增量距離計算 | `total_dist`、`misplaced`、`max_dist` 不再每次完整掃描 512 tokens。 |
| parent back-pointer | 不為每個候選保存完整 path，只在 retained candidate 建立 parent link，解出後 backtracking。 |
| 關閉 candidate trace | 正式 Q9 run 不記錄所有候選，避免 trace 檔案爆量。 |
| deterministic order | 候選仍保留 `order` 作最後 tie-breaker，避免平行化改變結果順序。 |

Q9 的正式執行參數是：

```powershell
.\qk_special_cases.exe 9 9 256 4608 0 2000000 30 `
  <output_dir> `
  .\custom_qk_cases.csv 0 <q9_case_name> `
  fingerprint128_disk 24 1024 1000000 16
```

參數含義：

| 參數 | 值 | 說明 |
|---|---:|---|
| `beam_width` | 256 | 每層最多保留 256 個候選。 |
| `max_depth` | 4608 | 搜尋深度上限，只是 cutoff，不是排序規則。 |
| `exact_max_dim` | 0 | Q9 不跑 Strong A* exact search。 |
| `candidate_trace_mode` | 0 | 不輸出候選 trace。 |
| `beam_visited_mode` | `fingerprint128_disk` | 使用 SQLite disk visited。 |
| `worker_threads` | 24 | 平行候選產生與分片查重。 |
| `disk_bloom_mb` | 1024 | Bloom filter 記憶體大小。 |
| `disk_batch_size` | 1,000,000 | pending fingerprints 批次 flush 大小。 |
| `disk_shards` | 16 | SQLite visited shards 數量。 |

Q9 目前完成兩個正式 case：

| Dimension | Case | Strong LB | Beam steps | Expanded | Beam sec | Batcher swaps | Beam path valid |
|---|---|---:|---:|---:|---:|---:|---:|
| Q9 | case1 | 1152 | 1558 | 909,749,194 | 14,307.567 | 5774 | 1 |
| Q9 | case2 | 1059 | 1563 | 914,621,697 | 10,559.491 | 5199 | 1 |

Q9 的重要限制是：這仍然是 heuristic Beam Search，不保證最短路徑。`strong_lb` 只是 lower bound，不是 exact proof。正確性目前以兩件事確認：

1. Beam 輸出的每一步 swap 都是合法 hypercube edge。
2. 完整 path replay 後確實抵達 identity permutation。

因此 Q9 的主要改進可以總結為：搜尋規則本身維持通用 Beam，不針對特定解寫死；為了讓 Q9 能在目前硬體上完成，將 visited 從 RAM 改成 128-bit fingerprint + SQLite shards，並加入平行候選產生、增量評分、back-pointer path reconstruction 和 trace 關閉。
