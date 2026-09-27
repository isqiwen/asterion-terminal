#pragma once
#include <asterion/domain/account.hpp>
#include <asterion/kernel/plugin.hpp>
#include <cstddef>
namespace asterion {
// Trusted execution supplies a consistent pre-submit snapshot. Quantities are
// gross, not netted; unfilled close orders do not reduce potential exposure.
struct PreTradeRiskContext {
  const Instrument& instrument;
  const LimitOrder& order;
  Offset offset;
  Decimal gross_position_quantity;
  Decimal pending_open_quantity;
  std::size_t working_orders;
};
enum class RiskReason {
  allowed,
  unavailable,
  invalid_context,
  order_quantity,
  gross_quantity,
  working_orders
};
struct RiskDecision {
  RiskReason reason;
  [[nodiscard]] bool allowed() const noexcept { return reason == RiskReason::allowed; }
};
// Risk implementations own algorithms and parameters; execution owns routing
// and atomic account mutation. Plugin failure must never imply acceptance.
class RiskPort : public Plugin {
public:
  virtual RiskDecision evaluate(const PreTradeRiskContext& context) const = 0;
};
// Reads authoritative typed ledger state. Caller serializes this check with
// submission so another accepted order cannot invalidate the reserved exposure.
RiskDecision assess_order(const RiskPort& risk, const FuturesAccount& account,
                          const LimitOrder& order, Offset offset);
} // namespace asterion
