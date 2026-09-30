#pragma once
#include <asterion/domain/market.hpp>
#include <asterion/kernel/plugin.hpp>
#include <optional>
namespace asterion {
// Dimensionless research features, not monetary ledger values or trade intents.
// A streaming factor receives only the current completed bar and its history.
class FactorPort : public Plugin {
public:
  virtual std::optional<double> on_bar(const MarketBar& bar) = 0;
};
} // namespace asterion
