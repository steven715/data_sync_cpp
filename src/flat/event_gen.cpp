#include "event_gen.h"

#include <algorithm>

namespace flat {

namespace {
const double kMultipliers[] = {1.0, 10.0, 50.0, 100.0};
}

EventGenerator::EventGenerator(const GenOptions& opt) : opt_(opt), state_(opt.seed) {
    ticks_.resize(opt_.products);
    margin_rate_.resize(opt_.products);
    for (std::uint32_t p = 0; p < opt_.products; ++p) {
        ticks_[p] = 1000 + static_cast<std::int64_t>(p) * 137;
        margin_rate_[p] = 0.05 + 0.01 * static_cast<double>(p % 5);
    }
    const double base_fx[] = {1.0, 0.128, 0.1375, 0.0067, 0.65};
    fx_.resize(opt_.currencies);
    for (std::uint32_t c = 0; c < opt_.currencies; ++c) fx_[c] = base_fx[c % 5];
}

// splitmix64：簡單、快、各平台結果一致
std::uint64_t EventGenerator::rand() {
    std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

// 熱門帳戶與熱門產品：讓平倉與強平有足夠機會命中既有持倉
AcctId EventGenerator::pickAcct() {
    const std::uint32_t hot = std::min<std::uint32_t>(10, opt_.accounts);
    return static_cast<AcctId>(uniform(100) < 70 ? uniform(hot) : uniform(opt_.accounts));
}

ProductId EventGenerator::pickProduct() {
    const std::uint32_t hot = std::min<std::uint32_t>(5, opt_.products);
    return static_cast<ProductId>(uniform(100) < 60 ? uniform(hot) : uniform(opt_.products));
}

ConfigPtr EventGenerator::makeConfig() {
    auto c = std::make_shared<ConfigSnapshot>();
    c->version = ++config_version_;
    c->liquidation_threshold = 1.0;
    for (std::uint32_t i = 0; i < opt_.currencies; ++i) c->fx[i] = fx_[i];
    for (std::uint32_t p = 0; p < opt_.products; ++p) {
        ProductSpec s;
        s.currency = p % opt_.currencies;
        s.multiplier = kMultipliers[p % 4];
        s.margin_rate = margin_rate_[p];
        c->products[p] = s;
    }
    return c;
}

std::vector<Event> EventGenerator::initial() {
    std::vector<Event> out;
    out.push_back(Event::configUpdate(makeConfig()));
    for (std::uint32_t p = 0; p < opt_.products; ++p) out.push_back(Event::tick(p, price(p)));
    for (std::uint32_t a = 0; a < opt_.accounts; ++a)
        out.push_back(Event::deposit(a, 0, 50000.0 + 10000.0 * static_cast<double>(uniform(46))));
    return out;
}

// 注意：函式參數的求值順序未規定，每次取亂數都先存進區域變數，
// 否則不同編譯器會以不同順序消耗亂數，產生不同的事件流。
Event EventGenerator::next() {
    const std::uint64_t r = uniform(1000);

    if (r < 400) {                              // 行情 40%
        ProductId p = pickProduct();
        ticks_[p] = std::max<std::int64_t>(10, ticks_[p] + static_cast<std::int64_t>(uniform(7)) - 3);
        return Event::tick(p, price(p));
    }
    if (r < 680) {                              // 開倉 28%
        const ProductId p = pickProduct();
        const AcctId    a = pickAcct();
        const Direction d = pickDir();
        const Qty       q = 1 + static_cast<Qty>(uniform(5));
        return Event::open(a, p, d, q, price(p));
    }
    if (r < 900) {                              // 用戶平倉 22%
        const ProductId p = pickProduct();
        const AcctId    a = pickAcct();
        const Direction d = pickDir();
        const Qty       q = 1 + static_cast<Qty>(uniform(5));
        return Event::close(a, p, d, q, price(p));
    }
    if (r < 930) {                              // 強平請求 3%
        const ProductId p = pickProduct();
        const AcctId    a = pickAcct();
        const Direction d = pickDir();
        const Qty       q = 1 + static_cast<Qty>(uniform(8));
        return Event::liqRequest(a, p, d, q);
    }
    if (r < 970) {                              // 強平成交 4%
        const ProductId p = pickProduct();
        const AcctId    a = pickAcct();
        const Direction d = pickDir();
        const Qty       q = 1 + static_cast<Qty>(uniform(3));
        return Event::liqFill(a, p, d, q, price(p));
    }
    if (r < 995) {                              // 出入金 2.5%
        const CurrencyId c   = static_cast<CurrencyId>(uniform(opt_.currencies));
        const double     amt = 1000.0 * (static_cast<double>(uniform(41)) - 10.0);
        const AcctId     a   = pickAcct();
        return Event::deposit(a, c, amt);
    }
    if (r < 997) {                              // 配置變更 0.2%：匯率與保證金率微調
        for (auto& f : fx_)
            if (f != 1.0) f *= 1.0 + 0.001 * (static_cast<double>(uniform(11)) - 5.0);
        for (auto& m : margin_rate_)
            m = std::max(0.01, m + 0.005 * (static_cast<double>(uniform(3)) - 1.0));
        return Event::configUpdate(makeConfig());
    }
    // 刻意不合法的事件 0.3%：驗證拒絕路徑在兩個模型中一致
    const bool      zero_qty = uniform(2) == 0;
    const AcctId    a = pickAcct();
    const ProductId p = pickProduct();
    const Direction d = pickDir();
    if (zero_qty) return Event::open(a, p, d, 0, 100.0);
    return Event::open(a, opt_.products + 5, d, 1, 100.0);
}

}  // namespace flat
