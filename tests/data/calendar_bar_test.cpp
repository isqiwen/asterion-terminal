#include "calendar_bars.hpp"
#include <asterion/domain/trading_days.hpp>
#include <gtest/gtest.h>
using namespace asterion;
using namespace asterion::chart_indicators;
namespace {
DailyCandle sample(const char* date) {
  return {parse_trading_date(date), Decimal::parse("100"), Decimal::parse("110"),
          Decimal::parse("90"),     Decimal::parse("105"), Decimal::parse("2")};
}
} // namespace
TEST(CalendarBars, IsoWeekCrossesYearAndKeepsFirstOpenLastCloseAndExactSums) {
  auto a = sample("2020-12-31"), b = sample("2021-01-01"), c = sample("2021-01-04");
  b.high = Decimal::parse("120");
  b.low = Decimal::parse("80");
  b.close = Decimal::parse("115");
  b.volume = Decimal::parse("2.5");
  const auto result = calendar_bars(std::vector{a, b, c}, CalendarPeriod::week);
  ASSERT_EQ(result.size(), 2);
  const auto& first = result.front();
  EXPECT_EQ(format_trading_date(first.period_begin), "2020-12-28");
  EXPECT_EQ(format_trading_date(first.period_end), "2021-01-03");
  EXPECT_EQ(format_trading_date(first.first_day), "2020-12-31");
  EXPECT_EQ(format_trading_date(first.bar.trading_day), "2021-01-01");
  EXPECT_EQ(first.source_rows, 2);
  EXPECT_EQ(first.bar.open, a.open);
  EXPECT_EQ(first.bar.high, b.high);
  EXPECT_EQ(first.bar.low, b.low);
  EXPECT_EQ(first.bar.close, b.close);
  EXPECT_EQ(first.bar.volume.str(), "4.5");
}
TEST(CalendarBars, CalendarMonthsKeepLeapDayAndDoNotInventMissingDatesOrMonths) {
  const auto result =
      calendar_bars(std::vector{sample("2024-02-01"), sample("2024-02-29"), sample("2024-04-02")},
                    CalendarPeriod::month);
  ASSERT_EQ(result.size(), 2);
  EXPECT_EQ(format_trading_date(result[0].period_end), "2024-02-29");
  EXPECT_EQ(result[0].source_rows, 2);
  EXPECT_EQ(format_trading_date(result[1].period_begin), "2024-04-01");
  EXPECT_EQ(format_trading_date(result[1].period_end), "2024-04-30");
  EXPECT_EQ(format_trading_date(result[1].bar.trading_day), "2024-04-02");
  EXPECT_EQ(result[1].source_rows, 1);
  EXPECT_TRUE(calendar_bars({}, CalendarPeriod::month).empty());
}
TEST(CalendarBars, RejectsAmbiguousDatesInvalidInputAndOverflowWithoutPartialOutput) {
  auto a = sample("2024-02-01"), b = sample("2024-02-02");
  EXPECT_THROW(calendar_bars(std::vector{a, a}, CalendarPeriod::week), std::invalid_argument);
  EXPECT_THROW(calendar_bars(std::vector{b, a}, CalendarPeriod::week), std::invalid_argument);
  EXPECT_THROW(calendar_bars({}, static_cast<CalendarPeriod>(42)), std::invalid_argument);
  b.volume = Decimal::parse("-1");
  EXPECT_THROW(calendar_bars(std::vector{a, b}, CalendarPeriod::week), std::invalid_argument);
  b.volume = Decimal::parse("1");
  b.close = Decimal::parse("111");
  EXPECT_THROW(calendar_bars(std::vector{a, b}, CalendarPeriod::week), std::invalid_argument);
  b.close = Decimal::parse("105");
  a.volume = Decimal::from_raw(INT64_MAX);
  EXPECT_THROW(calendar_bars(std::vector{a, b}, CalendarPeriod::week), std::overflow_error);
}

TEST(CalendarBars, CalendarQuartersAndYearsRespectBoundariesAndMissingPeriods) {
  const std::vector bars{sample("2023-12-31"), sample("2024-02-29"), sample("2024-03-31"),
                         sample("2024-04-01"), sample("2024-12-30"), sample("2026-01-02")};
  const auto quarters = calendar_bars(bars, CalendarPeriod::quarter);
  ASSERT_EQ(quarters.size(), 5);
  EXPECT_EQ(format_trading_date(quarters[1].period_begin), "2024-01-01");
  EXPECT_EQ(format_trading_date(quarters[1].period_end), "2024-03-31");
  EXPECT_EQ(quarters[1].source_rows, 2);
  EXPECT_EQ(quarters[1].bar.volume.str(), "4");
  EXPECT_EQ(format_trading_date(quarters[2].period_begin), "2024-04-01");
  EXPECT_EQ(format_trading_date(quarters[2].period_end), "2024-06-30");
  EXPECT_EQ(format_trading_date(quarters[3].bar.trading_day), "2024-12-30");
  const auto years = calendar_bars(bars, CalendarPeriod::year);
  ASSERT_EQ(years.size(), 3);
  EXPECT_EQ(format_trading_date(years[1].period_begin), "2024-01-01");
  EXPECT_EQ(format_trading_date(years[1].period_end), "2024-12-31");
  EXPECT_EQ(years[1].source_rows, 4);
  EXPECT_EQ(years[1].bar.volume.str(), "8");
  EXPECT_EQ(format_trading_date(years[2].bar.trading_day), "2026-01-02");
}
