#include "reference_model.h"

#include "formulas.h"

#include <algorithm>
#include <set>

namespace flat {

namespace {

bool matches(const DetailRow& d, AcctId a, ProductId p, Direction dir) {
    return d.acct == a && d.product == p && d.dir == dir;
}

}  // namespace

void ReferenceModel::credit(AcctId acct, CurrencyId ccy, double amount) {
    subs_[SubKey{acct, ccy}] += amount;
}

Reject ReferenceModel::apply(const Event& e) {
    const ProductSpec* spec = cfg_ ? cfg_->findProduct(e.product) : nullptr;

    switch (e.type) {
    case EventType::OpenFill: {
        if (e.qty <= 0 || !(e.price > 0.0)) return reject(Reject::InvalidArgs);
        if (!spec) return reject(Reject::UnknownProduct);
        details_.push_back(DetailRow{next_id_++, e.acct, e.product, e.dir, e.price, e.qty, 0});
        return Reject::None;
    }
    case EventType::CloseFill: {
        if (e.qty <= 0 || !(e.price > 0.0)) return reject(Reject::InvalidArgs);
        if (!spec) return reject(Reject::UnknownProduct);
        Qty free = 0;
        for (const auto& d : details_)
            if (matches(d, e.acct, e.product, e.dir)) free += d.qty - d.pending_close;
        if (free < e.qty) return reject(Reject::InsufficientQty);

        Qty remaining = e.qty;
        double realized = 0.0;
        for (auto& d : details_) {
            if (remaining == 0) break;
            if (!matches(d, e.acct, e.product, e.dir)) continue;
            Qty take = std::min(remaining, d.qty - d.pending_close);
            if (take == 0) continue;
            realized += realizedPnl(e.dir, d.open_price, e.price, take, spec->multiplier);
            d.qty -= take;
            remaining -= take;
        }
        details_.erase(std::remove_if(details_.begin(), details_.end(),
                                      [](const DetailRow& d) { return d.qty == 0; }),
                       details_.end());
        credit(e.acct, spec->currency, realized);
        return Reject::None;
    }
    case EventType::LiqRequest: {
        if (e.qty <= 0) return reject(Reject::InvalidArgs);
        if (!spec) return reject(Reject::UnknownProduct);
        Qty free = 0;
        for (const auto& d : details_)
            if (matches(d, e.acct, e.product, e.dir)) free += d.qty - d.pending_close;
        if (free == 0) return reject(Reject::NothingReserved);

        Qty remaining = e.qty;
        for (auto& d : details_) {
            if (remaining == 0) break;
            if (!matches(d, e.acct, e.product, e.dir)) continue;
            Qty take = std::min(remaining, d.qty - d.pending_close);
            d.pending_close += take;
            remaining -= take;
        }
        return Reject::None;
    }
    case EventType::LiqFill: {
        if (e.qty <= 0 || !(e.price > 0.0)) return reject(Reject::InvalidArgs);
        if (!spec) return reject(Reject::UnknownProduct);
        Qty pending = 0;
        for (const auto& d : details_)
            if (matches(d, e.acct, e.product, e.dir)) pending += d.pending_close;
        if (pending < e.qty) return reject(Reject::InsufficientPending);

        Qty remaining = e.qty;
        double realized = 0.0;
        for (auto& d : details_) {
            if (remaining == 0) break;
            if (!matches(d, e.acct, e.product, e.dir)) continue;
            Qty take = std::min(remaining, d.pending_close);
            if (take == 0) continue;
            realized += realizedPnl(e.dir, d.open_price, e.price, take, spec->multiplier);
            d.qty -= take;
            d.pending_close -= take;
            remaining -= take;
        }
        details_.erase(std::remove_if(details_.begin(), details_.end(),
                                      [](const DetailRow& d) { return d.qty == 0; }),
                       details_.end());
        credit(e.acct, spec->currency, realized);
        return Reject::None;
    }
    case EventType::Price: {
        if (!(e.price > 0.0)) return reject(Reject::InvalidArgs);
        if (!spec) return reject(Reject::UnknownProduct);
        prices_[e.product] = e.price;
        return Reject::None;
    }
    case EventType::Deposit: {
        if (e.amount != e.amount) return reject(Reject::InvalidArgs);
        if (!cfg_ || cfg_->fx.count(e.currency) == 0) return reject(Reject::UnknownCurrency);
        credit(e.acct, e.currency, e.amount);
        return Reject::None;
    }
    case EventType::Config: {
        const ConfigSnapshot* c = e.config.get();
        if (!c) return reject(Reject::InvalidConfig);
        for (const auto& kv : c->products)
            if (c->fx.count(kv.second.currency) == 0) return reject(Reject::InvalidConfig);
        for (const auto& d : details_)
            if (!c->findProduct(d.product)) return reject(Reject::InvalidConfig);
        for (const auto& kv : subs_)
            if (c->fx.count(kv.first.currency) == 0) return reject(Reject::InvalidConfig);
        cfg_ = e.config;
        return Reject::None;
    }
    }
    return Reject::None;
}

StateDump ReferenceModel::dump() const {
    StateDump s;
    s.details = details_;
    s.next_detail_id = next_id_;
    s.config_version = cfg_ ? cfg_->version : 0;
    s.rejects = rejects_;

    // 合計：直接從明細從頭彙總
    struct Acc { Qty qty = 0; Qty pending = 0; double cost = 0.0; };
    std::map<PosKey, Acc> acc;
    for (const auto& d : details_) {
        Acc& a = acc[PosKey{d.acct, d.product, d.dir}];
        a.qty += d.qty;
        a.pending += d.pending_close;
        a.cost += static_cast<double>(d.qty) * d.open_price;
    }
    for (const auto& kv : acc) {
        const PosKey& k = kv.first;
        const ProductSpec& spec = *cfg_->findProduct(k.product);
        auto pit = prices_.find(k.product);
        const bool has_price = pit != prices_.end();
        const double price = has_price ? pit->second : 0.0;
        const Valuation v = valuePosition(k.dir, kv.second.qty, kv.second.cost, has_price, price, spec);

        PositionTotal t;
        t.acct = k.acct; t.product = k.product; t.dir = k.dir;
        t.qty = kv.second.qty; t.pending_close = kv.second.pending; t.cost = kv.second.cost;
        t.avg_price = kv.second.cost / static_cast<double>(kv.second.qty);
        t.has_price = has_price; t.last_price = price;
        t.float_pnl = v.float_pnl; t.margin = v.margin;
        s.totals.push_back(t);
    }

    for (const auto& kv : subs_) s.subs.push_back(SubRow{kv.first.acct, kv.first.currency, kv.second});

    // 主帳戶：從子帳戶與上面算出的合計從頭彙總
    std::set<AcctId> accts;
    for (const auto& kv : subs_) accts.insert(kv.first.acct);
    for (const auto& t : s.totals) accts.insert(t.acct);
    for (AcctId a : accts) {
        MainAccount m;
        m.acct = a;
        double balance = 0.0, pnl = 0.0, margin = 0.0;
        for (const auto& kv : subs_)
            if (kv.first.acct == a) balance += kv.second * cfg_->fxRate(kv.first.currency);
        for (const auto& t : s.totals) {
            if (t.acct != a) continue;
            const double fx = cfg_->fxRate(cfg_->findProduct(t.product)->currency);
            pnl += t.float_pnl * fx;
            margin += t.margin * fx;
        }
        m.balance = balance; m.float_pnl = pnl; m.equity = balance + pnl; m.margin = margin;
        m.risk_ratio = margin > 0.0 ? m.equity / margin : 0.0;
        s.accounts.push_back(m);
    }
    return s;
}

}  // namespace flat
