# 從舊版 Beam Search 到完成 Q9 的改進紀錄

## 1. 概述

本文件說明本專案如何從原本會在 Q8、Q9 遭遇記憶體瓶頸的 Beam Search，逐步改進成能完成 Q9 special case 1 的版本。

這次改進的核心原則是：

1. 保留原本的 Beam Search 評分、排序、bound 與候選選擇邏輯。
2. 不以近似去重換取較低記憶體，避免因 Bloom filter false positive 誤刪候選。
3. 將歷史 visited set 從 RAM 移到磁碟。
4. 使用 back-pointer、增量評分與延遲 materialization 降低暫存記憶體。
5. 在維持決定性搜尋順序的前提下，平行化候選生成與磁碟查重。

最終 Q9 case1 使用 Beam width 256，在約 3 小時 58 分內找到一條 1558-step 的有效路徑。

---

## 2. 舊方法的主要瓶頸

Q9 hypercube 有：

- `2^9 = 512` 個節點。
- `9 * 2^(9-1) = 2304` 條無向邊。

Beam width 為 256 時，每一層最多產生：

```text
256 * 2304 = 589,824 candidates
```

原本方法的記憶體問題不只來自當前 Beam，主要還包括：

### 2.1 所有歷史 visited 都存放在 RAM

即使每個狀態只保留 128-bit fingerprint，C++ `unordered_set` 仍會附帶：

- hash bucket
- node allocation
- pointer
- alignment
- allocator metadata

因此每個 fingerprint 的實際記憶體成本遠高於 16 bytes。

早期 Q9 RAM fingerprint 測試在約 4.3 億個 visited states 時，private memory 已接近 24 GB，無法繼續支撐完整搜尋。

### 2.2 Path 隨候選重複複製

若每個候選都攜帶完整交換路徑，空間會近似：

```text
candidate_count * current_depth
```

搜尋深度增加時，path 的複製與記憶體配置成本會持續上升。

### 2.3 為所有候選建立完整 state

Q9 每個 permutation 包含 512 個 token。若每層將約 59 萬個候選全部 materialize 成完整 state，會產生龐大的暫存記憶體與複製成本。

### 2.4 重複計算整個 heuristic

交換只會改變兩個位置，但舊做法若對每個候選重新掃描全部 512 個位置，會浪費大量 CPU 時間。

---

## 3. 先修正搜尋與輸出的正確性

在處理效能前，先補上結果正確性驗證。

### 3.1 Batcher identity early stop

Batcher baseline 在輸入已經是 identity permutation 時，應立即回傳：

```text
success = true
swaps = 0
```

避免 identity case 仍執行 compare-exchange 並產生不必要 swaps。

### 3.2 Path replay validation

每條 Beam 與 Batcher 路徑完成後都重新 replay，檢查：

1. Swap 的兩端索引沒有超出範圍。
2. 兩個位置在 hypercube 上的 Hamming distance 必須為 1。
3. 依序執行全部 swaps 後，結果必須是 identity permutation。

因此 CSV 中的 `beam_path_valid=1` 不只是搜尋器自行宣告成功，而是經過獨立 replay 驗證。

### 3.3 每個 case 立即寫入結果

每完成一個 case 就立即更新：

- `qk_special_cases.csv`
- `qk_case_progress.csv`
- `qk_beam_depth_progress.csv`

即使後續 case 中止，已完成的結果與 path 仍會保留。

---

## 4. Path 改用 back-pointer

舊版若讓每個候選保存完整 path，會重複儲存大量共同前綴。

新版改為：

```cpp
struct BeamPathNode {
    int parent;
    SwapStep swap;
};
```

每個保留的 Beam 節點只記錄：

- 前一個 path node 的索引。
- 本層使用的 swap。

找到解時，再從最後一個節點沿 `parent` 反向追蹤，最後 reverse 得到完整 path。

空間由近似：

```text
候選數 * path depth
```

降低為：

```text
保留節點數 * (parent index + one swap)
```

這是 backtracking 思想在 path 儲存上的應用。它不會改變搜尋結果，只改變如何保存與重建路徑。

---

## 5. 加入可選的 Fingerprint128

為了避免保存完整 permutation 作為 visited key，新增 128-bit fingerprint：

```cpp
struct Fingerprint128 {
    uint64_t low;
    uint64_t high;
};
```

目前保留三種 Beam visited 模式：

| 模式 | 說明 |
|---|---|
| `exact` | 保存完整 packed state，為原本的精確模式 |
| `fingerprint128` | 將 128-bit fingerprint 保存在 RAM |
| `fingerprint128_disk` | 將 128-bit fingerprint 分批保存在 SQLite |

