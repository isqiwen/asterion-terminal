#include "performance.hpp"
#include <cmath>
#include <gtest/gtest.h>
#include <vector>
using namespace asterion;
using namespace std::chrono;
namespace {
Decimal d(const char* value) {
  return Decimal::parse(value);
}
// Consecutive calendar days from 2026-01-05 with the given settlement equity.
std::vector<EquityDay> record_of(const std::vector<const char*>& equity) {
  std::vector<EquityDay> result;
  for (std::size_t i = 0; i < equity.size(); ++i)
    result.push_back({sys_days{year{2026} / January / 5} + std::chrono::days{static_cast<int>(i)},
                      d(equity[i])});
  return result;
}
} // namespace
TEST(Performance, AFewDaysHaveAReturnAndADrawdownButNoYearlyFigures) {
  const auto record = record_of({"101000", "100000", "102000"});
  const std::vector<Decimal> marks{d("100500"), d("101000"), d("99000"), d("100000"), d("102000")};
  const auto result = performance(d("100000"), record, marks);
  EXPECT_EQ(result.trading_days, 3U);
  EXPECT_DOUBLE_EQ(result.total_return, 0.02);
  // The fall from 101000 to 99000 inside the second day, not the day-end 100000.
  EXPECT_NEAR(result.max_drawdown, 2000.0 / 101000, 1e-12);
  EXPECT_DOUBLE_EQ(result.winning_days, 2.0 / 3);
  EXPECT_FALSE(result.annual_return || result.annual_volatility || result.sharpe || result.calmar);
}
TEST(Performance, YearlyFiguresScaleByTheCalendarTimeTheDaysSpan) {
  // Up a quarter and back, ten times: daily returns of +25% and -20%.
  std::vector<const char*> equity;
  for (int i = 0; i < 20; ++i)
    equity.push_back(i % 2 ? "100000" : "125000");
  auto record = record_of(equity);
  std::vector<Decimal> marks;
  for (const auto& day : record)
    marks.push_back(day.equity);
  auto result = performance(d("100000"), record, marks);
  const double periods = 365.25; // 20 days spanning 20 calendar days
  const double deviation = 0.225 * std::sqrt(20.0 / 19);
  EXPECT_DOUBLE_EQ(result.total_return, 0);
  EXPECT_NEAR(*result.annual_return, 0, 1e-12);
  EXPECT_NEAR(*result.annual_volatility, deviation * std::sqrt(periods), 1e-9);
  EXPECT_NEAR(*result.sharpe, 0.025 / deviation * std::sqrt(periods), 1e-9);
  EXPECT_DOUBLE_EQ(result.max_drawdown, 0.2);
  EXPECT_NEAR(*result.calmar, 0, 1e-12);
  EXPECT_DOUBLE_EQ(result.winning_days, 0.5);
  // One more day up: a quarter gained over 21 calendar days.
  record.push_back({record.back().day + std::chrono::days{1}, d("125000")});
  marks.push_back(d("125000"));
  result = performance(d("100000"), record, marks);
  EXPECT_NEAR(*result.annual_return, std::pow(1.25, 365.25 / 21) - 1, 1e-6);
  EXPECT_NEAR(*result.calmar, *result.annual_return / 0.2, 1e-9);
}
TEST(Performance, UndefinedFiguresStayAbsentAndInvalidRecordsAreRefused) {
  std::vector<const char*> flat(25, "100000");
  auto record = record_of(flat);
  std::vector<Decimal> marks(25, d("100000"));
  auto result = performance(d("100000"), record, marks);
  // Nothing varied and nothing fell: a volatility of zero, no ratio to it.
  EXPECT_DOUBLE_EQ(*result.annual_volatility, 0);
  EXPECT_FALSE(result.sharpe || result.calmar);
  EXPECT_DOUBLE_EQ(result.winning_days, 0);
  // An account that lost everything has no yearly rate.
  record[10].equity = d("0");
  marks[10] = d("0");
  result = performance(d("100000"), record, marks);
  EXPECT_DOUBLE_EQ(result.max_drawdown, 1);
  EXPECT_FALSE(result.annual_return || result.sharpe);
  EXPECT_THROW(performance(d("0"), record, marks), std::invalid_argument);
  EXPECT_THROW(performance(d("100000"), {}, marks), std::invalid_argument);
  std::swap(record[3].day, record[4].day);
  EXPECT_THROW(performance(d("100000"), record, marks), std::invalid_argument);
}
TEST(Trades, ATradeIsOneContractsPositionFromFlatToFlatAgain) {
  const std::vector<Decimal> multipliers{d("10"), d("5")};
  const std::vector<TradedFill> fills{
      // The first contract: two lots bought and sold in two fills, a profit
      // of (110 + 120 - 200) x 10.
      {0, true, d("2"), d("100")},
      // The second contract opens meanwhile and never closes: no trade.
      {1, true, d("1"), d("50")},
      {0, false, d("1"), d("110")},
      {0, false, d("1"), d("120")},
      // Short one lot at 120 and covered at 125: a loss of 5 x 10.
      {0, false, d("1"), d("120")},
      {0, true, d("1"), d("125")},
      // In and out at one price: a trade that is neither a win nor a loss.
      {0, true, d("3"), d("130")},
      {0, false, d("3"), d("130")},
  };
  const auto result = trades(fills, multipliers);
  EXPECT_EQ(result.count, 3U);
  EXPECT_EQ(result.winning, 1U);
  EXPECT_EQ(result.losing, 1U);
  EXPECT_DOUBLE_EQ(*result.average_win, 300);
  EXPECT_DOUBLE_EQ(*result.average_loss, -50);
  EXPECT_DOUBLE_EQ(*result.payoff, 6);
}
TEST(Trades, AveragesNeedTradesOfTheirKindAndAFillNeverCrossesFlat) {
  const std::vector<Decimal> multipliers{d("10")};
  // Nothing closed: no trade and nothing to average.
  const std::vector<TradedFill> open{{0, true, d("1"), d("100")}};
  auto result = trades(open, multipliers);
  EXPECT_EQ(result.count, 0U);
  EXPECT_FALSE(result.average_win || result.average_loss || result.payoff);
  // Two wins, of 10 and of 30: an average, and no ratio without a loss.
  const std::vector<TradedFill> wins{{0, true, d("1"), d("100")},
                                     {0, false, d("1"), d("101")},
                                     {0, false, d("2"), d("103")},
                                     {0, true, d("2"), d("101.5")}};
  result = trades(wins, multipliers);
  EXPECT_EQ(result.winning, 2U);
  EXPECT_DOUBLE_EQ(*result.average_win, 20);
  EXPECT_FALSE(result.average_loss || result.payoff);
  // Selling two while holding one would close and open in one fill.
  const std::vector<TradedFill> crossing{{0, true, d("1"), d("100")}, {0, false, d("2"), d("101")}};
  EXPECT_THROW(trades(crossing, multipliers), std::invalid_argument);
}
