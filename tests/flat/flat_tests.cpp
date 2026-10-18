// 扁平持倉原型的測試：手寫情境 + 隨機差分 + 重放
// 不引入測試框架，失敗時印出位置並以非零碼結束（ctest 可直接使用）
#include "engine.h"
#include "event_gen.h"
#include "event_io.h"
#include "reference_model.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <iterator>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

using namespace flat;

namespace flat {
std::ostream& operator<<(std::ostream& os, Reject r) { return os << rejectName(r); }
}  // namespace flat

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) {                                                                 \
            ++g_failures;                                                              \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
        }                                                                              \
    } while (0)

#define CHECK_EQ(a, b)                                                                 \
    do {                                                                               \
        ++g_checks;                                                                    \
        auto va_ = (a); auto vb_ = (b);                                                \
        if (!(va_ == vb_)) {                                                           \
            ++g_failures;                                                              \
            std::ostringstream os_; os_ << va_ << " != " << vb_;                       \
            std::printf("  FAIL %s:%d: %s == %s (%s)\n", __FILE__, __LINE__, #a, #b,   \
                        os_.str().c_str());                                            \
        }                                                                              \
    } while (0)

#define CHECK_NEAR(a, b) CHECK(std::fabs((a) - (b)) < 1e-9)

const ProductId kP1 = 1;   // USD 計價，乘數 10，保證金率 10%
const ProductId kP2 = 2;   // HKD 計價，乘數 50，保證金率 5%
const AcctId    kA  = 7;

ConfigPtr makeConfig(std::uint64_t version, double hkd_fx = 0.128, bool include_p2 = true) {
    auto c = std::make_shared<ConfigSnapshot>();
    c->version = version;
    c->liquidation_threshold = 1.0;
    c->fx[0] = 1.0;
    c->fx[1] = hkd_fx;
    c->products[kP1] = ProductSpec{0, 10.0, 0.10};
    if (include_p2) c->products[kP2] = ProductSpec{1, 50.0, 0.05};
    return c;
}

// 每個情境都從同一個起點開始：有配置、帳戶入金 100000 USD
PositionEngine freshEngine() {
    PositionEngine eng;
    eng.apply(Event::configUpdate(makeConfig(1)));
    eng.apply(Event::deposit(kA, 0, 100000.0));
    eng.recompute();
    return eng;
}

const PositionTotal* findTotal(const PositionEngine& eng, AcctId a, ProductId p, Direction d) {
    const auto& idx = eng.totals().get<by_key>();
    auto it = idx.find(boost::make_tuple(a, p, d));
    return it == idx.end() ? nullptr : &*it;
}

double subBalance(const PositionEngine& eng, AcctId a, CurrencyId c) {
    auto it = eng.subs().find(SubKey{a, c});
    return it == eng.subs().end() ? 0.0 : it->second.balance;
}

void verifyOk(PositionEngine& eng) {
    eng.recompute();
    const std::string err = eng.verify();
    if (!err.empty()) std::printf("  verify: %s\n", err.c_str());
    CHECK(err.empty());
}

// ------------------------------------------------------------------ 情境

void test_fifo_close_consumes_oldest_first() {
    PositionEngine eng = freshEngine();
    eng.apply(Event::open(kA, kP1, Direction::Long, 3, 100.0));  // #1
    eng.apply(Event::open(kA, kP1, Direction::Long, 2, 110.0));  // #2
    CHECK_EQ(eng.apply(Event::close(kA, kP1, Direction::Long, 4, 120.0)), Reject::None);
    verifyOk(eng);

    CHECK_EQ(eng.details().size(), 1u);
    const auto& d = *eng.details().get<by_id>().find(2);
    CHECK_EQ(d.qty, 1);
    // 已實現：#1 三手 (120-100)*3*10 + #2 一手 (120-110)*1*10 = 700
    CHECK_NEAR(subBalance(eng, kA, 0), 100000.0 + 700.0);

    const PositionTotal* t = findTotal(eng, kA, kP1, Direction::Long);
    CHECK(t != nullptr);
    if (t) { CHECK_EQ(t->qty, 1); CHECK_NEAR(t->avg_price, 110.0); }
}

