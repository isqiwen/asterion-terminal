#pragma once
#include <asterion/domain/daily_bars.hpp>
#include <span>

namespace asterion::chart_indicators {
enum class CalendarPeriod { week, month, quarter, year };
struct CalendarBar {
  HistoricalDailyBar bar; // trading_day is the last observed source date.
  std::chrono::year_month_day period_begin, period_end, first_day;
  std::size_t source_rows = 0;
};
// ISO Monday-Sunday weeks or calendar months, quarters and years. Missing dates are not
// synthesized; bucket bounds do not prove exchange-session coverage or a completed period.
std::vector<CalendarBar> calendar_bars(std::span<const HistoricalDailyBar>, CalendarPeriod);
} // namespace asterion::chart_indicators
