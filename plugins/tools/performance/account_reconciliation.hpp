#pragma once
#include <asterion/protocol/backtest.hpp>
#include <stop_token>

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

struct ContractContributionDay
{
    std::size_t source_index;
    std::string day;
    Decimal settlement, opening_quantity, quantity_change, closing_quantity, gross, fees;
    std::vector<ReconciledFill> fills;
    std::vector<AccountCheck> checks;
};

struct ContractContribution
{
    std::string venue, symbol, currency;
    Decimal multiplier, gross, fees, net;
    std::vector<ContractContributionDay> days;
    AccountCheck final_position;
};

struct ContributionDay
{
    std::size_t source_index;
    std::string day;
    Decimal gross, fees, expected_balance, recorded_balance, recorded_equity, cumulative_residual;
    std::vector<AccountCheck> checks;
};

struct AccountContributions
{
    std::string currency;
    Decimal deposit, gross, fees, explained_net, recorded_net, residual;
    std::vector<ContractContribution> contracts;
    std::vector<ContributionDay> days;
    std::vector<AccountCheck> final_checks;
    bool reconciled = true;
};

// Full-account all-daily original evidence only; no allocated contract capital.
AccountContributions contract_contributions(
    const backtest::proto::BacktestInput& input,
    const backtest::proto::BacktestResult& result,
    std::stop_token stop = {});

// Read-only arithmetic over verified fixed evidence, not a strategy/matching
// replay. Daily attribution needs the recorded fill days. Model terms do not
// establish historical exchange/broker applicability.
AccountReconciliation reconcile_backtest(
    const backtest::proto::BacktestInput& input,
    const backtest::proto::BacktestResult& result,
    std::stop_token stop = {});

} // namespace asterion
