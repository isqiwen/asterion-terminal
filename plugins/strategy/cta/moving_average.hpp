#pragma once
#include <asterion/domain/strategy_port.hpp>
#include <deque>
namespace asterion {
// Long/flat SMA trend strategy. No clock, data source or execution ownership.
class MovingAverage final : public StrategyPort {
public:
  MovingAverage(Instrument instrument, std::size_t fast, std::size_t slow, Decimal quantity);
  PluginDescriptor descriptor() const override;
  void start() override;
  void stop() noexcept override;
  std::optional<Decimal> on_tick(const TradeTick& tick) override;

private:
  Instrument instrument_;
  std::size_t fast_, slow_;
  Decimal quantity_, fast_sum_, slow_sum_;
  std::deque<Decimal> fast_values_, slow_values_;
  std::int64_t last_time_ = -1;
  bool running_ = false;
};
} // namespace asterion