void test_short_close_realizes_inverse_pnl() {
    PositionEngine eng = freshEngine();
    eng.apply(Event::open(kA, kP1, Direction::Short, 2, 100.0));
    eng.apply(Event::close(kA, kP1, Direction::Short, 2, 90.0));
    verifyOk(eng);
    CHECK_NEAR(subBalance(eng, kA, 0), 100000.0 + 200.0);  // (100-90)*2*10
    CHECK(findTotal(eng, kA, kP1, Direction::Short) == nullptr);  // 全平後合計列移除
    CHECK_EQ(eng.details().size(), 0u);
}

void test_long_and_short_are_separate_totals() {
    PositionEngine eng = freshEngine();
    eng.apply(Event::open(kA, kP1, Direction::Long, 2, 100.0));
    eng.apply(Event::open(kA, kP1, Direction::Short, 3, 101.0));
    verifyOk(eng);
    CHECK(findTotal(eng, kA, kP1, Direction::Long) != nullptr);
    CHECK(findTotal(eng, kA, kP1, Direction::Short) != nullptr);

    // 平空單不影響多單
    CHECK_EQ(eng.apply(Event::close(kA, kP1, Direction::Short, 3, 99.0)), Reject::None);
    verifyOk(eng);
    CHECK(findTotal(eng, kA, kP1, Direction::Short) == nullptr);
    const PositionTotal* lt = findTotal(eng, kA, kP1, Direction::Long);
    CHECK(lt != nullptr);
    if (lt) CHECK_EQ(lt->qty, 2);
}

void test_close_cannot_touch_reserved_qty() {
    PositionEngine eng = freshEngine();
    eng.apply(Event::open(kA, kP1, Direction::Long, 5, 100.0));
    CHECK_EQ(eng.apply(Event::liqRequest(kA, kP1, Direction::Long, 3)), Reject::None);
    verifyOk(eng);

    // 可平量只剩 2：上一題的「強平超量」情境在這裡被結構性擋下
    CHECK_EQ(eng.apply(Event::close(kA, kP1, Direction::Long, 3, 100.0)), Reject::InsufficientQty);
    CHECK_EQ(eng.apply(Event::close(kA, kP1, Direction::Long, 2, 100.0)), Reject::None);
    verifyOk(eng);
    const PositionTotal* t = findTotal(eng, kA, kP1, Direction::Long);
    CHECK(t != nullptr);
    if (t) { CHECK_EQ(t->qty, 3); CHECK_EQ(t->pending_close, 3); }

    CHECK_EQ(eng.apply(Event::liqFill(kA, kP1, Direction::Long, 3, 95.0)), Reject::None);
    verifyOk(eng);
    CHECK_EQ(eng.details().size(), 0u);
    CHECK(findTotal(eng, kA, kP1, Direction::Long) == nullptr);
}

void test_liq_request_reserves_fifo_and_caps_at_available() {
    PositionEngine eng = freshEngine();
    eng.apply(Event::open(kA, kP1, Direction::Long, 2, 100.0));  // #1
    eng.apply(Event::open(kA, kP1, Direction::Long, 2, 105.0));  // #2
    CHECK_EQ(eng.apply(Event::liqRequest(kA, kP1, Direction::Long, 3)), Reject::None);
    const auto& byId = eng.details().get<by_id>();
    CHECK_EQ(byId.find(1)->pending_close, 2);
    CHECK_EQ(byId.find(2)->pending_close, 1);

    // 只剩 1 手可預留，請求 10 手時只預留 1 手
    CHECK_EQ(eng.apply(Event::liqRequest(kA, kP1, Direction::Long, 10)), Reject::None);
    CHECK_EQ(byId.find(2)->pending_close, 2);
    CHECK_EQ(eng.apply(Event::liqRequest(kA, kP1, Direction::Long, 1)), Reject::NothingReserved);
    CHECK_EQ(eng.apply(Event::liqFill(kA, kP1, Direction::Long, 5, 100.0)), Reject::InsufficientPending);
    verifyOk(eng);
}

