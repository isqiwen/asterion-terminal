#include <asterion/domain/risk_port.hpp>
namespace asterion {
RiskDecision assess_order(const RiskPort& risk, const FuturesAccount& account,
                          const LimitOrder& order, Offset offset) {
  // Quantities are per contract; working orders count across the account.
  const auto& instrument = account.contracts()[account.contract_index(order.instrument)].instrument;
  Decimal gross, pending;
  std::size_t working = 0;
  for (const auto& lot : account.positions())
    if (lot.instrument == order.instrument)
      gross = gross + lot.quantity;
  for (const auto& item : account.orders()) {
    const auto state = item.order.state();
    if (state != OrderState::accepted && state != OrderState::partially_filled)
      continue;
    ++working;
    if (item.offset == Offset::open && item.order.request().instrument == order.instrument)
      pending = pending + item.order.remaining_quantity();
  }
  return risk.evaluate({instrument, order, offset, gross, pending, working});
}
} // namespace asterion
