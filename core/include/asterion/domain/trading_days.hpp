#pragma once
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
namespace asterion {
// A provider's trading date: not an intraday timestamp.
std::chrono::year_month_day parse_trading_date(std::string_view); // strict YYYY-MM-DD
std::string format_trading_date(std::chrono::year_month_day);
// When a trading day's session closes: 15:00 Asia/Shanghai on it. A daily bar
// is labelled so, and so is the settlement of a day that has no bars.
std::int64_t trading_day_close_ns(std::string_view);
} // namespace asterion
