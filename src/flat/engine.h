// 持倉引擎：單一執行緒擁有全部狀態，依序處理事件
//
//   apply(event)  只修改事實表（明細、子帳戶），並標記受影響的 key 為 dirty
//   recompute()   沿依賴鏈重建衍生表：明細 → 合計 → 主帳戶
//   verify()      從事實表從頭重算一切，與現存衍生表逐列比對
//
// 引擎內部不使用任何鎖；多執行緒時由外層的事件佇列負責交接。
#pragma once

#include "config.h"
#include "events.h"
#include "state.h"
#include "tables.h"

#include <array>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace flat {

class PositionEngine {
public:
    Reject apply(const Event& e);
    void   recompute();
    bool   hasDirty() const { return !dirty_pos_.empty() || !dirty_val_.empty() || !dirty_acct_.empty(); }

    // 回傳空字串代表通過；必須在 recompute() 之後呼叫
    std::string verify() const;
    StateDump   dump() const;

    // 強平決策輸出：重算時風險率低於門檻的主帳戶（只做決策，不直接改持倉）
    std::vector<AcctId> takeLiquidationAlerts();

    const DetailTable&      details()  const { return details_; }
    const TotalTable&       totals()   const { return totals_; }
    const SubAccountTable&  subs()     const { return subs_; }
    const MainAccountTable& accounts() const { return accounts_; }
    const ConfigPtr&        config()   const { return cfg_; }

    std::uint64_t rejectCount(Reject r) const { return rejects_[static_cast<std::size_t>(r)]; }
    std::uint64_t liquidationAlertCount() const { return liq_alert_total_; }

private:
    Reject onOpen(const Event& e);
    Reject onClose(const Event& e);
    Reject onLiqRequest(const Event& e);
    Reject onLiqFill(const Event& e);
    Reject onPrice(const Event& e);
    Reject onDeposit(const Event& e);
    Reject onConfig(const Event& e);

    Qty  unreservedQty(const PosKey& k) const;
    Qty  pendingQty(const PosKey& k) const;
    void creditRealized(AcctId acct, CurrencyId ccy, double amount);
    void markPos(const PosKey& k);    // 明細變了：合計要從明細重新彙總
    void markValue(const PosKey& k);  // 只有價格或配置變了：用既有的數量與成本重新估值

    // 由明細計算一列合計；該 key 已無明細時回傳 false
    bool computeTotal(const PosKey& k, PositionTotal& out) const;
    // 由子帳戶 + 給定的合計表計算一列主帳戶；兩者皆空時回傳 false
    bool computeAccount(AcctId acct, const TotalTable& totals, MainAccount& out) const;

    void rebuildTotal(const PosKey& k);
    void revalueTotal(const PosKey& k);
    void fillValuation(PositionTotal& t) const;
    void rebuildAccount(AcctId acct);

    ConfigPtr                             cfg_;
    std::unordered_map<ProductId, double> prices_;

    DetailTable      details_;   // 事實
    SubAccountTable  subs_;      // 事實
    TotalTable       totals_;    // 衍生
    MainAccountTable accounts_;  // 衍生

    DetailId next_id_ = 1;

    std::unordered_set<PosKey, PosKeyHash> dirty_pos_;  // 要重新彙總
    std::unordered_set<PosKey, PosKeyHash> dirty_val_;  // 只要重新估值
    std::set<AcctId>                       dirty_acct_;

    std::array<std::uint64_t, static_cast<std::size_t>(Reject::Count)> rejects_{};
    std::vector<AcctId> liq_alerts_;
    std::uint64_t       liq_alert_total_ = 0;
};

}  // namespace flat
