#include "account_reconciliation.hpp"
#include <algorithm>
#include <map>

namespace asterion {
namespace {

Decimal decimal(const protocol::proto::Decimal& value)
{
    return Decimal::from_raw(value.units());
}

Decimal marked_change(Decimal quantity, Decimal mark, Decimal reference, Decimal multiplier)
{
    return quantity * (mark - reference) * multiplier;
}

} // namespace

AccountReconciliation reconcile_backtest(
    const backtest::proto::BacktestInput& input,
    const backtest::proto::BacktestResult& result,
    std::stop_token stop)
{
    const auto cancelled = [&] {
        if (stop.stop_requested())
            throw std::runtime_error("task cancelled");
    };
    cancelled();
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
    for (const auto& order : account.orders()) {
        cancelled();
        buys.emplace(order.id(), order.side() == protocol::proto::BUY);
    }
    Decimal quantity, previous_price, balance = audit.deposit, cumulative_gross, cumulative_fees;
    std::size_t cursor = 0;
    for (const auto& day : result.settlements()) {
        cancelled();
        const auto& row = day.contracts(0);
        ReconciledDay item;
        item.day = day.trading_day();
        item.opening_quantity = quantity;
        item.opening_balance = balance;
        item.settlement = decimal(row.price());
        item.gross = marked_change(quantity, item.settlement, previous_price, audit.multiplier);
        while (cursor < static_cast<std::size_t>(account.fills_size())
            && result.fill_days(static_cast<int>(cursor)).trading_day() == item.day) {
            cancelled();
            const auto& fill = account.fills(static_cast<int>(cursor));
            if (result.fill_days(static_cast<int>(cursor)).fill_id() != fill.id())
                throw std::invalid_argument("invalid backtest fill-day evidence");
            const bool buy = buys.at(fill.order_id());
            const auto lots = decimal(fill.quantity());
            const auto signed_lots = buy ? lots : Decimal{} - lots;
            quantity = quantity + signed_lots;
            item.quantity_change = item.quantity_change + signed_lots;
            item.gross = item.gross
                + marked_change(
                    signed_lots,
                    item.settlement,
                    decimal(fill.price()),
                    audit.multiplier);
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
        cancelled();
        const auto lots = decimal(lot.quantity());
        final_quantity =
            lot.side() == protocol::proto::BUY ? final_quantity + lots : final_quantity - lots;
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

AccountContributions contract_contributions(
    const backtest::proto::BacktestInput& input,
    const backtest::proto::BacktestResult& result,
    std::stop_token stop)
{
    const auto cancelled = [&] {
        if (stop.stop_requested())
            throw std::runtime_error("task cancelled");
    };
    cancelled();
    if (input.paper().contracts().empty() || result.settlements().empty())
        throw std::invalid_argument("contract contributions require original daily evidence");
    if (result.fill_days_size() != result.account().fills_size())
        throw std::invalid_argument("invalid backtest fill-day evidence");
    AccountContributions report;
    report.deposit = decimal(input.paper().deposit());
    using Key = std::pair<std::string, std::string>;
    std::map<Key, std::size_t> indices;
    for (const auto& fixed : input.paper().contracts()) {
        const auto& contract = fixed.dataset().contract();
        if (fixed.dataset().interval_minutes() != 0 || contract.currency().empty()
            || decimal(contract.multiplier()) <= Decimal{})
            throw std::invalid_argument("contract contributions require original daily evidence");
        if (report.contracts.empty())
            report.currency = contract.currency();
        if (contract.currency() != report.currency
            || !indices.emplace(Key{contract.venue(), contract.symbol()}, report.contracts.size())
                .second)
            throw std::invalid_argument(
                "contract contributions require unique same-currency contracts");
        report.contracts.push_back(
            {contract.venue(),
                contract.symbol(),
                contract.currency(),
                decimal(contract.multiplier()),
                {},
                {},
                {},
                {},
                {"position", {}, {}}});
    }
    std::map<std::string, const protocol::proto::Order*> orders;
    for (const auto& order : result.account().orders())
        orders.emplace(order.id(), &order);
    std::vector<Decimal> quantities(report.contracts.size()),
        previous_prices(report.contracts.size());
    std::size_t cursor = 0;
    Decimal balance = report.deposit;
    const auto check = [&](std::vector<AccountCheck>& checks,
                           const char* metric,
                           Decimal expected,
                           Decimal recorded) {
        checks.push_back({metric, expected, recorded});
        if (expected != recorded)
            report.reconciled = false;
    };
    for (int d = 0; d < result.settlements_size(); ++d) {
        cancelled();
        const auto& day = result.settlements(d);
        std::map<Key, std::vector<int>> fills;
        while (cursor < static_cast<std::size_t>(result.account().fills_size())
            && result.fill_days(static_cast<int>(cursor)).trading_day() == day.trading_day()) {
            const auto& fill = result.account().fills(static_cast<int>(cursor));
            if (result.fill_days(static_cast<int>(cursor)).fill_id() != fill.id())
                throw std::invalid_argument("invalid backtest fill-day evidence");
            fills[{fill.venue(), fill.symbol()}].push_back(static_cast<int>(cursor++));
        }
        ContributionDay total{
            static_cast<std::size_t>(d),
            day.trading_day(),
            {},
            {},
            {},
            decimal(day.balance()),
            decimal(day.equity()),
            {},
            {}
        };
        std::vector<bool> seen(report.contracts.size());
        for (const auto& original : day.contracts()) {
            cancelled();
            const Key key{original.venue(), original.symbol()};
            const auto found = indices.find(key);
            if (found == indices.end() || seen[found->second])
                throw std::invalid_argument("invalid original contract settlement identity");
            const auto index = found->second;
            seen[index] = true;
            auto& contract = report.contracts[index];
            ContractContributionDay row{
                static_cast<std::size_t>(d),
                day.trading_day(),
                decimal(original.price()),
                quantities[index],
                {},
                {},
                {},
                {},
                {},
                {}
            };
            row.gross = marked_change(
                quantities[index],
                row.settlement,
                previous_prices[index],
                contract.multiplier);
            if (const auto trades = fills.find(key); trades != fills.end()) {
                for (const auto f : trades->second) {
                    cancelled();
                    const auto& fill = result.account().fills(f);
                    const auto order = orders.find(fill.order_id());
                    if (order == orders.end() || order->second->venue() != fill.venue()
                        || order->second->symbol() != fill.symbol()
                        || (order->second->side() != protocol::proto::BUY
                            && order->second->side() != protocol::proto::SELL))
                        throw std::invalid_argument("invalid original fill order identity");
                    const bool buy = order->second->side() == protocol::proto::BUY;
                    const auto lots = decimal(fill.quantity());
                    const auto signed_lots = buy ? lots : Decimal{} - lots;
                    row.quantity_change = row.quantity_change + signed_lots;
                    row.gross = row.gross
                        + marked_change(
                            signed_lots,
                            row.settlement,
                            decimal(fill.price()),
                            contract.multiplier);
                    row.fees = row.fees + decimal(fill.fee());
                    row.fills.push_back(
                        {fill.id(),
                            fill.order_id(),
                            buy,
                            lots,
                            decimal(fill.price()),
                            decimal(fill.fee())});
                }
                fills.erase(trades);
            }
            quantities[index] = quantities[index] + row.quantity_change;
            row.closing_quantity = quantities[index];
            const auto& fixed = input.paper().contracts(static_cast<int>(index)).dataset().days();
            const auto price = std::lower_bound(
                fixed.begin(),
                fixed.end(),
                day.trading_day(),
                [](const auto& saved, const auto& label) {
                    return saved.trading_day() < label;
                });
            if (price == fixed.end() || price->trading_day() != day.trading_day())
                throw std::invalid_argument("original settlement has no fixed source day");
            check(row.checks, "settlement", decimal(price->settlement_price()), row.settlement);
            check(
                row.checks,
                "position",
                row.closing_quantity,
                decimal(original.position_quantity()));
            previous_prices[index] = row.settlement;
            contract.gross = contract.gross + row.gross;
            contract.fees = contract.fees + row.fees;
            contract.net = contract.gross - contract.fees;
            total.gross = total.gross + row.gross;
            total.fees = total.fees + row.fees;
            contract.days.push_back(std::move(row));
        }
        if (!fills.empty())
            throw std::invalid_argument("original fill has no matching contract settlement");
        report.gross = report.gross + total.gross;
        report.fees = report.fees + total.fees;
        balance = balance + total.gross - total.fees;
        total.expected_balance = balance;
        total.cumulative_residual = total.recorded_balance - balance;
        check(total.checks, "fees", report.fees, decimal(day.fees()));
        check(total.checks, "balance", balance, total.recorded_balance);
        check(total.checks, "equity", balance, total.recorded_equity);
        check(total.checks, "realized", report.gross, decimal(day.realized()));
        report.days.push_back(std::move(total));
    }
    if (cursor != static_cast<std::size_t>(result.account().fills_size()))
        throw std::invalid_argument("invalid backtest fill-day evidence");
    std::vector<Decimal> final_quantities(report.contracts.size());
    for (const auto& lot : result.account().positions()) {
        cancelled();
        const auto found = indices.find({lot.venue(), lot.symbol()});
        if (found == indices.end())
            throw std::invalid_argument("invalid original position contract identity");
        auto& quantity = final_quantities[found->second];
        quantity = lot.side() == protocol::proto::BUY ? quantity + decimal(lot.quantity())
                                                      : quantity - decimal(lot.quantity());
    }
    for (std::size_t c = 0; c < report.contracts.size(); ++c) {
        report.contracts[c].final_position = {"position", quantities[c], final_quantities[c]};
        if (quantities[c] != final_quantities[c])
            report.reconciled = false;
    }
    report.explained_net = report.gross - report.fees;
    report.recorded_net = decimal(result.account().equity()) - report.deposit;
    report.residual = report.recorded_net - report.explained_net;
    check(report.final_checks, "fees", report.fees, decimal(result.account().fees()));
    check(report.final_checks, "balance", balance, decimal(result.account().balance()));
    check(report.final_checks, "equity", balance, decimal(result.account().equity()));
    check(report.final_checks, "realized", report.gross, decimal(result.account().realized()));
    return report;
}

} // namespace asterion
