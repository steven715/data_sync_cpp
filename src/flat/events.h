// 事件：所有狀態變更的唯一來源
//
// C++14 沒有 std::variant，這裡用帶型別標記的扁平 struct，序列化也比較直接。
#pragma once

#include "config.h"
#include "types.h"

namespace flat {

enum class EventType : std::uint8_t {
    OpenFill,      // 開倉成交：新增一筆明細
    CloseFill,     // 用戶平倉成交：FIFO 扣減「未被強平預留」的數量
    LiqRequest,    // 強平請求：FIFO 在明細上預留待平手數（決策與執行分離）
    LiqFill,       // 強平成交：FIFO 扣減已預留的數量
    Price,         // 行情：更新最新價
    Deposit,       // 出入金：調整子帳戶（幣種帳戶）餘額
    Config,        // 配置變更：整份快照替換
};

struct Event {
    EventType  type    = EventType::Price;
    AcctId     acct    = 0;
    ProductId  product = 0;
    Direction  dir     = Direction::Long;
    Qty        qty     = 0;
    double     price   = 0.0;
    CurrencyId currency = 0;   // Deposit
    double     amount   = 0.0; // Deposit
    ConfigPtr  config;         // Config

    static Event open(AcctId a, ProductId p, Direction d, Qty q, double px) {
        Event e; e.type = EventType::OpenFill; e.acct = a; e.product = p; e.dir = d; e.qty = q; e.price = px; return e;
    }
    static Event close(AcctId a, ProductId p, Direction d, Qty q, double px) {
        Event e; e.type = EventType::CloseFill; e.acct = a; e.product = p; e.dir = d; e.qty = q; e.price = px; return e;
    }
    static Event liqRequest(AcctId a, ProductId p, Direction d, Qty q) {
        Event e; e.type = EventType::LiqRequest; e.acct = a; e.product = p; e.dir = d; e.qty = q; return e;
    }
    static Event liqFill(AcctId a, ProductId p, Direction d, Qty q, double px) {
        Event e; e.type = EventType::LiqFill; e.acct = a; e.product = p; e.dir = d; e.qty = q; e.price = px; return e;
    }
    static Event tick(ProductId p, double px) {
        Event e; e.type = EventType::Price; e.product = p; e.price = px; return e;
    }
    static Event deposit(AcctId a, CurrencyId c, double amt) {
        Event e; e.type = EventType::Deposit; e.acct = a; e.currency = c; e.amount = amt; return e;
    }
    static Event configUpdate(ConfigPtr cfg) {
        Event e; e.type = EventType::Config; e.config = std::move(cfg); return e;
    }
};

// 事件被拒絕的原因；引擎與參照模型必須對同一事件給出同樣的結果
enum class Reject : std::uint8_t {
    None = 0,
    UnknownProduct,     // 產品不在配置中
    InvalidArgs,        // 數量或價格不合法
    InsufficientQty,    // 可平量不足
    NothingReserved,    // 強平請求時沒有可預留數量
    InsufficientPending,// 強平成交數量超過已預留數量
    UnknownCurrency,    // 幣種不在配置中
    InvalidConfig,      // 新配置不涵蓋現有持倉或子帳戶
    Count
};

const char* rejectName(Reject r);

}  // namespace flat
