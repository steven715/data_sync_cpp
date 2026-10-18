// 可重現的隨機事件產生器
//
// 不使用 std::uniform_*_distribution：標準只規定引擎（mt19937 等）的輸出，
// 不規定分佈的實作，libstdc++、libc++、MSVC 會產生不同序列。
// 這裡自己做亂數與分佈，同一個 seed 在任何平台都產生同一串事件。
#pragma once

#include "config.h"
#include "events.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace flat {

struct GenOptions {
    std::uint64_t seed       = 1;
    std::uint32_t accounts   = 50;
    std::uint32_t products   = 20;
    std::uint32_t currencies = 3;
};

class EventGenerator {
public:
    explicit EventGenerator(const GenOptions& opt);

    // 開場事件：第一版配置、每個產品的初始行情、每個帳戶的入金
    std::vector<Event> initial();
    Event next();

private:
    std::uint64_t rand();
    std::uint64_t uniform(std::uint64_t n) { return rand() % n; }

    AcctId    pickAcct();
    ProductId pickProduct();
    Direction pickDir() { return uniform(2) == 0 ? Direction::Long : Direction::Short; }
    double    price(ProductId p) const { return static_cast<double>(ticks_[p]) * 0.25; }  // 0.25 可精確表示
    ConfigPtr makeConfig();

    GenOptions                 opt_;
    std::uint64_t              state_;
    std::vector<std::int64_t>  ticks_;       // 各產品目前價格（以 tick 計）
    std::vector<double>        fx_;          // 各幣種目前匯率
    std::vector<double>        margin_rate_; // 各產品目前保證金率
    std::uint64_t              config_version_ = 0;
};

}  // namespace flat
