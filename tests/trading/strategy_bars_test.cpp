#include "strategy_bars.hpp"
#include <asterion/futures/intraday_bars.hpp>
#include <gtest/gtest.h>

using namespace asterion;
using namespace asterion::trading;
namespace wire = ::strategy;

namespace {

wire::Instance configuration(::common::Period period, bool pair = false)
{
    wire::Instance result;
    result.set_period(period);
    auto* rebar = result.add_instruments();
    rebar->set_exchange("SHFE");
    rebar->set_symbol("rb2610");
    if (pair) {
        auto* copper = result.add_instruments();
        copper->set_exchange("SHFE");
        copper->set_symbol("cu2610");
    }
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
    StrategyBars bars(configuration(::common::PERIOD_MINUTES_5), begin + 150000);
    EXPECT_TRUE(bars.observe(tick(begin + 150000, 10)).empty());
    EXPECT_TRUE(bars.observe(tick(begin + 300000, 12, "101.25")).empty());
    EXPECT_TRUE(bars.observe(tick(begin + 350000, 16, "99.5")).empty());
    EXPECT_TRUE(bars.observe(tick(begin + 590000, 17, "102")).empty());
    const auto closed = bars.observe(tick(begin + 600000, 19, "103"));
    ASSERT_EQ(closed.size(), 1);
    ASSERT_EQ(closed[0].bars_size(), 1);
    const auto& bar = closed[0].bars(0);
    EXPECT_EQ(bar.end_ms(), begin + 600000);
    EXPECT_EQ(bar.open().text(), Decimal::parse("101.25").str());
    EXPECT_EQ(bar.low().text(), Decimal::parse("99.5").str());
    EXPECT_EQ(bar.high().text(), Decimal::parse("102").str());
    EXPECT_EQ(bar.close().text(), Decimal::parse("102").str());
    EXPECT_EQ(bar.volume(), 7);
    EXPECT_EQ(closed[0].sequence(), 1);
    // Silence is not a new bar. A session break does not create empty slots.
    const auto later = bars.observe(tick(begin + 7200000, 20, "104"));
    ASSERT_EQ(later.size(), 1);
    EXPECT_EQ(later[0].bars(0).end_ms(), begin + 900000);
}

TEST(StrategyBars, DailyWindowStartsAtObservedTradingDayTransition)
{
    StrategyBars bars(configuration(::common::PERIOD_DAY), start);
    EXPECT_TRUE(bars.observe(tick(start, 500)).empty());
    EXPECT_TRUE(bars.observe(tick(start + 86400000, 2, "101", "20260929")).empty());
    EXPECT_TRUE(bars.observe(tick(start + 86460000, 7, "103", "20260929")).empty());
    const auto closed = bars.observe(tick(start + 172800000, 1, "102", "20260930"));
    ASSERT_EQ(closed.size(), 1);
    const auto& bar = closed[0].bars(0);
    EXPECT_EQ(bar.trading_day(), "2026-09-29");
    // 15:00 Beijing, as a backtest labels the day.
    EXPECT_EQ(
        bar.end_ms(),
        std::chrono::duration_cast<std::chrono::milliseconds>(
            (std::chrono::sys_days(std::chrono::year(2026) / 9 / 29) + std::chrono::hours(7))
                .time_since_epoch())
            .count());
    EXPECT_EQ(bar.open().text(), Decimal::parse("101").str());
    EXPECT_EQ(bar.close().text(), Decimal::parse("103").str());
    EXPECT_EQ(bar.volume(), 7);
    StrategyBars restarted(configuration(::common::PERIOD_DAY), start + 86460000);
    EXPECT_TRUE(restarted.observe(tick(start + 86460000, 7, "103", "20260929")).empty());
    EXPECT_TRUE(restarted.observe(tick(start + 172800000, 1, "102", "20260930")).empty());
}

TEST(StrategyBars, BatchesContractsOnceSkipsRepeatedOrStaleQuotesAndRejectsResets)
{
    StrategyBars bars(configuration(::common::PERIOD_MINUTES_1, true), start);
    for (const auto* symbol : {"rb2610", "cu2610"})
        EXPECT_TRUE(bars.observe(tick(start + 1000, 10, "100", "20260928", symbol)).empty());
    for (const auto* symbol : {"rb2610", "cu2610"})
        EXPECT_TRUE(bars.observe(tick(start + 60000, 11, "101", "20260928", symbol)).empty());
    EXPECT_TRUE(bars.observe(tick(start + 120000, 12)).empty());
    const auto closed = bars.observe(tick(start + 120000, 12, "102", "20260928", "cu2610"));
    ASSERT_EQ(closed.size(), 1);
    ASSERT_EQ(closed[0].bars_size(), 2);
    EXPECT_EQ(closed[0].bars(0).instrument().symbol(), "rb2610");
    EXPECT_EQ(closed[0].bars(1).instrument().symbol(), "cu2610");
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
    StrategyBars bars(configuration(::common::PERIOD_MINUTES_1, false), start, 41);
    EXPECT_TRUE(bars.observe(tick(start + 1000, 10)).empty());
    EXPECT_TRUE(bars.observe(tick(start + 60000, 11)).empty());
    const auto closed = bars.observe(tick(start + 120000, 12));
    ASSERT_EQ(closed.size(), 1);
    EXPECT_EQ(closed[0].sequence(), 42U);
}

TEST(StrategyBars, ASubscriptionCacheCannotTurnAPreStartMinuteIntoACompleteBar)
{
    StrategyBars bars(configuration(::common::PERIOD_MINUTES_1), start + 5000);
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

// Research compares Tushare's minutes with Warehouse's recorded ones; that says
// something about live trading only if a strategy's one-minute bars are the
// recorded minutes. The same quotes, across a new trading day whose first
// quote carries the opening auction's volume, give the same bars.
TEST(StrategyBars, OneMinuteBarsAreTheMinutesMarketRecords)
{
    const std::vector<MarketQuote> quotes{
        tick(start + 30000, 10, "100"),
        tick(start + 60000, 12, "101"),
        tick(start + 90000, 15, "99"),
        tick(start + 120000, 20, "102"),
        tick(start + 150000, 21, "103"),
        // The next trading day opens: its counter restarts with the auction's volume.
        tick(start + 3600000, 7, "104", "20260929"),
        tick(start + 3630000, 9, "105", "20260929"),
        tick(start + 3660000, 13, "103", "20260929"),
        tick(start + 3720000, 14, "106", "20260929"),
    };
    StrategyBars live(configuration(::common::PERIOD_MINUTES_1), start);
    IntradayBars recorded;
    std::map<std::int64_t, MinuteBar> minutes;
    std::vector<wire::Bar> delivered;
    for (const auto& quote : quotes) {
        for (const auto& batch : live.observe(quote))
            for (const auto& bar : batch.bars())
                delivered.push_back(bar);
        recorded.observe(quote);
        const auto series = recorded.series(quote.instrument);
        for (const auto& bar : series->bars)
            minutes[bar.start_ms] = bar;
    }
    ASSERT_EQ(delivered.size(), 4U);
    for (const auto& bar : delivered) {
        SCOPED_TRACE(bar.end_ms());
        const auto& minute = minutes.at(bar.end_ms() - 60000);
        EXPECT_FALSE(minute.partial);
        EXPECT_EQ(bar.open().text(), minute.open.str());
        EXPECT_EQ(bar.high().text(), minute.high.str());
        EXPECT_EQ(bar.low().text(), minute.low.str());
        EXPECT_EQ(bar.close().text(), minute.close.str());
        EXPECT_EQ(bar.volume(), minute.volume);
    }
}
