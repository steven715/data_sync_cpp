#include "state.h"

#include <cstring>
#include <sstream>

namespace flat {

const char* rejectName(Reject r) {
    switch (r) {
        case Reject::None:                return "None";
        case Reject::UnknownProduct:      return "UnknownProduct";
        case Reject::InvalidArgs:         return "InvalidArgs";
        case Reject::InsufficientQty:     return "InsufficientQty";
        case Reject::NothingReserved:     return "NothingReserved";
        case Reject::InsufficientPending: return "InsufficientPending";
        case Reject::UnknownCurrency:     return "UnknownCurrency";
        case Reject::InvalidConfig:       return "InvalidConfig";
        case Reject::Count:               break;
    }
    return "?";
}

namespace {

struct Fnv {
    std::uint64_t h = 1469598103934665603ULL;
    void bytes(const void* p, std::size_t n) {
        const unsigned char* c = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) { h ^= c[i]; h *= 1099511628211ULL; }
    }
    void u64(std::uint64_t v) { bytes(&v, sizeof v); }
    void i64(std::int64_t v)  { bytes(&v, sizeof v); }
    void f64(double v) {
        if (v == 0.0) v = 0.0;  // -0.0 與 +0.0 視為相同
        std::uint64_t bits; std::memcpy(&bits, &v, sizeof bits); u64(bits);
    }
};

// 浮點比對要求位元一致：引擎與參照模型的加總順序相同，結果必須完全相同
bool same(double a, double b) { return a == b || (a != a && b != b); }

std::string keyStr(AcctId a, ProductId p, Direction d) {
    std::ostringstream os;
    os << "(acct=" << a << ", product=" << p << ", dir=" << (d == Direction::Long ? "L" : "S") << ")";
    return os.str();
}

}  // namespace

std::uint64_t StateDump::hash() const {
    Fnv f;
    for (const auto& d : details) {
        f.u64(d.id); f.u64(d.acct); f.u64(d.product); f.u64(static_cast<std::uint64_t>(d.dir));
        f.f64(d.open_price); f.i64(d.qty); f.i64(d.pending_close);
    }
    for (const auto& t : totals) {
        f.u64(t.acct); f.u64(t.product); f.u64(static_cast<std::uint64_t>(t.dir));
        f.i64(t.qty); f.i64(t.pending_close); f.f64(t.cost); f.f64(t.avg_price);
        f.u64(t.has_price ? 1 : 0); f.f64(t.last_price); f.f64(t.float_pnl); f.f64(t.margin);
    }
    for (const auto& s : subs) { f.u64(s.acct); f.u64(s.currency); f.f64(s.balance); }
    for (const auto& a : accounts) {
        f.u64(a.acct); f.f64(a.balance); f.f64(a.float_pnl); f.f64(a.equity);
        f.f64(a.margin); f.f64(a.risk_ratio);
    }
    f.u64(next_detail_id); f.u64(config_version);
    for (auto r : rejects) f.u64(r);
    return f.h;
}

std::string StateDump::diff(const StateDump& o) const {
    std::ostringstream os;
    if (next_detail_id != o.next_detail_id)
        { os << "next_detail_id " << next_detail_id << " vs " << o.next_detail_id; return os.str(); }
    if (config_version != o.config_version)
        { os << "config_version " << config_version << " vs " << o.config_version; return os.str(); }
    for (std::size_t i = 0; i < rejects.size(); ++i)
        if (rejects[i] != o.rejects[i]) {
            os << "rejects[" << rejectName(static_cast<Reject>(i)) << "] " << rejects[i] << " vs " << o.rejects[i];
            return os.str();
        }

    if (details.size() != o.details.size())
        { os << "detail count " << details.size() << " vs " << o.details.size(); return os.str(); }
    for (std::size_t i = 0; i < details.size(); ++i) {
        const auto& a = details[i]; const auto& b = o.details[i];
        if (a.id != b.id || a.acct != b.acct || a.product != b.product || a.dir != b.dir ||
            !same(a.open_price, b.open_price) || a.qty != b.qty || a.pending_close != b.pending_close) {
            os << "detail #" << a.id << " vs #" << b.id << " " << keyStr(a.acct, a.product, a.dir)
               << " qty " << a.qty << "/" << b.qty << " pending " << a.pending_close << "/" << b.pending_close;
            return os.str();
        }
    }

    if (totals.size() != o.totals.size())
        { os << "total count " << totals.size() << " vs " << o.totals.size(); return os.str(); }
    for (std::size_t i = 0; i < totals.size(); ++i) {
        const auto& a = totals[i]; const auto& b = o.totals[i];
        if (a.key() != b.key() || a.qty != b.qty || a.pending_close != b.pending_close ||
            !same(a.cost, b.cost) || !same(a.avg_price, b.avg_price) || a.has_price != b.has_price ||
            !same(a.last_price, b.last_price) || !same(a.float_pnl, b.float_pnl) || !same(a.margin, b.margin)) {
            os << "total " << keyStr(a.acct, a.product, a.dir) << " vs " << keyStr(b.acct, b.product, b.dir)
               << " qty " << a.qty << "/" << b.qty << " pnl " << a.float_pnl << "/" << b.float_pnl
               << " margin " << a.margin << "/" << b.margin;
            return os.str();
        }
    }

    if (subs.size() != o.subs.size())
        { os << "sub-account count " << subs.size() << " vs " << o.subs.size(); return os.str(); }
    for (std::size_t i = 0; i < subs.size(); ++i) {
        const auto& a = subs[i]; const auto& b = o.subs[i];
        if (a.acct != b.acct || a.currency != b.currency || !same(a.balance, b.balance)) {
            os << "sub (acct=" << a.acct << ", ccy=" << a.currency << ") balance "
               << a.balance << " vs " << b.balance;
            return os.str();
        }
    }

    if (accounts.size() != o.accounts.size())
        { os << "main-account count " << accounts.size() << " vs " << o.accounts.size(); return os.str(); }
    for (std::size_t i = 0; i < accounts.size(); ++i) {
        const auto& a = accounts[i]; const auto& b = o.accounts[i];
        if (a.acct != b.acct || !same(a.balance, b.balance) || !same(a.float_pnl, b.float_pnl) ||
            !same(a.equity, b.equity) || !same(a.margin, b.margin) || !same(a.risk_ratio, b.risk_ratio)) {
            os << "main account " << a.acct << " vs " << b.acct << " equity " << a.equity << "/" << b.equity
               << " margin " << a.margin << "/" << b.margin;
            return os.str();
        }
    }
    return std::string();
}

}  // namespace flat
