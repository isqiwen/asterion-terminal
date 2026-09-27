#pragma once
#include <asterion/domain/market.hpp>
#include <asterion/kernel/plugin.hpp>
#include <optional>
namespace asterion {
// Dimensionless research features, not monetary ledger values or trade intents.
// A streaming factor receives only the current event and its prior history.
class FactorPort : public Plugin {
public:
  virtual std::optional<double> on_tick(const TradeTick& tick) = 0;
};
} // namespace asterion