void test_price_revalues_through_product_index() {
    PositionEngine eng = freshEngine();
    eng.apply(Event::open(kA, kP1, Direction::Long, 2, 100.0));
    eng.apply(Event::open(kA + 1, kP1, Direction::Short, 1, 100.0));
    eng.apply(Event::open(kA, kP2, Direction::Long, 1, 20.0));
    eng.recompute();

    eng.apply(Event::tick(kP1, 105.0));
    verifyOk(eng);
    const PositionTotal* t = findTotal(eng, kA, kP1, Direction::Long);
    CHECK(t != nullptr);
    if (t) {
        CHECK(t->has_price);
        CHECK_NEAR(t->float_pnl, 5.0 * 2 * 10);
        CHECK_NEAR(t->margin, 105.0 * 2 * 10 * 0.10);
    }
    const PositionTotal* s = findTotal(eng, kA + 1, kP1, Direction::Short);
    CHECK(s != nullptr);
    if (s) CHECK_NEAR(s->float_pnl, -5.0 * 1 * 10);
    const PositionTotal* other = findTotal(eng, kA, kP2, Direction::Long);  // 不同產品不受影響
    CHECK(other != nullptr);
    if (other) CHECK(!other->has_price);
}

void test_main_account_converts_currencies() {
    PositionEngine eng = freshEngine();
    eng.apply(Event::deposit(kA, 1, 10000.0));                  // HKD 10000
    eng.apply(Event::open(kA, kP2, Direction::Long, 1, 20.0));  // HKD 計價
    eng.apply(Event::tick(kP2, 22.0));
    verifyOk(eng);
    const MainAccount& m = eng.accounts().at(kA);
    CHECK_NEAR(m.balance, 100000.0 + 10000.0 * 0.128);
    CHECK_NEAR(m.float_pnl, 2.0 * 50 * 0.128);
    CHECK_NEAR(m.margin, 22.0 * 50 * 0.05 * 0.128);
    CHECK_NEAR(m.equity, m.balance + m.float_pnl);
}

void test_config_change_revalues_and_validates() {
    PositionEngine eng = freshEngine();
    eng.apply(Event::deposit(kA, 1, 10000.0));
    eng.apply(Event::open(kA, kP2, Direction::Long, 1, 20.0));
    eng.apply(Event::tick(kP2, 20.0));
    verifyOk(eng);

    // 新配置少了持有中的產品：拒絕，舊配置繼續有效
    CHECK_EQ(eng.apply(Event::configUpdate(makeConfig(2, 0.128, false))), Reject::InvalidConfig);
    CHECK_EQ(eng.config()->version, 1u);

    // 匯率變動：主帳戶必須被重新估值
    CHECK_EQ(eng.apply(Event::configUpdate(makeConfig(3, 0.2))), Reject::None);
    verifyOk(eng);
    CHECK_NEAR(eng.accounts().at(kA).balance, 100000.0 + 10000.0 * 0.2);
}

void test_prefix_queries_replace_dimension_maps() {
    PositionEngine eng = freshEngine();
    eng.apply(Event::open(kA, kP1, Direction::Long, 1, 100.0));
    eng.apply(Event::open(kA, kP2, Direction::Short, 1, 20.0));
    eng.apply(Event::open(kA + 1, kP1, Direction::Long, 1, 100.0));
    eng.recompute();

    // 原本的「用戶維度 map」：某主帳戶的全部明細
    const auto& byPos = eng.details().get<by_pos>();
    auto r = byPos.equal_range(boost::make_tuple(kA));
    CHECK_EQ(static_cast<int>(std::distance(r.first, r.second)), 2);

    // 原本的「產品維度 map」：某產品的全部明細
    const auto& byProd = eng.details().get<by_product>();
    auto p = byProd.equal_range(kP1);
    CHECK_EQ(static_cast<int>(std::distance(p.first, p.second)), 2);
}

