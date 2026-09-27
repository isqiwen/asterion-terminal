#include <asterion/domain/risk_port.hpp>
namespace asterion {
RiskDecision assess_order(const RiskPort &risk, const FuturesAccount &account,
                          const LimitOrder &order, Offset offset) {
  Decimal gross, pending;
  std::size_t working = 0;
  for (const auto &lot : account.positions())
    gross = gross + lot.quantity;
  for (const auto &item : account.orders()) {
    const auto state = item.order.state();
    if (state != OrderState::accepted && state != OrderState::partially_filled)
      continue;
    ++working;
    if (item.offset == Offset::open)
      pending = pending + item.order.remaining_quantity();
  }
  return risk.evaluate(
      {account.instrument(), order, offset, gross, pending, working});
}
} // namespace asterion
