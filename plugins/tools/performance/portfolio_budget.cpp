#include "portfolio_budget.hpp"
#include <algorithm>

namespace asterion {
namespace {

Decimal decimal(const protocol::v1::Decimal& value)
{
    return Decimal::from_raw(value.units());
}

} // namespace

PortfolioExposure portfolio_exposure(
    const backtest::v1::BacktestInput& input,
    const backtest::v1::BacktestResult& result)
{
    PortfolioExposure exposure;
    const auto& account = result.account();
    if (result.settlements().empty())
        throw std::invalid_argument("portfolio exposure requires saved settlements");
    const auto& last_day = result.settlements(result.settlements_size() - 1).trading_day();
    for (const auto& fixed : input.paper().contracts()) {
        const auto& terms = fixed.dataset().contract();
        const auto same = [&](const auto& item) {
            return item.venue() == terms.venue() && item.symbol() == terms.symbol();
        };
        const auto saved = std::ranges::find_if(account.contracts(), [&](const auto& item) {
            return same(item.contract());
        });
        if (saved == account.contracts().end())
            throw std::invalid_argument("fixed portfolio contract is missing from saved account");
        const auto schedule = protocol::cost_schedule(fixed.cost_schedule());
        const auto& cost = costs_on(schedule, last_day);
        ContractExposure row;
        row.venue = terms.venue();
        row.symbol = terms.symbol();
        row.mark = decimal(saved->mark());
        row.multiplier = decimal(terms.multiplier());
        row.quantity_increment = decimal(terms.quantity_increment());
        row.cost_source = cost.source;
        row.cost_effective_from = cost.effective_from;
        row.costs = cost.values;
        for (const auto& lot : account.positions())
            if (same(lot)) {
                const auto quantity = decimal(lot.quantity());
                row.gross_quantity = row.gross_quantity + quantity;
                row.net_quantity = lot.side() == protocol::v1::BUY ? row.net_quantity + quantity
                                                                   : row.net_quantity - quantity;
                row.gross_notional = row.gross_notional + quantity * row.mark * row.multiplier;
                row.margin = row.margin + row.costs.margin(quantity, row.mark, row.multiplier);
                row.quantities_on_grid =
                    row.quantities_on_grid && quantity.multiple_of(row.quantity_increment);
            }
        for (const auto& fill : account.fills())
            if (same(fill))
                row.quantities_on_grid = row.quantities_on_grid
                    && decimal(fill.quantity()).multiple_of(row.quantity_increment);
        row.net_notional = row.net_quantity * row.mark * row.multiplier;
        exposure.net_notional = exposure.net_notional + row.net_notional;
        exposure.gross_notional = exposure.gross_notional + row.gross_notional;
        exposure.gross_quantity = exposure.gross_quantity + row.gross_quantity;
        exposure.margin = exposure.margin + row.margin;
        exposure.contracts.push_back(std::move(row));
    }
    exposure.balance = decimal(account.balance());
    exposure.equity = decimal(account.equity());
    exposure.available = decimal(account.available());
    exposure.frozen = decimal(account.frozen());
    exposure.recorded_margin = decimal(account.margin());
    exposure.max_order_quantity = decimal(input.paper().risk().max_order_quantity());
    exposure.max_gross_quantity = decimal(input.paper().risk().max_gross_quantity());
    exposure.max_working_orders = input.paper().risk().max_working_orders();
    return exposure;
}

std::vector<SettlementExposure> settlement_exposure(
    const backtest::v1::BacktestInput& input,
    const backtest::v1::BacktestResult& result)
{
    std::vector<SettlementExposure> points;
    for (const auto& day : result.settlements()) {
        SettlementExposure point;
        point.day = day.trading_day();
        point.balance = decimal(day.balance());
        point.equity = decimal(day.equity());
        if (day.has_occupancy()) {
            point.gross_notional = Decimal{};
            point.margin = decimal(day.occupancy().margin());
            point.available = decimal(day.occupancy().available());
            point.frozen = decimal(day.occupancy().frozen());
        }
        for (const auto& row : day.contracts()) {
            const auto fixed = std::ranges::find_if(input.paper().contracts(), [&](const auto& c) {
                return c.dataset().contract().venue() == row.venue()
                    && c.dataset().contract().symbol() == row.symbol();
            });
            if (fixed == input.paper().contracts().end())
                throw std::invalid_argument(
                    "fixed portfolio contract is missing from saved account");
            const auto multiplier = decimal(fixed->dataset().contract().multiplier());
            point.net_notional = point.net_notional
                + decimal(row.position_quantity()) * decimal(row.price()) * multiplier;
            if (point.gross_notional)
                *point.gross_notional = *point.gross_notional
                    + decimal(row.gross_quantity()) * decimal(row.price()) * multiplier;
        }
        points.push_back(std::move(point));
    }
    return points;
}

BudgetCheck review_budget(Decimal used, std::optional<Decimal> limit)
{
    if (used < Decimal{} || (limit && *limit < Decimal{}))
        throw std::invalid_argument(
            "portfolio budget review requires at least one nonnegative explicit limit");
    return {used, limit, limit ? std::optional{*limit - used} : std::nullopt};
}

} // namespace asterion
