# 扁平持倉原型（flat position）

> 設計動機與分析（欄位級所有權的問題、barrier 的替代方案、落地順序）見 [`docs/design-notes.md`](../../docs/design-notes.md)。

把風控系統的持倉模型從「物件圖」改成「扁平表 + 索引」的原型，C++14，單執行緒，零鎖。

## 跟原本的對應

| 原本 (`src/resource.h`)                  | 扁平版                                             |
|------------------------------------------|----------------------------------------------------|
| `RealObj`（持倉明細）                    | `PositionDetail`，存在 `DetailTable`（事實）       |
| `CalObj`（持倉合計）                     | `PositionTotal`，存在 `TotalTable`（衍生）         |
| `SharedObj`（主帳戶 + 合約的持倉物件）   | 不再是物件，是查詢：`by_pos` 索引 + 合計表的一列   |
| 用戶維度 map                             | `by_pos` 前綴查詢 `(acct)`                         |
| 產品維度 map                             | `by_product` 索引                                  |

同一份資料只存一次，索引存的是 key 不是指標。新增、修改、刪除都只走一個入口，
Boost.MultiIndex 會自動同步所有索引，不可能漏掉某一邊。

## 三條規則

1. **事實表只由事件修改**：明細、子帳戶
2. **衍生表只由重算產生**：合計、主帳戶，整列覆蓋，從不局部修改
3. **索引只存 key**：由 MultiIndex 維護

依賴鏈：`明細 → 合計 → 主帳戶`、`子帳戶 → 主帳戶`。事件只標記 dirty，`recompute()` 沿鏈重建。

合計的 dirty 分兩種：

- **重新彙總**（成交、強平預留）：數量與成本變了，要掃該 key 的明細
- **只重新估值**（行情、配置）：數量與成本沒變，用合計列上既有的值套新價格或新配置，不掃明細

兩種都是整列覆蓋；`verify()` 一律從明細從頭算，所以估值捷徑若出錯會被抓到。

行情是最頻繁的事件，這個區分讓每筆行情的成本從「受影響合計 × 明細數」降為「受影響合計數」。

## 事件

| 事件         | 作用                                                      |
|--------------|-----------------------------------------------------------|
| `OpenFill`   | 新增一筆明細，ID 遞增配發（ID 順序 = FIFO 順序）          |
| `CloseFill`  | 用戶平倉，FIFO 扣「未被強平預留」的數量                   |
| `LiqRequest` | 強平請求，FIFO 在明細上預留 `pending_close`               |
| `LiqFill`    | 強平成交，FIFO 扣已預留的數量                             |
| `Price`      | 更新最新價，透過合計表的產品索引標記受影響的合計          |
| `Deposit`    | 出入金，調整子帳戶餘額                                    |
| `Config`     | 整份配置替換；不涵蓋現有持倉或子帳戶時拒絕，保留舊配置    |

「待平 ≤ 持倉」由結構保證：預留和用戶平倉都在同一條執行緒、同一個入口處理，
用戶平倉只能動未預留的部分。

## 建置與執行

```sh
docker compose up -d --build          # Dockerfile 已加入 libboost-dev
docker compose exec dev bash

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

### flat_proto

```sh
# 隨機事件 + 每 100 個事件 verify + 每 5000 個事件與參照模型比對，並錄下事件流
./build/flat_proto run --events 200000 --seed 7 --verify-every 100 --check-ref-every 5000 \
    --record /tmp/s7.events --dump

# 重放（重算頻率不同，最終狀態雜湊必須相同）
./build/flat_proto replay /tmp/s7.events --recompute-every 100

# 只量引擎吞吐
./build/flat_proto bench --events 2000000 --recompute-every 100
```

| 選項                    | 說明                                                   |
|-------------------------|--------------------------------------------------------|
| `--events N`            | 事件數（不含開場的配置、行情、入金）                   |
| `--seed S`              | 亂數種子，同一個 seed 在任何平台產生同一串事件         |
| `--accounts` / `--products` | 主帳戶數 / 產品數                                  |
| `--recompute-every K`   | 每 K 個事件重算一次                                    |
| `--verify-every V`      | 每 V 個事件重算並 `verify()`；O(全部資料)，大量時別設 1 |
| `--check-ref-every R`   | 每 R 個事件與參照模型比對                              |
| `--record FILE`         | 錄下完整事件流（純文字，格式見 `event_io.h`）          |
| `--dump`                | 結束時列出風險率最低的 10 個主帳戶                     |

## 正確性怎麼保證

三層，各自獨立：

- **`verify()`**：從事實表從頭重建合計與主帳戶，和引擎增量維護的結果逐位元比對。
  抓的是「漏標 dirty」「索引漂移」這類增量更新的 bug。
- **參照模型 `ReferenceModel`**：刻意寫成最笨的版本（vector + 線性掃描、不存衍生資料），
  與引擎只共用事件定義和計價公式。同一串事件餵給兩者，狀態必須逐位元相同。
  抓的是 FIFO、預留、拒絕規則這類業務邏輯的 bug。
- **重放**：事件流錄成檔案再重放，狀態雜湊必須相同；不同重算頻率的雜湊也必須相同。

## 刻意的簡化

- 計價公式在 `formulas.h`：浮盈 = 方向 × (現價 × 數量 − 成本) × 乘數；保證金 = 現價 × 數量 × 乘數 × 保證金率
- 配置只有匯率、乘數、保證金率、強平門檻，沒有模板展開
- 平倉只有 FIFO
- 強平只輸出「哪個主帳戶低於門檻」，不自動產生強平請求
- 沒有手續費、委託、凍結保證金

## 檔案

```
types.h            ID 型別、PosKey、SubKey
config.h           ConfigSnapshot（不可變，shared_ptr<const>）
formulas.h         計價公式
events.h           事件與拒絕原因
tables.h           四張表與索引定義
engine.h/.cpp      PositionEngine：apply / recompute / verify / dump
reference_model.*  樸素參照模型（oracle）
state.h/.cpp       標準化快照、狀態雜湊、差異描述
event_gen.*        可重現的隨機事件產生器
event_io.*         事件錄製與重放
main.cpp           flat_proto CLI
../../tests/flat/flat_tests.cpp   情境測試 + 隨機差分 + 重放
```

## 下一步

1. 平倉規則「浮盈大的先平」：在 `onClose` 依當前價對該 key 的明細排序後扣減，參照模型同步加上
2. 委託模型：同一套模式，主鍵 OrderId，索引 `(acct, product)` 與狀態；委託凍結併入主帳戶重算
3. 配置編譯：把模板展開成 `(帳戶群組, 產品) → 參數` 查表，放進 `ConfigSnapshot`
4. 增量重算的觸發改成事件佇列，接上多執行緒（按主帳戶分片）
