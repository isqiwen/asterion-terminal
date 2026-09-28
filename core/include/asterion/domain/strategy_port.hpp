#pragma once
#include <asterion/domain/market.hpp>
#include <asterion/kernel/plugin.hpp>
#include <optional>
namespace asterion {
// A target is an intent, never an authorized order. The host supplies events in
// order and routes any resulting orders through the account execution chain.
class StrategyPort : public Plugin {
public:
  virtual std::optional<Decimal> on_tick(const TradeTick& tick) = 0;
};
} // namespace asterion
