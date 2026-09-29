#include "daily_momentum.hpp"
#include "momentum.hpp"
#include <stdexcept>

namespace asterion {
std::vector<DailyMomentumSample>
daily_momentum_samples(std::span<const HistoricalDailyBar> bars, std::size_t lookback,
                       std::size_t horizon, std::stop_token stop,
                       const std::function<void(std::size_t, std::size_t)>& progress) {
  PriceMomentum prices(lookback);
  if (!horizon || horizon > 10000 || bars.size() > 10000)
    throw std::invalid_argument("invalid daily momentum horizon or observation count");
  if (bars.size() <= lookback + horizon)
    throw std::invalid_argument("daily momentum requires observations after warmup and horizon");
  auto cancelled = [&] {
    if (stop.stop_requested())
      throw std::runtime_error("daily momentum analysis cancelled");
  };
  for (std::size_t i = 0; i < bars.size(); ++i) {
    cancelled();
    bars[i].validate();
    if (bars[i].close <= Decimal{} || (i && bars[i].trading_day <= bars[i - 1].trading_day))
      throw std::invalid_argument("daily momentum requires positive closes and increasing dates");
  }
  std::vector<DailyMomentumSample> result;
  result.reserve(bars.size() - lookback - horizon);
  for (std::size_t i = 0; i < bars.size(); ++i) {
    cancelled();
    const auto feature = prices.push(bars[i].close);
    if (feature && i + horizon < bars.size()) {
      const auto& future = bars[i + horizon];
      result.push_back({i, bars[i].trading_day, future.trading_day, *feature,
                        price_return(bars[i].close, future.close)});
    }
    if (progress)
      progress(i + 1, bars.size());
  }
  cancelled();
  return result;
}
} // namespace asterion
