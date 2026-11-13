#include <asterion/futures/risk_port.hpp>

namespace asterion {

RiskDecision assess_order(
    const RiskPort& risk,
    const FuturesAccount& account,
    const LimitOrder& order,
    Offset offset)
{
    const auto& instrument =
        account.contracts()[account.contract_index(order.instrument)].instrument;
    Decimal gross, pending, account_gross, account_pending;
    for (const auto& lot : account.positions()) {
        account_gross = account_gross + lot.quantity;
        if (lot.instrument == order.instrument)
            gross = gross + lot.quantity;
    }
    for (const auto index : account.working_orders()) {
        const auto& item = account.orders()[index];
        if (item.offset == Offset::open) {
            account_pending = account_pending + item.order.remaining_quantity();
            if (item.order.request().instrument == order.instrument)
                pending = pending + item.order.remaining_quantity();
        }
    }
    return risk.evaluate(
        {instrument,
            order,
            offset,
            gross,
            pending,
            account.working_orders().size(),
            account_gross,
            account_pending});
}

} // namespace asterion
