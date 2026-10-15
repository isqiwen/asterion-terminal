#pragma once
#include <asterion/protocol/backtest.hpp>

namespace asterion {

struct AccountCheck
{
    std::string metric;
    Decimal expected, recorded;
};

struct ReconciledFill
{
    std::string id, order_id;
    bool buy;
    Decimal quantity, price, fee;
};

struct ReconciledDay
{
    std::string day;
    Decimal opening_quantity, quantity_change, closing_quantity, settlement;
    Decimal opening_balance, gross, fees, expected_balance, recorded_balance;
    std::vector<ReconciledFill> fills;
    std::vector<AccountCheck> checks;
};

struct AccountReconciliation
{
    enum class State
    {
        available,
        unsupported_input,
        missing_fill_days
    };
    State state = State::unsupported_input;
    std::string venue, symbol, cost_source, cost_effective_from;
    Decimal deposit, multiplier, signed_notional, gross_notional;
    FuturesCosts costs;
    std::vector<ReconciledDay> days;
    std::vector<AccountCheck> final_checks;
};

// Read-only arithmetic over verified fixed evidence, not a strategy/matching
// replay. Daily attribution needs the recorded fill days. Model terms do not
// establish historical exchange/broker applicability.
AccountReconciliation reconcile_backtest(
    const backtest::v1::BacktestInput& input,
    const backtest::v1::BacktestResult& result);

} // namespace asterion