舊模式沒有被刪除，因此 Q8 以下仍可選擇原本的 exact packed visited。

### 5.1 O(1) fingerprint 更新

Fingerprint 是所有 `(position, token)` component 的 XOR：

```text
fingerprint(state) =
    component(0, state[0]) XOR
    component(1, state[1]) XOR
    ...
```

交換位置 `u`、`v` 時，只需要：

1. XOR 移除舊的 `(u, token_u)` 與 `(v, token_v)`。
2. XOR 加入新的 `(u, token_v)` 與 `(v, token_u)`。

不需要重新掃描 512 個位置，因此 fingerprint 更新為 O(1)。

### 5.2 Q8 等價性驗證

Q8 case1 使用 Fingerprint128 重跑後，與舊 exact-packed 結果比較：

- steps 相同
- expanded states 相同
- visited states 相同
- 完整 path 相同
- path replay 有效

但 RAM `unordered_set` 的額外成本仍然太高，因此 Q9 必須進一步將 visited 移到磁碟。

### 5.3 Fingerprint collision

Fingerprint128 理論上仍可能發生 hash collision，但機率極低。

除了這個 128-bit fingerprint 本身的理論碰撞風險外，磁碟模式沒有額外使用會漏掉候選的近似去重。

---

## 6. Visited set 改為磁碟分批儲存

新版使用 SQLite 儲存所有已出現的 Fingerprint128：

```sql
CREATE TABLE seen (
    fingerprint BLOB PRIMARY KEY
) WITHOUT ROWID;
```

Fingerprint 以 16-byte BLOB 作為 primary key。

### 6.1 查重流程

新候選的查重順序如下：

1. 使用 Bloom filter 判斷 fingerprint 是否「可能已存在」。
2. 若 Bloom filter 表示一定不存在，直接接受為新候選。
3. 若可能存在，先查 RAM 中尚未 flush 的 pending set。
4. 再查 SQLite database。
5. 新 fingerprint 加入 pending batch。
6. pending 達到設定數量後，以 transaction 一次寫入磁碟。

### 6.2 Bloom filter 不會誤刪候選

Bloom filter 只作為快速 negative filter：

- Bloom 回答「不存在」：可以確定是新 fingerprint。
- Bloom 回答「可能存在」：仍會查 pending set 與 SQLite。

因此 Bloom false positive 只會多做一次資料庫查詢，不會直接剔除候選。

### 6.3 Batch transaction

逐筆寫 SQLite 會產生過多 transaction overhead，因此 fingerprint 先累積在 RAM：

```text
pending set
pending insertion order
```

達到 `disk_batch_size` 後，再一次：

```sql
BEGIN IMMEDIATE;
INSERT ...
INSERT ...
COMMIT;
```

最終 Q9 設定的總 batch size 為 1,000,000。

### 6.4 SQLite 效能設定

搜尋使用的 SQLite 設定包括：

```sql
PRAGMA journal_mode=OFF;
PRAGMA synchronous=OFF;
PRAGMA locking_mode=EXCLUSIVE;
PRAGMA temp_store=MEMORY;
```

這些資料庫是一次性搜尋 visited index，不是需要長期交易復原的正式資料庫，因此優先考慮吞吐量。

---

## 7. SQLite 分成 16 個 shards

單一 SQLite database 會讓全部查重與寫入集中在同一 connection，難以使用多核心。

因此 visited database 被拆成：

```text
part00.sqlite3
part01.sqlite3
...
part15.sqlite3
```

每個 fingerprint 透過固定 hash 決定 shard：

```text
shard = hash(fingerprint) mod shard_count
```

每個 shard 都有獨立的：

- SQLite connection
- prepared statements
- Bloom filter
- pending set
- pending insertion batch

不同 shard 可以平行查詢與 flush。

### 7.1 Shard 效能測試

Q9 前 100 層壓力測試中：

| 設定 | 平均 visited 處理時間 |
|---|---:|
| 單一 SQLite | 約 462 ms/層 |
| 8 shards | 約 102 ms/層 |
| 16 shards | 約 60 ms/層 |

因此最終版本將預設 shard 數調為 16，配合使用者的 16-core / 32-thread CPU。

---

## 8. 候選生成改為多執行緒

最終設定使用 24 個 worker threads。

候選生成依 Beam parent 平行處理：

```text
worker 1 -> parent 0
worker 2 -> parent 1
...
```