void test_liquidation_alert_when_ratio_below_threshold() {
    PositionEngine eng;
    eng.apply(Event::configUpdate(makeConfig(1)));
    eng.apply(Event::deposit(kA, 0, 1000.0));
    eng.apply(Event::open(kA, kP1, Direction::Long, 10, 100.0));  // 保證金 1000
    eng.apply(Event::tick(kP1, 100.0));
    eng.recompute();
    eng.takeLiquidationAlerts();

    eng.apply(Event::tick(kP1, 99.0));  // 浮虧 100，權益 900 < 保證金 990
    eng.recompute();
    std::vector<AcctId> alerts = eng.takeLiquidationAlerts();
    CHECK_EQ(alerts.size(), 1u);
    if (!alerts.empty()) CHECK_EQ(alerts[0], kA);
    // 強平是決策輸出，引擎不會自己動持倉
    CHECK_EQ(eng.details().begin()->pending_close, 0);
}

void test_rejections_leave_state_untouched() {
    PositionEngine eng;
    CHECK_EQ(eng.apply(Event::open(kA, kP1, Direction::Long, 1, 100.0)), Reject::UnknownProduct);  // 尚無配置
    CHECK_EQ(eng.apply(Event::deposit(kA, 0, 1.0)), Reject::UnknownCurrency);
    eng.apply(Event::configUpdate(makeConfig(1)));
    CHECK_EQ(eng.apply(Event::open(kA, kP1, Direction::Long, 0, 100.0)), Reject::InvalidArgs);
    CHECK_EQ(eng.apply(Event::open(kA, 99, Direction::Long, 1, 100.0)), Reject::UnknownProduct);
    CHECK_EQ(eng.apply(Event::close(kA, kP1, Direction::Long, 1, 100.0)), Reject::InsufficientQty);
    verifyOk(eng);
    CHECK_EQ(eng.details().size(), 0u);
    CHECK_EQ(eng.dump().next_detail_id, 1u);  // 被拒絕的開倉不消耗 ID
}

// ------------------------------------------------------------------ 隨機差分

bool runDifferential(std::uint64_t seed, std::uint64_t events, std::uint64_t recompute_every) {
    GenOptions opt; opt.seed = seed;
    EventGenerator gen(opt);
    PositionEngine eng;
    ReferenceModel ref;
    std::vector<Event> stream = gen.initial();
    for (std::uint64_t i = 0; i < events; ++i) stream.push_back(gen.next());

    std::uint64_t n = 0;
    for (const auto& e : stream) {
        ++n;
        if (eng.apply(e) != ref.apply(e)) {
            std::printf("  seed %llu: reject mismatch at event %llu\n", (unsigned long long)seed, (unsigned long long)n);
            return false;
        }
        if (n % recompute_every == 0) eng.recompute();
        if (n % 997 == 0) {
            eng.recompute();
            std::string err = eng.verify();
            if (err.empty()) err = eng.dump().diff(ref.dump());
            if (!err.empty()) {
                std::printf("  seed %llu after event %llu: %s\n", (unsigned long long)seed, (unsigned long long)n, err.c_str());
                return false;
            }
        }
    }
    eng.recompute();
    return eng.verify().empty() && eng.dump().diff(ref.dump()).empty();
}

void test_random_differential_against_reference() {
    CHECK(runDifferential(1, 40000, 1));
    CHECK(runDifferential(2, 40000, 7));    // 批次重算：中間狀態不同，結果必須一致
    CHECK(runDifferential(3, 40000, 250));
}

