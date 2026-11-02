#include "strategy_bars.hpp"
#include <gtest/gtest.h>

using namespace asterion;
using namespace asterion::trading;
namespace wire = strategy::proto;

namespace {

wire::Instance configuration(const char* period, bool pair = false)
{
    wire::Instance result;
    result.set_period(period);
    result.add_contracts("SHFE/rb2610");
    if (pair)
        result.add_contracts("SHFE/cu2610");
    return result;
}

MarketQuote tick(
    std::int64_t time,
    int volume,
    const char* price = "100",
    const char* day = "20260928",
    const char* symbol = "rb2610")
{
    MarketQuote result;
    result.instrument = {"SHFE", symbol};
    result.source_ms = result.received_ms = time;
    result.trading_day = day;
    result.last = Decimal::parse(price);
    result.open_interest = Decimal::parse("1000");
    result.volume = volume;
    return result;
}

constexpr std::int64_t start = 1790557200000; // an exact minute boundary

} // namespace

TEST(StrategyBars, SkipsInitialPartialPeriodAndSealsOnlyOnNewObservation)
{
    const auto begin = start / 300000 * 300000;
    StrategyBars bars(configuration("5m"), begin + 150000);
    EXPECT_TRUE(bars.observe(tick(begin + 150000, 10)).empty());
    EXPECT_TRUE(bars.observe(tick(begin + 300000, 12, "101.25")).empty());
    EXPECT_TRUE(bars.observe(tick(begin + 350000, 16, "99.5")).empty());
    EXPECT_TRUE(bars.observe(tick(begin + 590000, 17, "102")).empty());
    const auto closed = bars.observe(tick(begin + 600000, 19, "103"));
    ASSERT_EQ(closed.size(), 1);
    ASSERT_EQ(closed[0].bars_size(), 1);
    const auto& bar = closed[0].bars(0);
    EXPECT_EQ(bar.end_ms(), begin + 600000);
    EXPECT_EQ(bar.open_units(), Decimal::parse("101.25").raw());
    EXPECT_EQ(bar.low_units(), Decimal::parse("99.5").raw());
    EXPECT_EQ(bar.high_units(), Decimal::parse("102").raw());
    EXPECT_EQ(bar.close_units(), Decimal::parse("102").raw());
    EXPECT_EQ(bar.volume(), 7);
    EXPECT_EQ(closed[0].sequence(), 1);
    // Silence is not a new bar. A session break does not create empty slots.
    const auto later = bars.observe(tick(begin + 7200000, 20, "104"));
    ASSERT_EQ(later.size(), 1);
    EXPECT_EQ(later[0].bars(0).end_ms(), begin + 900000);
}

TEST(StrategyBars, DailyWindowStartsAtObservedTradingDayTransition)
{
    StrategyBars bars(configuration("1d"), start);
    EXPECT_TRUE(bars.observe(tick(start, 500)).empty());
    EXPECT_TRUE(bars.observe(tick(start + 86400000, 2, "101", "20260929")).empty());
    EXPECT_TRUE(bars.observe(tick(start + 86460000, 7, "103", "20260929")).empty());
    const auto closed = bars.observe(tick(start + 172800000, 1, "102", "20260930"));
    ASSERT_EQ(closed.size(), 1);
    const auto& bar = closed[0].bars(0);
    EXPECT_EQ(bar.trading_day(), "2026-09-29");
    EXPECT_EQ(bar.open_units(), Decimal::parse("101").raw());
    EXPECT_EQ(bar.close_units(), Decimal::parse("103").raw());
    EXPECT_EQ(bar.volume(), 7);
    StrategyBars restarted(configuration("1d"), start + 86460000);
    EXPECT_TRUE(restarted.observe(tick(start + 86460000, 7, "103", "20260929")).empty());
    EXPECT_TRUE(restarted.observe(tick(start + 172800000, 1, "102", "20260930")).empty());
}

TEST(StrategyBars, BatchesContractsOnceSkipsRepeatedOrStaleQuotesAndRejectsResets)
{
    StrategyBars bars(configuration("1m", true), start);
    for (const auto* symbol : {"rb2610", "cu2610"})
        EXPECT_TRUE(bars.observe(tick(start + 1000, 10, "100", "20260928", symbol)).empty());
    for (const auto* symbol : {"rb2610", "cu2610"})
        EXPECT_TRUE(bars.observe(tick(start + 60000, 11, "101", "20260928", symbol)).empty());
    EXPECT_TRUE(bars.observe(tick(start + 120000, 12)).empty());
    const auto closed = bars.observe(tick(start + 120000, 12, "102", "20260928", "cu2610"));
    ASSERT_EQ(closed.size(), 1);
    ASSERT_EQ(closed[0].bars_size(), 2);
    EXPECT_EQ(closed[0].bars(0).contract(), "SHFE/rb2610");
    EXPECT_EQ(closed[0].bars(1).contract(), "SHFE/cu2610");
    // A reconnect may repeat an older snapshot; it is skipped, not an error.
    EXPECT_TRUE(bars.observe(tick(start + 119000, 13)).empty());
    auto stale = tick(start + 121000, 13);
    stale.received_ms += 10001;
    EXPECT_TRUE(bars.observe(stale).empty());
    EXPECT_THROW(bars.observe(tick(start + 121000, 1)), std::runtime_error);
    EXPECT_THROW(bars.observe(tick(start + 121000, 1, "100", "20260929")), std::runtime_error);
}

TEST(StrategyBars, AResumedWindowContinuesTheRecordedSequence)
{
    StrategyBars bars(configuration("1m", false), start, 41);
    EXPECT_TRUE(bars.observe(tick(start + 1000, 10)).empty());
    EXPECT_TRUE(bars.observe(tick(start + 60000, 11)).empty());
    const auto closed = bars.observe(tick(start + 120000, 12));
    ASSERT_EQ(closed.size(), 1);
    EXPECT_EQ(closed[0].sequence(), 42U);
}

TEST(StrategyBars, ASubscriptionCacheCannotTurnAPreStartMinuteIntoACompleteBar)
{
    StrategyBars bars(configuration("1m"), start + 5000);
    // Both cached snapshots are within the freshness tolerance but precede the
    // instance; crossing their boundary must not complete its warmup input.
    auto cached = tick(start - 1000, 10);
    cached.received_ms = start + 5000;
    EXPECT_TRUE(bars.observe(cached).empty());
    cached = tick(start, 11);
    cached.received_ms = start + 5000;
    EXPECT_TRUE(bars.observe(cached).empty());
    EXPECT_TRUE(bars.observe(tick(start + 6000, 12)).empty());
    EXPECT_TRUE(bars.observe(tick(start + 60000, 13)).empty());
    const auto closed = bars.observe(tick(start + 120000, 14));
    ASSERT_EQ(closed.size(), 1);
    EXPECT_EQ(closed[0].bars(0).end_ms(), start + 120000);
    EXPECT_EQ(closed[0].bars(0).volume(), 1);
}
