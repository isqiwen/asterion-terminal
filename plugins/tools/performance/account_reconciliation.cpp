#include "account_reconciliation.hpp"
#include <algorithm>
#include <map>

namespace asterion {
namespace {

Decimal decimal(const protocol::v1::Decimal& value)
{
    return Decimal::from_raw(value.units());
}

} // namespace

AccountReconciliation reconcile_backtest(
    const backtest::v1::BacktestInput& input,
    const backtest::v1::BacktestResult& result)
{
    AccountReconciliation audit;
    if (input.paper().contracts_size() != 1
        || input.paper().contracts(0).dataset().interval_minutes() != 0
        || result.settlements().empty())
        return audit;
    const auto& fixed = input.paper().contracts(0);
    const auto& contract = fixed.dataset().contract();
    const auto& account = result.account();
    if (result.fill_days_size() != account.fills_size()) {
        audit.state = AccountReconciliation::State::missing_fill_days;
        return audit;
    }
    audit.state = AccountReconciliation::State::available;
    audit.venue = contract.venue();
    audit.symbol = contract.symbol();
    audit.multiplier = decimal(contract.multiplier());
    audit.deposit = decimal(input.paper().deposit());
    std::map<std::string, bool> buys;
    for (const auto& order : account.orders())
        buys.emplace(order.id(), order.side() == protocol::v1::BUY);
    Decimal quantity, previous_price, balance = audit.deposit, cumulative_gross, cumulative_fees;
    std::size_t cursor = 0;
    for (const auto& day : result.settlements()) {
        const auto& row = day.contracts(0);
        ReconciledDay item;
        item.day = day.trading_day();
        item.opening_quantity = quantity;
        item.opening_balance = balance;
        item.settlement = decimal(row.price());
        item.gross = quantity * (item.settlement - previous_price) * audit.multiplier;
        while (cursor < static_cast<std::size_t>(account.fills_size())
            && result.fill_days(static_cast<int>(cursor)).trading_day() == item.day) {
            const auto& fill = account.fills(static_cast<int>(cursor));
            if (result.fill_days(static_cast<int>(cursor)).fill_id() != fill.id())
                throw std::invalid_argument("invalid backtest fill-day evidence");
            const bool buy = buys.at(fill.order_id());
            const auto lots = decimal(fill.quantity());
            const auto signed_lots = buy ? lots : Decimal{} - lots;
            quantity = quantity + signed_lots;
            item.quantity_change = item.quantity_change + signed_lots;
            item.gross = item.gross
                + signed_lots * (item.settlement - decimal(fill.price())) * audit.multiplier;
            item.fees = item.fees + decimal(fill.fee());
            item.fills.push_back(
                {fill.id(),
                    fill.order_id(),
                    buy,
                    lots,
                    decimal(fill.price()),
                    decimal(fill.fee())});
            ++cursor;
        }
        cumulative_gross = cumulative_gross + item.gross;
        cumulative_fees = cumulative_fees + item.fees;
        balance = balance + item.gross - item.fees;
        item.closing_quantity = quantity;
        item.expected_balance = balance;
        item.recorded_balance = decimal(day.balance());
        const auto fixed_day = std::lower_bound(
            fixed.dataset().days().begin(),
            fixed.dataset().days().end(),
            item.day,
            [](const auto& saved, const auto& label) {
                return saved.trading_day() < label;
            });
        if (fixed_day == fixed.dataset().days().end() || fixed_day->trading_day() != item.day)
            throw std::invalid_argument("invalid backtest fill-day evidence");
        item.checks = {
            {"settlement", decimal(fixed_day->settlement_price()), item.settlement},
            {"position", quantity, decimal(row.position_quantity())},
            {"fees", cumulative_fees, decimal(day.fees())},
            {"balance", balance, decimal(day.balance())},
            {"equity", balance, decimal(day.equity())},
            {"realized", cumulative_gross, decimal(day.realized())}
        };
        audit.days.push_back(std::move(item));
        previous_price = decimal(row.price());
    }
    if (cursor != static_cast<std::size_t>(account.fills_size()))
        throw std::invalid_argument("invalid backtest fill-day evidence");
    const auto version = protocol::backtest_settlement_costs(input, 0, audit.days.back().day);
    audit.costs = version.values;
    audit.cost_source = version.source;
    audit.cost_effective_from = version.effective_from;
    Decimal margin, final_quantity;
    for (const auto& lot : account.positions()) {
        const auto lots = decimal(lot.quantity());
        final_quantity =
            lot.side() == protocol::v1::BUY ? final_quantity + lots : final_quantity - lots;
        audit.gross_notional = audit.gross_notional + lots * previous_price * audit.multiplier;
        // Sum retained position lots, exactly as the fixed model does; rounding
        // each margin charge can differ from first aggregating the quantities.
        margin = margin + audit.costs.margin(lots, previous_price, audit.multiplier);
    }
    audit.signed_notional = final_quantity * previous_price * audit.multiplier;
    audit.final_checks = {
        {"position", quantity, final_quantity},
        {"fees", cumulative_fees, decimal(account.fees())},
        {"balance", balance, decimal(account.balance())},
        {"equity", balance, decimal(account.equity())},
        {"realized", cumulative_gross, decimal(account.realized())},
        {"unrealized", Decimal{}, decimal(account.unrealized())},
        {"frozen", Decimal{}, decimal(account.frozen())},
        {"margin", margin, decimal(account.margin())},
        {"available",
            std::min(decimal(account.balance()), decimal(account.equity())) - margin
                - decimal(account.frozen()),
            decimal(account.available())}
    };
    return audit;
}

} // namespace asterion
