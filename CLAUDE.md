# data_sync_cpp

C++ 並發資料同步的實驗 repo，場景模擬期貨平台風控系統的持倉、帳戶與行情。

## 結構

- `src/*.cpp`：早期實驗（巢狀 map + 讀寫鎖、拷貝 vs 指標、輪詢 vs 事件驅動），C++23
- `src/flat/`：扁平持倉原型，**固定 C++14**（與公司程式碼一致），設計說明見 `src/flat/README.md`
- `tests/flat/`：扁平原型的測試

## 建置與測試

開發環境是 `docker compose` 的 dev container（Ubuntu 24.04，含 libboost-dev）。

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## 修改 src/flat 時的規則

- 只能用 C++14：不可用 `std::optional`、`std::variant`、structured bindings、`if constexpr`、`if` 初始化語句
- 事實表（明細、子帳戶）只在事件處理函式裡修改；衍生表（合計、主帳戶）只在 `recompute()` 裡整列重建
- 改了業務規則（平倉、強平、拒絕條件），`ReferenceModel` 要同步改，並維持它「最笨、無索引」的寫法
- 會影響狀態的改動，跑完 `ctest` 之外再跑一次：
  `./build/flat_proto run --events 200000 --seed 7 --verify-every 100 --check-ref-every 5000`
- 事件產生器取亂數時先存進區域變數再傳參，避免依賴未規定的參數求值順序
