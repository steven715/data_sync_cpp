// 樸素參照模型（測試用 oracle）
//
// 刻意寫成最笨的版本：明細放在一個 vector 裡，每次都線性掃描；
// 合計與主帳戶不存，dump() 時才從頭算。沒有索引、沒有 dirty 標記、
// 沒有 MultiIndex——跟引擎共用的只有事件定義與計價公式。
// 用同一串事件餵給兩者，dump() 必須逐位元相同。
#pragma once

#include "config.h"
#include "events.h"
#include "state.h"

#include <map>
#include <vector>

namespace flat {

class ReferenceModel {
public:
    Reject    apply(const Event& e);
    StateDump dump() const;

private:
    Reject reject(Reject r) { ++rejects_[static_cast<std::size_t>(r)]; return r; }
    void   credit(AcctId acct, CurrencyId ccy, double amount);

    ConfigPtr                       cfg_;
    std::map<ProductId, double>     prices_;
    std::vector<DetailRow>          details_;  // 依 ID（= 開倉順序）排列
    std::map<SubKey, double>        subs_;
    DetailId                        next_id_ = 1;
    std::array<std::uint64_t, static_cast<std::size_t>(Reject::Count)> rejects_{};
};

}  // namespace flat
