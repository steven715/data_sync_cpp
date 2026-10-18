// 配置快照：發布後不可變，整個物件以 shared_ptr<const ConfigSnapshot> 傳遞
#pragma once

#include "types.h"

#include <map>
#include <memory>

namespace flat {

// 簡化版產品配置（實際系統會由保證金模板、品牌模板、產品配置展開而來）
struct ProductSpec {
    CurrencyId currency;     // 計價幣種
    double     multiplier;   // 合約乘數
    double     margin_rate;  // 保證金率
};

struct ConfigSnapshot {
    std::uint64_t                   version = 0;
    double                          liquidation_threshold = 1.0;  // 風險率 = 權益 / 保證金，低於此值觸發強平
    std::map<CurrencyId, double>    fx;        // 幣種 -> 對基準幣匯率
    std::map<ProductId, ProductSpec> products;

    const ProductSpec* findProduct(ProductId p) const {
        auto it = products.find(p);
        return it == products.end() ? nullptr : &it->second;
    }
    // 呼叫端需先確認幣種存在（配置驗證保證這點）
    double fxRate(CurrencyId c) const {
        auto it = fx.find(c);
        return it == fx.end() ? 0.0 : it->second;
    }
};

using ConfigPtr = std::shared_ptr<const ConfigSnapshot>;

}  // namespace flat