void test_recompute_frequency_does_not_change_result() {
    GenOptions opt; opt.seed = 11;
    std::uint64_t hashes[3];
    const std::uint64_t every[3] = {1, 13, 5000};
    for (int k = 0; k < 3; ++k) {
        EventGenerator gen(opt);
        PositionEngine eng;
        std::uint64_t n = 0;
        for (const auto& e : gen.initial()) eng.apply(e);
        for (int i = 0; i < 30000; ++i) {
            eng.apply(gen.next());
            if (++n % every[k] == 0) eng.recompute();
        }
        eng.recompute();
        hashes[k] = eng.dump().hash();
    }
    CHECK_EQ(hashes[0], hashes[1]);
    CHECK_EQ(hashes[0], hashes[2]);
}

// ------------------------------------------------------------------ 重放

void test_record_and_replay_are_bit_identical() {
    GenOptions opt; opt.seed = 42;
    EventGenerator gen(opt);
    std::vector<Event> stream = gen.initial();
    for (int i = 0; i < 30000; ++i) stream.push_back(gen.next());

    std::stringstream file;
    for (const auto& e : stream) writeEvent(file, e);
    std::vector<Event> replayed = readEvents(file);
    CHECK_EQ(replayed.size(), stream.size());

    PositionEngine a, b;
    for (const auto& e : stream) a.apply(e);
    for (const auto& e : replayed) b.apply(e);
    a.recompute(); b.recompute();
    CHECK_EQ(a.dump().hash(), b.dump().hash());
    CHECK(a.dump().diff(b.dump()).empty());
}

void test_same_seed_same_stream() {
    GenOptions opt; opt.seed = 5;
    EventGenerator g1(opt), g2(opt);
    std::stringstream s1, s2;
    for (const auto& e : g1.initial()) writeEvent(s1, e);
    for (const auto& e : g2.initial()) writeEvent(s2, e);
    for (int i = 0; i < 5000; ++i) { writeEvent(s1, g1.next()); writeEvent(s2, g2.next()); }
    CHECK(s1.str() == s2.str());
}

struct Case { const char* name; void (*fn)(); };

}  // namespace

int main() {
    const Case cases[] = {
        {"fifo_close_consumes_oldest_first",            test_fifo_close_consumes_oldest_first},
        {"short_close_realizes_inverse_pnl",            test_short_close_realizes_inverse_pnl},
        {"long_and_short_are_separate_totals",          test_long_and_short_are_separate_totals},
        {"close_cannot_touch_reserved_qty",             test_close_cannot_touch_reserved_qty},
        {"liq_request_reserves_fifo_and_caps",          test_liq_request_reserves_fifo_and_caps_at_available},
        {"price_revalues_through_product_index",        test_price_revalues_through_product_index},
        {"main_account_converts_currencies",            test_main_account_converts_currencies},
        {"config_change_revalues_and_validates",        test_config_change_revalues_and_validates},
        {"prefix_queries_replace_dimension_maps",       test_prefix_queries_replace_dimension_maps},
        {"liquidation_alert_when_ratio_below_threshold", test_liquidation_alert_when_ratio_below_threshold},
        {"rejections_leave_state_untouched",            test_rejections_leave_state_untouched},
        {"random_differential_against_reference",       test_random_differential_against_reference},
        {"recompute_frequency_does_not_change_result",  test_recompute_frequency_does_not_change_result},
        {"record_and_replay_are_bit_identical",         test_record_and_replay_are_bit_identical},
        {"same_seed_same_stream",                       test_same_seed_same_stream},
    };
    for (const auto& c : cases) {
        const int before = g_failures;
        c.fn();
        std::printf("%s %s\n", g_failures == before ? "[ OK ]" : "[FAIL]", c.name);
    }
    std::printf("\n%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
