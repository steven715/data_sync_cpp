// 扁平持倉原型 —— 基本型別
//
// 所有實體都用整數 ID 當鍵（字串在系統邊界 intern 成整數），
// 表與表之間只存 key，不存指標。
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <tuple>

namespace flat {

using DetailId   = std::uint64_t;  // 持倉明細 ID，開倉成交時遞增配發，天然代表 FIFO 順序
using AcctId     = std::uint32_t;  // 主帳戶
using ProductId  = std::uint32_t;  // 產品（合約）
using CurrencyId = std::uint32_t;  // 幣種
using Qty        = std::int64_t;   // 手數，用整數避免浮點比較問題

enum class Direction : std::uint8_t { Long = 0, Short = 1 };

inline int dirSign(Direction d) { return d == Direction::Long ? 1 : -1; }

// 持倉合計的鍵：(主帳戶, 產品, 方向)
struct PosKey {
    AcctId    acct;
    ProductId product;
    Direction dir;
};

inline bool operator==(const PosKey& a, const PosKey& b) {
    return a.acct == b.acct && a.product == b.product && a.dir == b.dir;
}
inline bool operator!=(const PosKey& a, const PosKey& b) { return !(a == b); }
inline bool operator<(const PosKey& a, const PosKey& b) {
    return std::make_tuple(a.acct, a.product, a.dir) <
           std::make_tuple(b.acct, b.product, b.dir);
}

struct PosKeyHash {
    std::size_t operator()(const PosKey& k) const {
        std::uint64_t h = (static_cast<std::uint64_t>(k.acct) << 32) ^
                          (static_cast<std::uint64_t>(k.product) << 1) ^
                          static_cast<std::uint64_t>(k.dir);
        return std::hash<std::uint64_t>()(h);
    }
};

// 子帳戶（幣種帳戶）的鍵：(主帳戶, 幣種)
struct SubKey {
    AcctId     acct;
    CurrencyId currency;
};

inline bool operator<(const SubKey& a, const SubKey& b) {
    return std::make_tuple(a.acct, a.currency) < std::make_tuple(b.acct, b.currency);
}
inline bool operator==(const SubKey& a, const SubKey& b) {
    return a.acct == b.acct && a.currency == b.currency;
}

}  // namespace flat