每個 candidate 寫入預先決定的位置：

```text
candidate_index = parent_index * edge_count + edge_id
```

因此即使 threads 的完成順序不同，輸出的 candidate array 順序仍與原本單執行緒一致。

### 8.1 查重也以 shard 平行執行

候選先依 fingerprint 分配到對應 shard。

各 shard 可平行執行：

- Bloom check
- pending lookup
- SQLite lookup
- pending insert

完成後再回到單一固定順序掃描 `is_new` 結果。

### 8.2 保持決定性搜尋順序

為了避免多執行緒改變 Beam 結果：

1. Candidate array 使用固定 index。
2. 每個 shard 內的 indices 保持原始遞增順序。
3. `candidate_order` 只在最後依原始 parent/edge 順序掃描時分配。
4. Top-k comparator 沿用原本排序。

因此 worker 完成順序不會影響候選排序或 Beam 保留結果。

---

## 9. 候選使用延遲 materialization

Q9 每個完整 state 有 512 個 token。

新版產生候選時不立即複製完整 state，只保存：

```text
parent_index
edge_id
fingerprint
total_dist
misplaced
max_dist
```

只有通過：

1. visited 去重
2. top-k selection

並真正進入下一層 Beam 的候選，才會：

1. 複製 parent state。
2. 執行該 edge 的 swap。

每層約 59 萬個候選中，最終只 materialize Beam width 256 個 state。

這大幅降低：

- state copy
- heap allocation
- temporary memory
- memory bandwidth

---

## 10. Heuristic 改為增量計算

每次 swap 只會改變兩個位置，因此不需要重新掃描整個 permutation。

### 10.1 Total Hamming distance

只更新位置 `u`、`v`：

```text
new_total =
    old_total
    - old_distance(u)
    - old_distance(v)
    + new_distance(u)
    + new_distance(v)
```

### 10.2 Misplaced count

同樣只扣除交換前兩個位置的 misplaced contribution，再加入交換後 contribution。

### 10.3 Max distance

每個 Beam item 保存 distance histogram：

```text
histogram[d] = 距離為 d 的 token 數量
```

交換時只更新四個 histogram entries，再從 histogram 找最大非零距離。

因此候選評分不需要反覆掃描 512 個 token。

---

## 11. 保留原本的 Beam 排序與 bound

這次改進沒有增加新的 pruning bound。

候選仍依原本順序比較：

1. `total_dist`
2. `max_dist`
3. `misplaced`
4. `depth`
5. `edge_id`
6. packed key 或 fingerprint
7. `candidate_order`

磁碟化、多執行緒與延遲 materialization 只改變候選的產生、保存及查重方式，不改變 top-k 的評分原則。

### 11.1 最終解層的順序修正

平行版本最初曾將解出 identity 之後、同層較後面的候選也加入 visited，造成 `beam_states` 數量與舊版不同。

修正方式是：

1. 先依原始 candidate index 找到最早的 goal candidate。
2. 該層只處理到 goal candidate 為止。
3. Goal 後面的候選不進行 visited insert。

修正後 steps、expanded、states 與 path 都能和舊方法完全一致。

---

## 12. 搜尋語意等價性驗證

### 12.1 Q5 完整搜尋

磁碟分片版本與 RAM fingerprint 版本比較：

- steps 相同
- expanded 相同
- visited states 相同
- 完整 path 相同
- path replay 有效

### 12.2 Q9 前 300 層

將磁碟平行版本與舊 RAM fingerprint run 的前 300 層逐列比較：

- depth
- expanded
- layer candidates
- retained
- best total distance
- best misplaced
- best max distance

結果：

```text
300 rows
7 fields per row
differences = 0
```

這證明磁碟查重與多執行緒沒有改變前 300 層的搜尋軌跡。

### 12.3 Beam width 的差異

早期 Q9 測試使用 Beam width 64。

最終成功 run 使用 Beam width 256，因此每層可保留四倍候選。這確實會改變 Beam Search 的搜尋範圍，但這是明確調高 `beam_width` 的結果，不是磁碟 visited 或多執行緒造成的隱性邏輯變更。

---

## 13. 記憶體與監控改進

新增以下監控輸出：

### 13.1 Beam depth progress

`qk_beam_depth_progress.csv` 記錄：

- depth
- expanded
- layer candidates
- retained
- best total distance
- best misplaced
- best max distance
- elapsed time

### 13.2 Disk progress

`qk_beam_disk_progress.csv` 記錄：

- visited states
- pending states
- disk bytes
- worker threads
- generation time
- visited time
- selection time

