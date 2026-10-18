// 扁平資料表
//
// 規則：
//   1. 事實表（明細、子帳戶）只由事件修改
//   2. 衍生表（合計、主帳戶）只由重算產生，整列覆蓋，從不局部修改
//   3. 索引只存 key，由 Boost.MultiIndex 在 insert/modify/erase 時自動同步
#pragma once

#include "types.h"

#include <boost/multi_index/composite_key.hpp>
#include <boost/multi_index/hashed_index.hpp>
#include <boost/multi_index/member.hpp>
#include <boost/multi_index/ordered_index.hpp>
#include <boost/multi_index_container.hpp>

#include <map>

namespace flat {

namespace bmi = boost::multi_index;

// ---------------------------------------------------------------- 持倉明細（事實）
struct PositionDetail {
    DetailId  id;
    AcctId    acct;
    ProductId product;
    Direction dir;
    double    open_price;
    Qty       qty;            // 剩餘手數
    Qty       pending_close;  // 已被強平預留的手數，恆有 0 <= pending_close <= qty
};

struct by_id {};
struct by_pos {};
struct by_product {};

// by_pos 是 (主帳戶, 產品, 方向, ID) 的有序複合鍵：
//   - 給完整前三欄 → 某個合計底下的明細，且依 ID 排序 = FIFO 順序
//   - 只給 (主帳戶)       → 該主帳戶的全部明細（取代原本的「用戶維度 map」）
//   - 只給 (主帳戶, 產品) → 該帳戶該產品的多空明細
using DetailTable = bmi::multi_index_container<
    PositionDetail,
    bmi::indexed_by<
        bmi::hashed_unique<bmi::tag<by_id>,
            bmi::member<PositionDetail, DetailId, &PositionDetail::id>>,
        bmi::ordered_unique<bmi::tag<by_pos>,
            bmi::composite_key<PositionDetail,
                bmi::member<PositionDetail, AcctId,    &PositionDetail::acct>,
                bmi::member<PositionDetail, ProductId, &PositionDetail::product>,
                bmi::member<PositionDetail, Direction, &PositionDetail::dir>,
                bmi::member<PositionDetail, DetailId,  &PositionDetail::id>>>,
        bmi::hashed_non_unique<bmi::tag<by_product>,
            bmi::member<PositionDetail, ProductId, &PositionDetail::product>>
    >>;

// ---------------------------------------------------------------- 持倉合計（衍生）
struct PositionTotal {
    AcctId    acct;
    ProductId product;
    Direction dir;
    Qty    qty;
    Qty    pending_close;
    double cost;        // Σ(數量 × 開倉價)
    double avg_price;   // cost / qty
    bool   has_price;
    double last_price;
    double float_pnl;   // 計價幣種
    double margin;      // 計價幣種

    PosKey key() const { return PosKey{acct, product, dir}; }
};

struct by_key {};

// 合計表也用 MultiIndex：by_key 支援前綴查詢（某主帳戶的所有合計），
// by_product 用於行情變動時找出要重算的合計（取代原本的「產品維度 map」）
using TotalTable = bmi::multi_index_container<
    PositionTotal,
    bmi::indexed_by<
        bmi::ordered_unique<bmi::tag<by_key>,
            bmi::composite_key<PositionTotal,
                bmi::member<PositionTotal, AcctId,    &PositionTotal::acct>,
                bmi::member<PositionTotal, ProductId, &PositionTotal::product>,
                bmi::member<PositionTotal, Direction, &PositionTotal::dir>>>,
        bmi::hashed_non_unique<bmi::tag<by_product>,
            bmi::member<PositionTotal, ProductId, &PositionTotal::product>>
    >>;

// ---------------------------------------------------------------- 子帳戶（事實）
struct SubAccount {
    SubKey key;
    double balance;   // 幣種本位
};
using SubAccountTable = std::map<SubKey, SubAccount>;  // 有序：同一主帳戶的子帳戶相鄰

// ---------------------------------------------------------------- 主帳戶（衍生）
struct MainAccount {
    AcctId acct;
    double balance;     // Σ 子帳戶餘額 × 匯率（基準幣）
    double float_pnl;   // Σ 合計浮盈 × 匯率
    double equity;      // balance + float_pnl
    double margin;      // Σ 合計保證金 × 匯率
    double risk_ratio;  // equity / margin；無保證金占用時為 0
};
using MainAccountTable = std::map<AcctId, MainAccount>;

}  // namespace flat
