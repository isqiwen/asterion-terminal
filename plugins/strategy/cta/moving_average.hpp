#pragma once
#include <asterion/domain/market.hpp>
#include <optional>
#include <deque>
namespace asterion {
// Long/flat SMA trend strategy on bar closes. No clock, data source or execution ownership.
class MovingAverage final {
public:
  MovingAverage(Instrument instrument, std::size_t fast, std::size_t slow, Decimal quantity);
  void start();
  void stop() noexcept;
  // Called once per completed bar, in order. The returned target is an intent,
  // never an authorized order; the host routes it through the execution chain.
  std::optional<Decimal> on_bar(const MarketBar& bar);

private:
  Instrument instrument_;
  std::size_t fast_, slow_;
  Decimal quantity_, fast_sum_, slow_sum_;
  std::deque<Decimal> fast_values_, slow_values_;
  std::int64_t last_time_ = -1;
  bool running_ = false;
};
} // namespace asterion
