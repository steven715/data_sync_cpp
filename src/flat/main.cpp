// flat_proto —— 扁平持倉原型的命令列入口
//
//   flat_proto run    [選項]          用隨機事件跑引擎，可同時驗證與比對參照模型
//   flat_proto replay <事件檔> [選項]  重放錄下的事件檔
//   flat_proto bench  [選項]          只跑引擎量吞吐（不驗證、不比對）
//
// 選項：
//   --events N            事件數（run/bench，預設 100000）
//   --seed S              亂數種子（預設 1）
//   --accounts A          主帳戶數（預設 50）
//   --products P          產品數（預設 20）
//   --recompute-every K   每 K 個事件重算一次衍生資料（預設 1）
//   --verify-every V      每 V 個事件重算並跑 verify()（0 = 關閉，預設 0）
//   --check-ref-every R   每 R 個事件與參照模型比對（0 = 關閉，預設 0；開啟時結束時也會比對）
//   --record FILE         把完整事件流寫入檔案（run）
//   --dump                結束時列出風險率最低的主帳戶
#include "engine.h"
#include "event_gen.h"
#include "event_io.h"
#include "reference_model.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace flat;

namespace {

struct Options {
    std::string   mode;
    std::string   file;
    std::uint64_t events = 100000;
    GenOptions    gen;
    std::uint64_t recompute_every = 1;
    std::uint64_t verify_every = 0;
    std::uint64_t check_ref_every = 0;
    std::string   record;
    bool          dump = false;
};

[[noreturn]] void usage(const std::string& err) {
    if (!err.empty()) std::cerr << "error: " << err << "\n\n";
    std::cerr <<
        "usage:\n"
        "  flat_proto run    [--events N] [--seed S] [--accounts A] [--products P]\n"
        "                    [--recompute-every K] [--verify-every V] [--check-ref-every R]\n"
        "                    [--record FILE] [--dump]\n"
        "  flat_proto replay FILE [--recompute-every K] [--verify-every V] [--check-ref-every R] [--dump]\n"
        "  flat_proto bench  [--events N] [--seed S] [--accounts A] [--products P] [--recompute-every K]\n";
    std::exit(2);
}

std::uint64_t parseU64(const std::string& flag, const char* v) {
    char* end = nullptr;
    unsigned long long n = std::strtoull(v, &end, 10);
    if (*end != '\0') usage("bad value for " + flag + ": " + v);
    return n;
}

Options parseArgs(int argc, char** argv) {
    if (argc < 2) usage("");
    Options o;
    o.mode = argv[1];
    if (o.mode != "run" && o.mode != "replay" && o.mode != "bench") usage("unknown mode '" + o.mode + "'");
    int i = 2;
    if (o.mode == "replay") {
        if (argc < 3) usage("replay needs an event file");
        o.file = argv[2];
        i = 3;
    }
    for (; i < argc; ++i) {
        const std::string f = argv[i];
        auto val = [&]() -> const char* {
            if (i + 1 >= argc) usage("missing value for " + f);
            return argv[++i];
        };
        if      (f == "--events")          o.events = parseU64(f, val());
        else if (f == "--seed")            o.gen.seed = parseU64(f, val());
        else if (f == "--accounts")        o.gen.accounts = static_cast<std::uint32_t>(parseU64(f, val()));
        else if (f == "--products")        o.gen.products = static_cast<std::uint32_t>(parseU64(f, val()));
        else if (f == "--recompute-every") o.recompute_every = parseU64(f, val());
        else if (f == "--verify-every")    o.verify_every = parseU64(f, val());
        else if (f == "--check-ref-every") o.check_ref_every = parseU64(f, val());
        else if (f == "--record")          o.record = val();
        else if (f == "--dump")            o.dump = true;
        else usage("unknown option " + f);
    }
    if (o.recompute_every == 0) usage("--recompute-every must be >= 1");
    if (o.gen.accounts == 0 || o.gen.products == 0) usage("--accounts and --products must be >= 1");
    return o;
}

using Source = std::function<bool(Event&)>;

Source generatorSource(const Options& o) {
    auto gen = std::make_shared<EventGenerator>(o.gen);
    auto init = std::make_shared<std::vector<Event>>(gen->initial());
    auto pos = std::make_shared<std::size_t>(0);
    auto produced = std::make_shared<std::uint64_t>(0);
    const std::uint64_t limit = o.events;
    return [=](Event& e) {
        if (*pos < init->size()) { e = (*init)[(*pos)++]; return true; }
        if (*produced >= limit) return false;
        ++*produced;
        e = gen->next();
        return true;
    };
}

void printDump(const PositionEngine& eng) {
    std::vector<MainAccount> accts;
    for (const auto& kv : eng.accounts())
        if (kv.second.margin > 0.0) accts.push_back(kv.second);
    std::sort(accts.begin(), accts.end(),
              [](const MainAccount& a, const MainAccount& b) { return a.risk_ratio < b.risk_ratio; });
    std::printf("\nlowest risk ratio main accounts (equity / margin):\n");
    std::printf("  %6s %14s %14s %14s %10s\n", "acct", "equity", "float_pnl", "margin", "ratio");
    for (std::size_t i = 0; i < accts.size() && i < 10; ++i)
        std::printf("  %6u %14.2f %14.2f %14.2f %10.4f\n", accts[i].acct, accts[i].equity,
                    accts[i].float_pnl, accts[i].margin, accts[i].risk_ratio);
}

int runStream(const Options& o, Source next) {
    PositionEngine eng;
    std::unique_ptr<ReferenceModel> ref;
    if (o.check_ref_every > 0) ref.reset(new ReferenceModel());
    std::unique_ptr<std::ofstream> rec;
    if (!o.record.empty()) {
        rec.reset(new std::ofstream(o.record));
        if (!*rec) { std::cerr << "cannot open " << o.record << " for writing\n"; return 1; }
        *rec << "# flat_proto event stream, seed=" << o.gen.seed << " accounts=" << o.gen.accounts
             << " products=" << o.gen.products << "\n";
    }

    std::uint64_t n = 0, rejected = 0, verifies = 0, ref_checks = 0;
    auto checkRef = [&]() -> bool {
        eng.recompute();
        ++ref_checks;
        const std::string d = eng.dump().diff(ref->dump());
        if (!d.empty()) {
            std::cerr << "MISMATCH vs reference model after event " << n << ": " << d << "\n";
            return false;
        }
        return true;
    };

    const auto t0 = std::chrono::steady_clock::now();
    Event e;
    while (next(e)) {
        ++n;
        if (rec) writeEvent(*rec, e);
        const Reject r = eng.apply(e);
        if (r != Reject::None) ++rejected;
        if (ref) {
            const Reject rr = ref->apply(e);
            if (rr != r) {
                std::cerr << "MISMATCH at event " << n << ": engine " << rejectName(r)
                          << ", reference " << rejectName(rr) << "\n";
                return 1;
            }
        }
        if (n % o.recompute_every == 0) {
            eng.recompute();
            eng.takeLiquidationAlerts();  // 原型只統計數量；實際系統會轉成強平指令送出
        }
        if (o.verify_every > 0 && n % o.verify_every == 0) {
            eng.recompute();
            ++verifies;
            const std::string err = eng.verify();
            if (!err.empty()) { std::cerr << "VERIFY FAILED after event " << n << ": " << err << "\n"; return 1; }
        }
        if (ref && n % o.check_ref_every == 0 && !checkRef()) return 1;
    }
    eng.recompute();
    const std::string err = eng.verify();
    if (!err.empty()) { std::cerr << "VERIFY FAILED at end: " << err << "\n"; return 1; }
    ++verifies;
    if (ref && !checkRef()) return 1;
    const auto t1 = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();

    const StateDump s = eng.dump();
    std::printf("events         %llu (rejected %llu)\n", (unsigned long long)n, (unsigned long long)rejected);
    for (std::size_t i = 1; i < static_cast<std::size_t>(Reject::Count); ++i)
        if (s.rejects[i]) std::printf("  reject %-20s %llu\n", rejectName(static_cast<Reject>(i)),
                                      (unsigned long long)s.rejects[i]);
    std::printf("details        %zu\n", s.details.size());
    std::printf("totals         %zu\n", s.totals.size());
    std::printf("sub-accounts   %zu\n", s.subs.size());
    std::printf("main accounts  %zu\n", s.accounts.size());
    std::printf("config version %llu\n", (unsigned long long)s.config_version);
    std::printf("liq alerts     %llu\n", (unsigned long long)eng.liquidationAlertCount());
    std::printf("verify() runs  %llu, all passed\n", (unsigned long long)verifies);
    if (ref) std::printf("reference      %llu comparisons, all identical\n", (unsigned long long)ref_checks);
    std::printf("state hash     %016llx\n", (unsigned long long)s.hash());
    std::printf("elapsed        %.3f s\n", secs);
    if (o.dump) printDump(eng);
    return 0;
}

int bench(const Options& o) {
    EventGenerator gen(o.gen);
    std::vector<Event> events = gen.initial();
    events.reserve(events.size() + o.events);
    for (std::uint64_t i = 0; i < o.events; ++i) events.push_back(gen.next());  // 先產生，計時只含引擎

    PositionEngine eng;
    const auto t0 = std::chrono::steady_clock::now();
    std::uint64_t n = 0;
    for (const auto& e : events) {
        eng.apply(e);
        if (++n % o.recompute_every == 0) { eng.recompute(); eng.takeLiquidationAlerts(); }
    }
    eng.recompute();
    const auto t1 = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(t1 - t0).count();

    std::printf("events          %zu\n", events.size());
    std::printf("recompute every %llu event(s)\n", (unsigned long long)o.recompute_every);
    std::printf("elapsed         %.3f s\n", secs);
    std::printf("throughput      %.0f events/s (%.0f ns/event)\n",
                static_cast<double>(events.size()) / secs, secs * 1e9 / static_cast<double>(events.size()));
    std::printf("final details   %zu, totals %zu, main accounts %zu\n",
                eng.details().size(), eng.totals().size(), eng.accounts().size());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const Options o = parseArgs(argc, argv);
    try {
        if (o.mode == "bench") return bench(o);
        if (o.mode == "run") return runStream(o, generatorSource(o));

        std::ifstream in(o.file);
        if (!in) { std::cerr << "cannot open " << o.file << "\n"; return 1; }
        auto events = std::make_shared<std::vector<Event>>(readEvents(in));
        auto pos = std::make_shared<std::size_t>(0);
        return runStream(o, [=](Event& e) {
            if (*pos >= events->size()) return false;
            e = (*events)[(*pos)++];
            return true;
        });
    } catch (const std::exception& ex) {
        std::cerr << "error: " << ex.what() << "\n";
        return 1;
    }
}
