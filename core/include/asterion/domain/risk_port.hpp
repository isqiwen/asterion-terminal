#pragma once
#include <asterion/domain/account.hpp>
#include <cstddef>
#include <string_view>
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
constexpr std::string_view risk_reason_name(RiskReason reason) noexcept {
  switch (reason) {
  case RiskReason::allowed:
    return "allowed";
  case RiskReason::unavailable:
    return "unavailable";
  case RiskReason::invalid_context:
    return "invalid_context";
  case RiskReason::order_quantity:
    return "order_quantity";
  case RiskReason::gross_quantity:
    return "gross_quantity";
  case RiskReason::working_orders:
    return "working_orders";
  }
  return "unavailable";
}
struct RiskDecision {
  RiskReason reason;
  [[nodiscard]] bool allowed() const noexcept { return reason == RiskReason::allowed; }
};
// Risk implementations own algorithms and parameters; execution owns routing
// and atomic account mutation. Plugin failure must never imply acceptance.
class RiskPort {
public:
  virtual ~RiskPort() = default;
  virtual RiskDecision evaluate(const PreTradeRiskContext& context) const = 0;
};
// Reads authoritative typed ledger state. Caller serializes this check with
// submission so another accepted order cannot invalidate the reserved exposure.
RiskDecision assess_order(const RiskPort& risk, const FuturesAccount& account,
                          const LimitOrder& order, Offset offset);
} // namespace asterion
