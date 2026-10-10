#pragma once
#include <asterion/domain/market.hpp>
#include <asterion/domain/position_target.hpp>
#include <optional>
#include <deque>
namespace asterion {
// SMA trend strategy on bar closes: long while the fast average is above the
// slow one, short while it is below, on the sides it may hold; flat otherwise
// and where the two are equal. No clock, data source or execution ownership.
class MovingAverage final {
public:
  MovingAverage(Instrument instrument, std::size_t fast, std::size_t slow, Decimal quantity,
                PositionSides sides);
  void start();
  void stop() noexcept;
  // Called once per completed bar, in order. The returned target is the
  // position wanted, in lots: positive long, negative short. It is an intent,
  // never an authorized order; the host routes it through the execution chain.
  std::optional<Decimal> on_bar(const MarketBar& bar);

private:
  Instrument instrument_;
  std::size_t fast_, slow_;
  Decimal quantity_, fast_sum_, slow_sum_;
  PositionSides sides_;
  std::deque<Decimal> fast_values_, slow_values_;
  std::int64_t last_time_ = -1;
  bool running_ = false;
};
} // namespace asterion
