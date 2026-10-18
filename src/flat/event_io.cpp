#include "event_io.h"

#include <cstdio>
#include <cstdlib>
#include <istream>
#include <ostream>
#include <sstream>
#include <stdexcept>

namespace flat {

namespace {

std::string fmt(double v) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.17g", v);
    return buf;
}

unsigned dirNum(Direction d) { return d == Direction::Long ? 0u : 1u; }

class LineReader {
public:
    LineReader(const std::string& line, std::size_t lineno) : is_(line), lineno_(lineno) {}

    std::string token() {
        std::string t;
        if (!(is_ >> t)) fail("unexpected end of line");
        return t;
    }
    std::uint64_t u64() {
        std::string t = token();
        char* end = nullptr;
        unsigned long long v = std::strtoull(t.c_str(), &end, 10);
        if (*end != '\0') fail("bad integer '" + t + "'");
        return v;
    }
    std::int64_t i64() {
        std::string t = token();
        char* end = nullptr;
        long long v = std::strtoll(t.c_str(), &end, 10);
        if (*end != '\0') fail("bad integer '" + t + "'");
        return v;
    }
    double f64() {
        std::string t = token();
        char* end = nullptr;
        double v = std::strtod(t.c_str(), &end);  // 正確捨入，可精確還原 %.17g
        if (*end != '\0') fail("bad number '" + t + "'");
        return v;
    }
    Direction dir() {
        std::uint64_t d = u64();
        if (d > 1) fail("bad direction");
        return d == 0 ? Direction::Long : Direction::Short;
    }
    [[noreturn]] void fail(const std::string& msg) {
        throw std::runtime_error("event file line " + std::to_string(lineno_) + ": " + msg);
    }

private:
    std::istringstream is_;
    std::size_t lineno_;
};

}  // namespace

void writeEvent(std::ostream& os, const Event& e) {
    switch (e.type) {
    case EventType::OpenFill:
        os << "O " << e.acct << ' ' << e.product << ' ' << dirNum(e.dir) << ' ' << e.qty << ' ' << fmt(e.price);
        break;
    case EventType::CloseFill:
        os << "C " << e.acct << ' ' << e.product << ' ' << dirNum(e.dir) << ' ' << e.qty << ' ' << fmt(e.price);
        break;
    case EventType::LiqRequest:
        os << "R " << e.acct << ' ' << e.product << ' ' << dirNum(e.dir) << ' ' << e.qty;
        break;
    case EventType::LiqFill:
        os << "L " << e.acct << ' ' << e.product << ' ' << dirNum(e.dir) << ' ' << e.qty << ' ' << fmt(e.price);
        break;
    case EventType::Price:
        os << "P " << e.product << ' ' << fmt(e.price);
        break;
    case EventType::Deposit:
        os << "D " << e.acct << ' ' << e.currency << ' ' << fmt(e.amount);
        break;
    case EventType::Config: {
        const ConfigSnapshot& c = *e.config;
        os << "F " << c.version << ' ' << fmt(c.liquidation_threshold) << ' ' << c.fx.size();
        for (const auto& kv : c.fx) os << ' ' << kv.first << ' ' << fmt(kv.second);
        os << ' ' << c.products.size();
        for (const auto& kv : c.products)
            os << ' ' << kv.first << ' ' << kv.second.currency << ' ' << fmt(kv.second.multiplier)
               << ' ' << fmt(kv.second.margin_rate);
        break;
    }
    }
    os << '\n';
}

std::vector<Event> readEvents(std::istream& is) {
    std::vector<Event> out;
    std::string line;
    std::size_t lineno = 0;
    while (std::getline(is, line)) {
        ++lineno;
        if (line.empty() || line[0] == '#') continue;
        LineReader r(line, lineno);
        const std::string tag = r.token();
        Event e;
        if (tag == "O" || tag == "C" || tag == "L") {
            e.type = tag == "O" ? EventType::OpenFill : tag == "C" ? EventType::CloseFill : EventType::LiqFill;
            e.acct = static_cast<AcctId>(r.u64());
            e.product = static_cast<ProductId>(r.u64());
            e.dir = r.dir();
            e.qty = r.i64();
            e.price = r.f64();
        } else if (tag == "R") {
            e.type = EventType::LiqRequest;
            e.acct = static_cast<AcctId>(r.u64());
            e.product = static_cast<ProductId>(r.u64());
            e.dir = r.dir();
            e.qty = r.i64();
        } else if (tag == "P") {
            e.type = EventType::Price;
            e.product = static_cast<ProductId>(r.u64());
            e.price = r.f64();
        } else if (tag == "D") {
            e.type = EventType::Deposit;
            e.acct = static_cast<AcctId>(r.u64());
            e.currency = static_cast<CurrencyId>(r.u64());
            e.amount = r.f64();
        } else if (tag == "F") {
            auto c = std::make_shared<ConfigSnapshot>();
            c->version = r.u64();
            c->liquidation_threshold = r.f64();
            const std::uint64_t nfx = r.u64();
            for (std::uint64_t i = 0; i < nfx; ++i) {
                CurrencyId ccy = static_cast<CurrencyId>(r.u64());
                c->fx[ccy] = r.f64();
            }
            const std::uint64_t np = r.u64();
            for (std::uint64_t i = 0; i < np; ++i) {
                ProductId pid = static_cast<ProductId>(r.u64());
                ProductSpec s;
                s.currency = static_cast<CurrencyId>(r.u64());
                s.multiplier = r.f64();
                s.margin_rate = r.f64();
                c->products[pid] = s;
            }
            e.type = EventType::Config;
            e.config = c;
        } else {
            r.fail("unknown event tag '" + tag + "'");
        }
        out.push_back(e);
    }
    return out;
}

}  // namespace flat
