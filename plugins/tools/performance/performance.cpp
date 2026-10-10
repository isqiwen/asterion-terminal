#include "performance.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>
namespace asterion {
namespace {
// Subtract before converting, so small changes of large balances survive.
long double change(Decimal from, Decimal to) {
  return static_cast<long double>(to.raw() - from.raw()) / static_cast<long double>(from.raw());
}
} // namespace
Performance performance(Decimal deposit, std::span<const EquityDay> days,
                        std::span<const Decimal> marks) {
  if (deposit <= Decimal{} || days.empty())
    throw std::invalid_argument("performance needs a positive deposit and at least one day");
  for (std::size_t i = 1; i < days.size(); ++i)
    if (days[i].day <= days[i - 1].day)
      throw std::invalid_argument("performance days must ascend");
  Performance result{};
  result.trading_days = days.size();
  result.total_return = static_cast<double>(change(deposit, days.back().equity));
  auto peak = deposit;
  long double drawdown = 0;
  for (const auto equity : marks) {
    peak = std::max(peak, equity);
    drawdown = std::max(drawdown, -change(peak, equity));
  }
  result.max_drawdown = static_cast<double>(drawdown);
  std::vector<long double> returns;
  auto previous = deposit;
  bool positive = true;
  std::size_t winning = 0;
  for (const auto& day : days) {
    winning += day.equity > previous;
    if (positive)
      returns.push_back(change(previous, day.equity));
    previous = day.equity;
    positive = positive && previous > Decimal{};
  }
  result.winning_days = static_cast<double>(winning) / static_cast<double>(days.size());
  if (days.size() < 20 || !positive)
    return result;
  const long double years =
      static_cast<long double>((days.back().day - days.front().day).count() + 1) / 365.25L;
  const auto periods = static_cast<long double>(returns.size()) / years;
  long double mean = 0, squares = 0;
  for (const auto value : returns)
    mean += value;
  mean /= static_cast<long double>(returns.size());
  for (const auto value : returns)
    squares += (value - mean) * (value - mean);
  const auto deviation = std::sqrt(squares / static_cast<long double>(returns.size() - 1));
  result.annual_return = static_cast<double>(
      std::pow(1 + static_cast<long double>(result.total_return), 1 / years) - 1);
  result.annual_volatility = static_cast<double>(deviation * std::sqrt(periods));
  if (deviation > 0)
    result.sharpe = static_cast<double>(mean / deviation * std::sqrt(periods));
  if (drawdown > 0)
    result.calmar = *result.annual_return / result.max_drawdown;
  return result;
}
Trades trades(std::span<const TradedFill> fills, std::span<const Decimal> multipliers) {
  // What each contract holds now, and what its open trade has paid and received.
  std::vector<Decimal> position(multipliers.size()), cash(multipliers.size());
  Trades result{};
  long double won = 0, lost = 0;
  for (const auto& fill : fills) {
    auto& held = position.at(fill.contract);
    auto& paid = cash[fill.contract];
    const auto after = fill.buy ? held + fill.quantity : held - fill.quantity;
    if ((held > Decimal{} && after < Decimal{}) || (held < Decimal{} && after > Decimal{}))
      throw std::invalid_argument("a fill takes a position through flat to the other side");
    const auto amount = fill.price * fill.quantity;
    paid = fill.buy ? paid - amount : paid + amount;
    held = after;
    if (held != Decimal{})
      continue;
    const auto profit = static_cast<long double>((paid * multipliers[fill.contract]).raw());
    paid = {};
    ++result.count;
    if (profit > 0) {
      ++result.winning;
      won += profit;
    } else if (profit < 0) {
      ++result.losing;
      lost += profit;
    }
  }
  // Raw decimals carry eight places.
  if (result.winning)
    result.average_win = static_cast<double>(won / result.winning / 100000000);
  if (result.losing)
    result.average_loss = static_cast<double>(lost / result.losing / 100000000);
  if (result.winning && result.losing)
    result.payoff = *result.average_win / -*result.average_loss;
  return result;
}
} // namespace asterion
