#pragma once
#include <asterion/foundation/decimal.hpp>
#include <chrono>
#include <optional>
#include <span>
#include <vector>

namespace asterion {

// What a day-by-day equity record says about a strategy. These are statistics
// for judging a result, not ledger values.
struct Performance
{
    std::size_t trading_days;
    // Final equity over the deposit, less one.
    double total_return;
    // The largest fall from an earlier peak, as a share of that peak.
    double max_drawdown;
    // The share of days that ended above the day before.
    double winning_days;
    // A year's worth, scaled by the calendar time the days span. Absent with
    // fewer than 20 trading days or once equity is not positive: a few days say
    // nothing about a year. `sharpe` takes a zero risk-free rate and is also
    // absent when the days do not vary; `calmar` when there was no drawdown.
    std::optional<double> annual_return, annual_volatility, sharpe, calmar;
};

struct EquityDay
{
    std::chrono::sys_days day;
    Decimal equity; // at that day's settlement
};

// `days` ascend and follow a positive `deposit`; `marks` is every equity
// observation in order, settlements included, and decides the drawdown.
Performance
performance(Decimal deposit, std::span<const EquityDay> days, std::span<const Decimal> marks);

// Fixed day-end account evidence. Fees are cumulative since the initial deposit.
struct SettlementDay
{
    std::chrono::sys_days day;
    Decimal equity, fees;
};

struct MonthlyContribution
{
    std::chrono::sys_days first_day, last_day;
    std::size_t trading_days;
    Decimal opening_equity, closing_equity, gross, fees, net;
    // Net P&L divided by the original deposit: additive contributions to the
    // full result's return, not compounded monthly returns or shares of profit.
    double return_contribution;
};

// Group recorded trading dates by calendar month, including partial months.
// Missing months are not filled. All positions contribute through settlement
// equity, whether closed or still held; gross adds back fees, not slippage.
// A backtest has no deposits/withdrawals after its initial deposit.
std::vector<MonthlyContribution>
monthly_contributions(Decimal deposit, std::span<const SettlementDay> days);

// What the closed positions of a result say. A trade is one contract's
// position from flat to flat again, however many fills built and closed it.
// Gross profit is sells less buys times the multiplier. Net profit subtracts
// the fees the account charged on every fill of the round. A position still
// open at the end is no closed trade; its fees remain separate.
struct ClosedTrade
{
    std::size_t contract;
    Decimal gross, fees, net;
    std::size_t first_fill, last_fill;
};

struct OpenTrade
{
    std::size_t contract, first_fill;
    Decimal quantity, fees;
};

struct TradeContribution
{
    std::size_t contract, count;
    Decimal gross, fees, net;
};

struct Trades
{
    std::vector<ClosedTrade> closed;
    std::vector<OpenTrade> open;
    // Closed rounds only, in contract order; no unrealized P&L or open fees.
    std::vector<TradeContribution> by_contract;
    Decimal open_fees;
    // Net winners and losers; a net break-even round is neither.
    std::size_t winning, losing;
    // The average profit of the winning trades and the average loss, a negative
    // amount, of the losing ones; absent without any.
    std::optional<double> average_win, average_loss;
    // The average win over the size of the average loss; absent unless there
    // are both.
    std::optional<double> payoff;
};

struct TradedFill
{
    std::size_t contract; // An index into the multipliers.
    bool buy;
    Decimal quantity, price, fee;
};

// `fills` are in the order they happened. A fill that takes a position
// through flat to the other side is refused: closing and opening are
// separate orders.
Trades trades(std::span<const TradedFill> fills, std::span<const Decimal> multipliers);

} // namespace asterion
