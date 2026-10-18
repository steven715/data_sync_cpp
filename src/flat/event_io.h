// 事件錄製與重放：每行一個事件的純文字格式，浮點數以 %.17g 輸出，可精確還原
//
//   O acct product dir qty price      開倉成交
//   C acct product dir qty price      用戶平倉
//   R acct product dir qty            強平請求
//   L acct product dir qty price      強平成交
//   P product price                   行情
//   D acct currency amount            出入金
//   F version threshold nfx {ccy rate}... nprod {pid ccy mult rate}...   配置
//
// dir：0 = 多，1 = 空。以 # 開頭的行為註解。
#pragma once

#include "events.h"

#include <iosfwd>
#include <string>
#include <vector>

namespace flat {

void writeEvent(std::ostream& os, const Event& e);

// 解析失敗時丟出 std::runtime_error，訊息包含行號
std::vector<Event> readEvents(std::istream& is);

}  // namespace flat
