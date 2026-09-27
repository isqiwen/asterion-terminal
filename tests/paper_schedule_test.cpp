#include "replay_schedule.hpp"
#include <gtest/gtest.h>
using namespace asterion;
namespace {
Decimal d(const char* text) {
  return Decimal::parse(text);
}
Instrument contract() {
  return {{"SHFE", "rb2610"}, AssetClass::futures, "CNY", d("1"), d("1"), d("10")};
}
std::vector<SettlementDay> days() {
  return {{TradingDaySchedule("2026-09-25", {{10, 20}, {30, 40}, {50, 60}}), d("105"), "fixture",
           "fixture"},
          {TradingDaySchedule("2026-09-28", {{100, 110}}), d("110"), "fixture", "fixture"}};
}
std::vector<TradeTick> ticks() {
  std::vector<TradeTick> result;
  for (auto time : {10, 10, 19, 50, 100, 109})
    result.push_back({contract().id, time, d("100"), d("1")});
  return result;
}
} // namespace
TEST(PaperReplaySchedule, ExactEdgesEmptySessionsAndDuplicateTimesPreserveEventIdentity) {
  const PaperReplaySchedule schedule(contract(), ticks(), days());
  ASSERT_EQ(schedule.size(), 6U);
  EXPECT_FALSE(schedule.event(0).session_end);
  EXPECT_FALSE(schedule.event(1).session_end);
  EXPECT_TRUE(schedule.event(2).session_end);
  EXPECT_FALSE(schedule.event(2).day_end);
  EXPECT_EQ(schedule.event(3).session, 2U);
  EXPECT_TRUE(schedule.event(3).day_end);
  EXPECT_EQ(schedule.day(0).schedule.sessions().back().end_ns, 60);
  EXPECT_EQ(schedule.day(0).settlement_price, d("105"));
  EXPECT_FALSE(schedule.event(4).session_end);
  EXPECT_EQ(schedule.event(4).day, 1U);
  EXPECT_TRUE(schedule.event(5).session_end);
  EXPECT_TRUE(schedule.event(5).day_end);
  EXPECT_THROW(schedule.event(6), std::out_of_range);
}
TEST(PaperReplaySchedule, RejectsGapsEndExclusiveReverseTicksAndUnobservedDays) {
  for (auto time : {9, 20, 40, 60, 99, 110}) {
    std::vector<TradeTick> input{{contract().id, time, d("100"), d("1")}};
    EXPECT_THROW(PaperReplaySchedule(contract(), input, days()), std::invalid_argument);
  }
  auto input = ticks();
  std::swap(input[2], input[3]);
  EXPECT_THROW(PaperReplaySchedule(contract(), input, days()), std::invalid_argument);
  input = ticks();
  input.resize(4);
  EXPECT_THROW(PaperReplaySchedule(contract(), input, days()), std::invalid_argument);
  input = ticks();
  input[2].instrument.symbol = "rb2611";
  EXPECT_THROW(PaperReplaySchedule(contract(), input, days()), std::invalid_argument);
}
TEST(PaperReplaySchedule, SimulatorRejectsUnsupportedPricesAndOverlappingDaysWithoutChangingInput) {
  auto calendar = days();
  calendar[0].settlement_price = d("-1");
  EXPECT_THROW(PaperReplaySchedule(contract(), ticks(), calendar), std::invalid_argument);
  EXPECT_EQ(calendar[0].settlement_price, d("-1"));
  calendar[0].settlement_price = d("100.5");
  EXPECT_THROW(PaperReplaySchedule(contract(), ticks(), calendar), std::invalid_argument);
  calendar = days();
  calendar[1].schedule = TradingDaySchedule("2026-09-28", {{59, 110}});
  EXPECT_THROW(PaperReplaySchedule(contract(), ticks(), calendar), std::invalid_argument);
  calendar = days();
  std::swap(calendar[0], calendar[1]);
  EXPECT_THROW(PaperReplaySchedule(contract(), ticks(), calendar), std::invalid_argument);
}
