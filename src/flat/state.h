// 狀態的標準化快照：用來比對引擎與參照模型、比對重放前後、計算狀態雜湊
#pragma once

#include "events.h"
#include "tables.h"

#include <array>
#include <string>
#include <vector>

namespace flat {

struct DetailRow {
    DetailId id; AcctId acct; ProductId product; Direction dir;
    double open_price; Qty qty; Qty pending_close;
};

struct SubRow {
    AcctId acct; CurrencyId currency; double balance;
};

struct StateDump {
    // 以下各列皆依主鍵排序
    std::vector<DetailRow>     details;
    std::vector<PositionTotal> totals;
    std::vector<SubRow>        subs;
    std::vector<MainAccount>   accounts;

    DetailId      next_detail_id = 1;
    std::uint64_t config_version = 0;
    std::array<std::uint64_t, static_cast<std::size_t>(Reject::Count)> rejects{};

    // FNV-1a，浮點數以位元表示參與計算：同樣的事件流必須得到同樣的雜湊
    std::uint64_t hash() const;

    // 回傳第一個差異的描述；完全相同時回傳空字串
    std::string diff(const StateDump& other) const;
};

}  // namespace flat
