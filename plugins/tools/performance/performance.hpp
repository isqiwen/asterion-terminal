#pragma once
#include <asterion/foundation/decimal.hpp>
#include <chrono>
#include <optional>
#include <span>
namespace asterion {
// What a day-by-day equity record says about a strategy. These are statistics
// for judging a result, not ledger values.
struct Performance {
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
struct EquityDay {
  std::chrono::sys_days day;
  Decimal equity; // at that day's settlement
};
// `days` ascend and follow a positive `deposit`; `marks` is every equity
// observation in order, settlements included, and decides the drawdown.
Performance performance(Decimal deposit, std::span<const EquityDay> days,
                        std::span<const Decimal> marks);
// What the closed positions of a result say. A trade is one contract's
// position from flat to flat again, however many fills built and closed it.
// Its profit is what its sells brought in less what its buys cost, times the
// contract multiplier: before fees, and the same however lots are paired. A
// position still open at the end is no trade.
struct Trades {
  // Closed trades, and those with a profit and with a loss; one that broke
  // even is neither.
  std::size_t count, winning, losing;
  // The average profit of the winning trades and the average loss, a negative
  // amount, of the losing ones; absent without any.
  std::optional<double> average_win, average_loss;
  // The average win over the size of the average loss; absent unless there
  // are both.
  std::optional<double> payoff;
};
struct TradedFill {
  std::size_t contract; // An index into the multipliers.
  bool buy;
  Decimal quantity, price;
};
// `fills` are in the order they happened. A fill that takes a position
// through flat to the other side is refused: closing and opening are
// separate orders.
Trades trades(std::span<const TradedFill> fills, std::span<const Decimal> multipliers);
} // namespace asterion
