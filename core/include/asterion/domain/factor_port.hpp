#pragma once
#include <asterion/domain/market.hpp>
#include <optional>
namespace asterion {
// Dimensionless factor features, not monetary ledger values or trade intents.
// A streaming factor receives only the current completed bar and its history.
class FactorPort {
public:
  virtual ~FactorPort() = default;
  virtual std::optional<double> on_bar(const MarketBar& bar) = 0;
};
} // namespace asterion