### 13.3 Process memory trim

每個 case 結束後，在 Windows 執行：

```cpp
_heapmin();
SetProcessWorkingSetSize(...);
```

讓 allocator 與 working set 儘量將不再使用的頁面交還系統。

### 13.4 降低程序優先權

磁碟模式會將 process priority 設為：

```text
BelowNormal
```

配合 24 workers，盡量使用多核心，同時保留系統互動性。

### 13.5 關閉大型 candidate trace

最終 Q9 run 的 `candidate_trace_mode=0`。

如果保存每個新候選的完整 512-token state 與 path，I/O 量會極大，而且會反過來拖慢搜尋。因此正式 run 只保留：

- 每層 progress
- 每 case summary
- 最終完整 Beam path

---

## 14. Q8 與 Q9 最終結果

### 14.1 Q8 special cases

| Case | Beam steps | Expanded | 執行時間 | Path valid | Batcher |
|---|---:|---:|---:|---:|---:|
| Q8 case1 | 686 | 88,551,994 | 207.72 秒 | 1 | 2318 |
| Q8 case2 | 562 | 72,584,496 | 153.83 秒 | 1 | 2102 |

### 14.2 Q9 case1 最終設定

```text
Dimension:          Q9
Beam width:         256
Max depth:          4608
Exact A*:           disabled
Visited mode:       fingerprint128_disk
Worker threads:     24
Bloom memory:       1024 MB
Disk batch size:    1,000,000
SQLite shards:      16
Candidate trace:    disabled
Process priority:   BelowNormal
```

### 14.3 Q9 case1 最終成果

| 項目 | 結果 |
|---|---:|
| Beam status | solved |
| Beam steps | 1558 |
| Strong lower bound | 1152 |
| Gap | 406 |
| Expanded states | 909,749,194 |
| Visited states | 909,749,195 |
| 執行時間 | 14,307.57 秒 |
| 約略時間 | 3 小時 58 分 28 秒 |
| Peak RAM | 約 1.56 GB |
| 搜尋資料庫 | 約 19.75 GB |
| Path valid | 1 |
| Batcher swaps | 5774 |

Beam 相較 Batcher 減少的 swaps 比例約為：

```text
(5774 - 1558) / 5774 = 73.0%
```

完整 1558-step path 經 replay 後確實回到 identity permutation。

---

## 15. 哪些資料會立即釋放

每層候選陣列只保留到該層處理結束。

未進入 top-k Beam 的候選：

- 不保存完整 state。
- 不建立 path node。
- 在該層結束後隨 candidate buffer 重用或釋放。

真正長期保存的是：

1. SQLite 中的歷史 Fingerprint128。
2. 尚未 flush 的 pending fingerprint batch。
3. 被保留 Beam 節點的 state。
4. 每個被保留節點的一個 path back-pointer。

因此 RAM 使用量不再隨全部 visited states 線性增長；visited 的主要線性成長被轉移到磁碟。

---

## 16. 最終架構

```text
Current Beam states
        |
        v
24-thread candidate generation
        |
        |-- O(1) fingerprint update
        |-- incremental total distance
        |-- incremental misplaced count
        |-- distance histogram update
        v
Fixed candidate array order
        |
        v
Fingerprint -> 16 SQLite shards
        |
        |-- Bloom negative filter
        |-- pending exact set
        |-- SQLite exact primary-key lookup
        |-- batched transaction
        v
Original-order acceptance scan
        |
        v
Original Beam comparator + top 256
        |
        v
Materialize only retained states
        |
        v
Store one path back-pointer per retained node
```

---

## 17. 結論

Q9 能成功完成，並不是因為放寬去重或換成較激進的 pruning，而是把原本記憶體不適合大規模搜尋的資料結構重新安排：

- path 由完整複製改為 back-pointer。
- state fingerprint 改為 O(1) 增量更新。
- visited 從 RAM `unordered_set` 移到精確 SQLite primary key。
- Bloom filter 僅用於加速，不負責直接淘汰候選。
- SQLite 拆成 16 shards 以支援平行查重。
- 候選生成使用 24 threads。
- heuristic 改為只更新被 swap 影響的部分。
- 只有 top-k retained candidates 才建立完整 state。
- 透過固定 candidate index 與原始順序提交，維持決定性搜尋結果。

最終將原本會成長到數十 GB 的 RAM 使用量控制在約 1.56 GB，將大型 visited 集合轉移到約 19.75 GB 的磁碟資料庫，並成功找出 Q9 case1 的 1558-step 有效路徑。
