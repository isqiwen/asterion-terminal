#include <asterion/domain/risk_port.hpp>
namespace asterion {
RiskDecision assess_order(const RiskPort& risk, const FuturesAccount& account,
                          const LimitOrder& order, Offset offset) {
  // Quantities are per contract; working orders count across the account.
  const auto& instrument = account.contracts()[account.contract_index(order.instrument)].instrument;
  Decimal gross, pending;
  for (const auto& lot : account.positions())
    if (lot.instrument == order.instrument)
      gross = gross + lot.quantity;
  for (const auto index : account.working_orders()) {
    const auto& item = account.orders()[index];
    if (item.offset == Offset::open && item.order.request().instrument == order.instrument)
      pending = pending + item.order.remaining_quantity();
  }
  return risk.evaluate(
      {instrument, order, offset, gross, pending, account.working_orders().size()});
}
} // namespace asterion
