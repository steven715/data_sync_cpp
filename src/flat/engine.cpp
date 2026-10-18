#include "engine.h"

#include "formulas.h"

#include <algorithm>
#include <sstream>

namespace flat {

namespace {

bool validQty(Qty q) { return q > 0; }
bool validPrice(double p) { return p > 0.0 && p == p; }

}  // namespace

// ============================================================ 事件入口

Reject PositionEngine::apply(const Event& e) {
    Reject r = Reject::None;
    switch (e.type) {
        case EventType::OpenFill:   r = onOpen(e); break;
        case EventType::CloseFill:  r = onClose(e); break;
        case EventType::LiqRequest: r = onLiqRequest(e); break;
        case EventType::LiqFill:    r = onLiqFill(e); break;
        case EventType::Price:      r = onPrice(e); break;
        case EventType::Deposit:    r = onDeposit(e); break;
        case EventType::Config:     r = onConfig(e); break;
    }
    if (r != Reject::None) ++rejects_[static_cast<std::size_t>(r)];
    return r;
}

void PositionEngine::markPos(const PosKey& k) {
    dirty_pos_.insert(k);
    dirty_acct_.insert(k.acct);
}

void PositionEngine::markValue(const PosKey& k) {
    dirty_val_.insert(k);
    dirty_acct_.insert(k.acct);
}

Reject PositionEngine::onOpen(const Event& e) {
    if (!validQty(e.qty) || !validPrice(e.price)) return Reject::InvalidArgs;
    if (!cfg_ || !cfg_->findProduct(e.product))  return Reject::UnknownProduct;

    PositionDetail d;
    d.id = next_id_++;
    d.acct = e.acct; d.product = e.product; d.dir = e.dir;
    d.open_price = e.price; d.qty = e.qty; d.pending_close = 0;
    details_.insert(d);  // 三個索引同時更新

    markPos(PosKey{e.acct, e.product, e.dir});
    return Reject::None;
}

Qty PositionEngine::unreservedQty(const PosKey& k) const {
    const auto& idx = details_.get<by_pos>();
    auto range = idx.equal_range(boost::make_tuple(k.acct, k.product, k.dir));
    Qty free = 0;
    for (auto it = range.first; it != range.second; ++it) free += it->qty - it->pending_close;
    return free;
}

Qty PositionEngine::pendingQty(const PosKey& k) const {
    const auto& idx = details_.get<by_pos>();
    auto range = idx.equal_range(boost::make_tuple(k.acct, k.product, k.dir));
    Qty pending = 0;
    for (auto it = range.first; it != range.second; ++it) pending += it->pending_close;
    return pending;
}

void PositionEngine::creditRealized(AcctId acct, CurrencyId ccy, double amount) {
    SubKey sk{acct, ccy};
    auto it = subs_.find(sk);
    if (it == subs_.end()) it = subs_.emplace(sk, SubAccount{sk, 0.0}).first;
    it->second.balance += amount;
    dirty_acct_.insert(acct);
}

// 用戶平倉：只能平「未被強平預留」的數量，依開倉順序（ID 遞增）扣減
Reject PositionEngine::onClose(const Event& e) {
    if (!validQty(e.qty) || !validPrice(e.price)) return Reject::InvalidArgs;
    const ProductSpec* spec = cfg_ ? cfg_->findProduct(e.product) : nullptr;
    if (!spec) return Reject::UnknownProduct;

    const PosKey k{e.acct, e.product, e.dir};
    if (unreservedQty(k) < e.qty) return Reject::InsufficientQty;

    auto& idx = details_.get<by_pos>();
    auto range = idx.equal_range(boost::make_tuple(k.acct, k.product, k.dir));
    Qty remaining = e.qty;
    double realized = 0.0;
    for (auto it = range.first; it != range.second && remaining > 0;) {
        const Qty take = std::min(remaining, it->qty - it->pending_close);
        if (take == 0) { ++it; continue; }
        realized += realizedPnl(k.dir, it->open_price, e.price, take, spec->multiplier);
        remaining -= take;
        if (it->qty == take) {
            it = idx.erase(it);  // 三個索引同時移除
        } else {
            idx.modify(it, [take](PositionDetail& d) { d.qty -= take; });
            ++it;
        }
    }

    creditRealized(k.acct, spec->currency, realized);
    markPos(k);
    return Reject::None;
}

// 強平請求：只在明細上預留，實際扣減等強平成交回來。
// 預留與用戶平倉都由同一條執行緒處理，「待平 <= 持倉」由結構保證。
Reject PositionEngine::onLiqRequest(const Event& e) {
    if (!validQty(e.qty)) return Reject::InvalidArgs;
    if (!cfg_ || !cfg_->findProduct(e.product)) return Reject::UnknownProduct;

    const PosKey k{e.acct, e.product, e.dir};
    if (unreservedQty(k) == 0) return Reject::NothingReserved;

    auto& idx = details_.get<by_pos>();
    auto range = idx.equal_range(boost::make_tuple(k.acct, k.product, k.dir));
    Qty remaining = e.qty;  // 可預留量不足時只預留可用部分
    for (auto it = range.first; it != range.second && remaining > 0; ++it) {
        const Qty take = std::min(remaining, it->qty - it->pending_close);
        if (take == 0) continue;
        idx.modify(it, [take](PositionDetail& d) { d.pending_close += take; });
        remaining -= take;
    }
    markPos(k);
    return Reject::None;
}

Reject PositionEngine::onLiqFill(const Event& e) {
    if (!validQty(e.qty) || !validPrice(e.price)) return Reject::InvalidArgs;
    const ProductSpec* spec = cfg_ ? cfg_->findProduct(e.product) : nullptr;
    if (!spec) return Reject::UnknownProduct;

    const PosKey k{e.acct, e.product, e.dir};
    if (pendingQty(k) < e.qty) return Reject::InsufficientPending;

    auto& idx = details_.get<by_pos>();
    auto range = idx.equal_range(boost::make_tuple(k.acct, k.product, k.dir));
    Qty remaining = e.qty;
    double realized = 0.0;
    for (auto it = range.first; it != range.second && remaining > 0;) {
        const Qty take = std::min(remaining, it->pending_close);
        if (take == 0) { ++it; continue; }
        realized += realizedPnl(k.dir, it->open_price, e.price, take, spec->multiplier);
        remaining -= take;
        if (it->qty == take) {
            it = idx.erase(it);
        } else {
            idx.modify(it, [take](PositionDetail& d) { d.qty -= take; d.pending_close -= take; });
            ++it;
        }
    }

    creditRealized(k.acct, spec->currency, realized);
    markPos(k);
    return Reject::None;
}

// 行情：透過合計表的產品索引找出受影響的合計。
// 尚未有合計列的 key 必然已在 dirty 集合中（剛開倉），不會漏算。
Reject PositionEngine::onPrice(const Event& e) {
    if (!validPrice(e.price)) return Reject::InvalidArgs;
    if (!cfg_ || !cfg_->findProduct(e.product)) return Reject::UnknownProduct;

    prices_[e.product] = e.price;
    const auto& idx = totals_.get<by_product>();
    auto range = idx.equal_range(e.product);
    for (auto it = range.first; it != range.second; ++it) markValue(it->key());
    return Reject::None;
}

Reject PositionEngine::onDeposit(const Event& e) {
    if (e.amount != e.amount) return Reject::InvalidArgs;
    if (!cfg_ || cfg_->fx.find(e.currency) == cfg_->fx.end()) return Reject::UnknownCurrency;
    creditRealized(e.acct, e.currency, e.amount);
    return Reject::None;
}

// 配置：整份替換。新配置必須涵蓋所有現存持倉的產品與所有子帳戶的幣種，
// 驗證失敗就保留舊配置繼續運作。
Reject PositionEngine::onConfig(const Event& e) {
    const ConfigSnapshot* c = e.config.get();
    if (!c) return Reject::InvalidConfig;
    for (const auto& kv : c->products)
        if (c->fx.find(kv.second.currency) == c->fx.end()) return Reject::InvalidConfig;
    for (const auto& d : details_)
        if (!c->findProduct(d.product)) return Reject::InvalidConfig;
    for (const auto& kv : subs_)
        if (c->fx.find(kv.first.currency) == c->fx.end()) return Reject::InvalidConfig;

    cfg_ = e.config;
    for (const auto& t : totals_) markValue(t.key());
    for (const auto& kv : accounts_) dirty_acct_.insert(kv.first);
    return Reject::None;
}

// ============================================================ 衍生資料重算

bool PositionEngine::computeTotal(const PosKey& k, PositionTotal& out) const {
    const auto& idx = details_.get<by_pos>();
    auto range = idx.equal_range(boost::make_tuple(k.acct, k.product, k.dir));
    if (range.first == range.second) return false;

    Qty qty = 0, pending = 0;
    double cost = 0.0;
    for (auto it = range.first; it != range.second; ++it) {  // FIFO 順序加總，結果可重現
        qty += it->qty;
        pending += it->pending_close;
        cost += static_cast<double>(it->qty) * it->open_price;
    }

    out.acct = k.acct; out.product = k.product; out.dir = k.dir;
    out.qty = qty; out.pending_close = pending; out.cost = cost;
    out.avg_price = cost / static_cast<double>(qty);
    fillValuation(out);
    return true;
}

// 估值只依賴 (數量, 成本, 現價, 配置)，不需要再掃明細
void PositionEngine::fillValuation(PositionTotal& t) const {
    const ProductSpec& spec = *cfg_->findProduct(t.product);  // 配置驗證保證存在
    auto pit = prices_.find(t.product);
    t.has_price = pit != prices_.end();
    t.last_price = t.has_price ? pit->second : 0.0;
    const Valuation v = valuePosition(t.dir, t.qty, t.cost, t.has_price, t.last_price, spec);
    t.float_pnl = v.float_pnl;
    t.margin = v.margin;
}

bool PositionEngine::computeAccount(AcctId acct, const TotalTable& totals, MainAccount& out) const {
    bool any = false;
    double balance = 0.0, pnl = 0.0, margin = 0.0;

    for (auto it = subs_.lower_bound(SubKey{acct, 0}); it != subs_.end() && it->first.acct == acct; ++it) {
        balance += it->second.balance * cfg_->fxRate(it->first.currency);
        any = true;
    }
    const auto& idx = totals.get<by_key>();
    auto range = idx.equal_range(boost::make_tuple(acct));  // 前綴查詢：該主帳戶的全部合計
    for (auto it = range.first; it != range.second; ++it) {
        const double fx = cfg_->fxRate(cfg_->findProduct(it->product)->currency);
        pnl += it->float_pnl * fx;
        margin += it->margin * fx;
        any = true;
    }
    if (!any) return false;

    out.acct = acct;
    out.balance = balance;
    out.float_pnl = pnl;
    out.equity = balance + pnl;
    out.margin = margin;
    out.risk_ratio = margin > 0.0 ? out.equity / margin : 0.0;
    return true;
}

void PositionEngine::rebuildTotal(const PosKey& k) {
    auto& idx = totals_.get<by_key>();
    auto it = idx.find(boost::make_tuple(k.acct, k.product, k.dir));
    PositionTotal t;
    if (!computeTotal(k, t)) {
        if (it != idx.end()) idx.erase(it);  // 已無明細：合計列一併移除
        return;
    }
    if (it == idx.end()) idx.insert(t);
    else idx.replace(it, t);  // 整列覆蓋，不做局部修改
}

void PositionEngine::revalueTotal(const PosKey& k) {
    auto& idx = totals_.get<by_key>();
    auto it = idx.find(boost::make_tuple(k.acct, k.product, k.dir));
    if (it == idx.end()) return;
    PositionTotal t = *it;
    fillValuation(t);
    idx.replace(it, t);  // 仍是整列覆蓋
}

void PositionEngine::rebuildAccount(AcctId acct) {
    MainAccount a;
    if (!computeAccount(acct, totals_, a)) {
        accounts_.erase(acct);
        return;
    }
    accounts_[acct] = a;
    if (a.margin > 0.0 && a.risk_ratio < cfg_->liquidation_threshold) {
        liq_alerts_.push_back(acct);
        ++liq_alert_total_;
    }
}

void PositionEngine::recompute() {
    for (const auto& k : dirty_pos_) {                  // 第一層：明細 → 合計
        rebuildTotal(k);                                 //   重新彙總時已一併估值
        dirty_val_.erase(k);
    }
    dirty_pos_.clear();
    for (const auto& k : dirty_val_) revalueTotal(k);   //   只有價格/配置變：不掃明細
    dirty_val_.clear();
    for (AcctId a : dirty_acct_) rebuildAccount(a);     // 第二層：子帳戶 + 合計 → 主帳戶
    dirty_acct_.clear();
}

std::vector<AcctId> PositionEngine::takeLiquidationAlerts() {
    std::vector<AcctId> out;
    out.swap(liq_alerts_);
    return out;
}

// ============================================================ 驗證與快照

std::string PositionEngine::verify() const {
    if (hasDirty()) return "verify() called with pending dirty keys; call recompute() first";

    // 1. 明細層級不變量
    for (const auto& d : details_) {
        std::ostringstream os;
        if (d.qty <= 0) { os << "detail #" << d.id << " has qty " << d.qty; return os.str(); }
        if (d.pending_close < 0 || d.pending_close > d.qty) {
            os << "detail #" << d.id << " pending_close " << d.pending_close << " out of [0, " << d.qty << "]";
            return os.str();
        }
        if (d.id >= next_id_) { os << "detail #" << d.id << " >= next id " << next_id_; return os.str(); }
        if (!cfg_ || !cfg_->findProduct(d.product)) {
            os << "detail #" << d.id << " product " << d.product << " missing from config";
            return os.str();
        }
    }

    // 2. 從明細從頭重建合計表
    TotalTable expected_totals;
    const auto& by_pos_idx = details_.get<by_pos>();
    for (auto it = by_pos_idx.begin(); it != by_pos_idx.end();) {
        const PosKey k{it->acct, it->product, it->dir};
        PositionTotal t;
        computeTotal(k, t);
        expected_totals.insert(t);
        it = by_pos_idx.upper_bound(boost::make_tuple(k.acct, k.product, k.dir));
    }

    // 3. 從子帳戶 + 重建的合計表從頭重建主帳戶表
    std::set<AcctId> accts;
    for (const auto& kv : subs_) accts.insert(kv.first.acct);
    for (const auto& t : expected_totals) accts.insert(t.acct);
    MainAccountTable expected_accounts;
    for (AcctId a : accts) {
        MainAccount m;
        if (computeAccount(a, expected_totals, m)) expected_accounts[a] = m;
    }

    // 4. 與現存的衍生表逐列比對（位元一致）
    StateDump have, want;
    for (const auto& t : totals_) have.totals.push_back(t);
    for (const auto& t : expected_totals) want.totals.push_back(t);
    for (const auto& kv : accounts_) have.accounts.push_back(kv.second);
    for (const auto& kv : expected_accounts) want.accounts.push_back(kv.second);
    std::string d = have.diff(want);
    if (!d.empty()) return "derived state drift: " + d;

    // 5. 索引一致性：產品索引筆數與主表一致
    if (details_.get<by_product>().size() != details_.size() ||
        totals_.get<by_product>().size() != totals_.size())
        return "secondary index size mismatch";
    return std::string();
}

StateDump PositionEngine::dump() const {
    StateDump s;
    for (const auto& d : details_)
        s.details.push_back(DetailRow{d.id, d.acct, d.product, d.dir, d.open_price, d.qty, d.pending_close});
    std::sort(s.details.begin(), s.details.end(),
              [](const DetailRow& a, const DetailRow& b) { return a.id < b.id; });
    for (const auto& t : totals_) s.totals.push_back(t);  // by_key 已排序
    for (const auto& kv : subs_) s.subs.push_back(SubRow{kv.first.acct, kv.first.currency, kv.second.balance});
    for (const auto& kv : accounts_) s.accounts.push_back(kv.second);
    s.next_detail_id = next_id_;
    s.config_version = cfg_ ? cfg_->version : 0;
    s.rejects = rejects_;
    return s;
}

}  // namespace flat
