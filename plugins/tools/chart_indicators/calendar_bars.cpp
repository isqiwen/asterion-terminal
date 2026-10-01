#include "calendar_bars.hpp"
#include <algorithm>
#include <stdexcept>

namespace asterion::chart_indicators {
std::vector<CalendarBar> calendar_bars(std::span<const HistoricalDailyBar> source,
                                       CalendarPeriod period) {
  using namespace std::chrono;
  if (period != CalendarPeriod::week && period != CalendarPeriod::month &&
      period != CalendarPeriod::quarter && period != CalendarPeriod::year)
    throw std::invalid_argument("invalid calendar bar period");
  std::vector<CalendarBar> result;
  std::optional<year_month_day> previous;
  for (const auto& bar : source) {
    bar.validate();
    if (previous && bar.trading_day <= *previous)
      throw std::invalid_argument("calendar bars require strictly increasing source dates");
    previous = bar.trading_day;
    const sys_days date{bar.trading_day};
    const unsigned months_per_period = period == CalendarPeriod::year      ? 12
                                       : period == CalendarPeriod::quarter ? 3
                                                                           : 1;
    const unsigned first_month =
        ((unsigned(bar.trading_day.month()) - 1) / months_per_period) * months_per_period + 1;
    const year_month_day begin =
        period == CalendarPeriod::week
            ? year_month_day{date - days{weekday{date}.iso_encoding() - 1}}
            : year_month_day{bar.trading_day.year(), month{first_month}, day{1}};
    const year_month_day end =
        period == CalendarPeriod::week
            ? year_month_day{sys_days{begin} + days{6}}
            : year_month_day{begin.year() / month{first_month + months_per_period - 1} / last};
    if (result.empty() || result.back().period_begin != begin) {
      result.push_back({bar, begin, end, bar.trading_day, 1});
      continue;
    }
    auto& bucket = result.back();
    auto& value = bucket.bar;
    value.trading_day = bar.trading_day;
    value.high = std::max(value.high, bar.high);
    value.low = std::min(value.low, bar.low);
    value.close = bar.close;
    value.volume = value.volume + bar.volume;
    value.amount = value.amount + bar.amount;
    value.open_interest = bar.open_interest;
    value.settlement = bar.settlement;
    ++bucket.source_rows;
  }
  return result;
}
} // namespace asterion::chart_indicators
