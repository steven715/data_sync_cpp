// 計價公式（簡化版）
//
// 引擎與參照模型共用這幾個公式：原型要驗證的是「帳務簿記」(FIFO、索引、
// 衍生資料重算) 是否正確，而不是公式本身。換成實際公式時只需改這裡。
#pragma once

#include "config.h"
#include "types.h"

namespace flat {

// 單筆平倉的已實現損益（計價幣種）
inline double realizedPnl(Direction dir, double open_price, double close_price,
                          Qty qty, double multiplier) {
    return dirSign(dir) * (close_price - open_price) * static_cast<double>(qty) * multiplier;
}

struct Valuation {
    double float_pnl;  // 計價幣種
    double margin;     // 計價幣種
};

// cost = Σ(數量 × 開倉價)；無行情時浮盈為 0，保證金以均價估算
inline Valuation valuePosition(Direction dir, Qty qty, double cost, bool has_price,
                               double price, const ProductSpec& spec) {
    const double q = static_cast<double>(qty);
    Valuation v;
    if (has_price) {
        v.float_pnl = dirSign(dir) * (price * q - cost) * spec.multiplier;
        v.margin    = price * q * spec.multiplier * spec.margin_rate;
    } else {
        v.float_pnl = 0.0;
        v.margin    = cost * spec.multiplier * spec.margin_rate;
    }
    return v;
}

}  // namespace flat
