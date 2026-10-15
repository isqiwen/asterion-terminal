#pragma once
#include <asterion/foundation/decimal.hpp>
#include <chrono>
#include <span>
#include <vector>

namespace asterion::chart_indicators {

// One day of a contract as a chart draws it: its auction prices and volume on
// a provider trading date.
struct DailyCandle
{
    std::chrono::year_month_day trading_day;
    Decimal open, high, low, close, volume;
    auto operator<=>(const DailyCandle&) const = default;
    void validate() const;
};
enum class CalendarPeriod
{
    week,
    month,
    quarter,
    year
};

struct CalendarBar
{
    DailyCandle bar; // trading_day is the last observed source date.
    std::chrono::year_month_day period_begin, period_end, first_day;
    std::size_t source_rows = 0;
};

// ISO Monday-Sunday weeks or calendar months, quarters and years. Missing dates are not
// synthesized; bucket bounds do not prove exchange-session coverage or a completed period.
std::vector<CalendarBar> calendar_bars(std::span<const DailyCandle>, CalendarPeriod);

} // namespace asterion::chart_indicators
