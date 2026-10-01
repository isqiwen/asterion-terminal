#pragma once
#include <asterion/domain/daily_bars.hpp>
#include <functional>
#include <span>

namespace asterion {
struct DailyMomentumSample {
  std::size_t observation_index;
  std::chrono::year_month_day trading_day, label_day;
  double value, forward_return;
};
// Windows count available daily observations, not elapsed calendar days.
// Dates are provider trading dates, never synthesized intraday timestamps.
// Only the evaluator reads future closes for labels; features use current/past closes.
std::vector<DailyMomentumSample>
daily_momentum_samples(std::span<const HistoricalDailyBar> bars, std::size_t lookback,
                       std::size_t horizon, std::stop_token stop = {},
                       const std::function<void(std::size_t, std::size_t)>& progress = {});
} // namespace asterion
