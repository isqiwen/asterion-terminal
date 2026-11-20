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

TEST(StrategyBars, ADayEndsWhenALaterTradingDayIsObserved)
{
    StrategyBars bars(configuration(::common::PERIOD_DAY, true), start);
    const auto day = start + 86400000; // 09:00 on 2026-09-29
    const auto observe = [&](std::int64_t time,
                             int volume,
                             const char* price,
                             const char* date,
                             const char* symbol) {
        return bars.observe(tick(time, volume, price, date, symbol));
    };
    for (const auto* symbol : {"rb2610", "cu2610"}) {
        EXPECT_TRUE(observe(start, 1, "100", "20260928", symbol).empty());
        EXPECT_TRUE(observe(day, 2, "101", "20260929", symbol).empty());
    }
    // Quotes after 15:00 of the same trading day still belong to it.
    EXPECT_TRUE(observe(day + 22500000, 5, "104", "20260929", "rb2610").empty());
    // cu opens the night session of the next trading day; rb stays silent.
    const auto night = day + 43200000;
    EXPECT_TRUE(observe(night, 1, "99", "20260930", "cu2610").empty());
    const auto closed = observe(night + 10000, 2, "98", "20260930", "cu2610");
    ASSERT_EQ(closed.size(), 1);
    ASSERT_EQ(closed[0].bars_size(), 2);
    EXPECT_EQ(closed[0].bars(0).trading_day(), "2026-09-29");
    EXPECT_EQ(closed[0].bars(0).close().text(), Decimal::parse("104").str());
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
    // A quote more than 2 s ahead of the host clock is skipped rather than killing the instance.
    auto ahead = tick(start + 121000, 13);
    ahead.received_ms -= 2500;
    EXPECT_TRUE(bars.observe(ahead).empty());
    // A falling cumulative volume counter marks that minute partial rather than killing the instance.
    EXPECT_TRUE(bars.observe(tick(start + 121000, 1)).empty());
    EXPECT_TRUE(bars.observe(tick(start + 121000, 13, "102", "20260928", "cu2610")).empty());
    // Crossing to 09:03 delivers cu's complete 09:02..09:03 bar while skipping rb's reset bar.
    EXPECT_TRUE(bars.observe(tick(start + 180000, 2, "103", "20260928", "rb2610")).empty());
    const auto after_reset = bars.observe(tick(start + 180000, 14, "103", "20260928", "cu2610"));
    ASSERT_EQ(after_reset.size(), 1U);
    ASSERT_EQ(after_reset[0].bars_size(), 1);
    EXPECT_EQ(after_reset[0].bars(0).instrument().symbol(), "cu2610");
    EXPECT_THROW(bars.observe(tick(start + 181000, 1, "100", "20260929")), std::runtime_error);
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
    // The next trading day opens: its counter restarts with the auction's volume.
    for (const auto& quote : quotes) {
        for (const auto& batch : live.observe(quote))
            for (const auto& bar : batch.bars())
                delivered.push_back(bar);
        recorded.observe(quote);
        const auto series = recorded.series(quote.instrument);
        for (const auto& bar : series->bars)
            minutes[bar.start_ms] = bar;
    }
    auto untraded = tick(start + 86400000 - 30000, 0, "104", "20260930");
    untraded.last.reset();
    EXPECT_TRUE(live.observe(untraded).empty());
    for (const auto& quote :
        {tick(start + 86400000 - 15000, 3, "104", "20260930"), // 08:59:45 auction -> 09:00
            tick(start + 86400000 + 30000, 9, "105", "20260930"),
            tick(start + 86400000 + 6 * 3600000LL - 15000, 13, "103", "20260930"), // 14:59:45
            tick(start + 86400000 + 6 * 3600000LL, 16, "106", "20260930"), // 15:00:00 -> 14:59
            tick(start + 2 * 86400000, 1, "107", "20261001")}) {
        for (const auto& batch : live.observe(quote))
            for (const auto& bar : batch.bars())
                delivered.push_back(bar);
        recorded.observe(quote);
        const auto series = recorded.series(quote.instrument);
        for (const auto& bar : series->bars)
            minutes[bar.start_ms] = bar;
    }
    ASSERT_EQ(delivered.size(), 7U);
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

TEST(StrategyBars, AHaltedContractNeitherBlocksNorRepeatsTheOthers)
{
    StrategyBars bars(configuration(::common::PERIOD_MINUTES_1, true), start);
    std::vector<wire::Bars> steps;
    const auto observe = [&](std::int64_t time, int volume, const char* symbol) {
        for (auto& step : bars.observe(tick(time, volume, "100", "20260928", symbol)))
            steps.push_back(std::move(step));
    };
    for (const auto* symbol : {"rb2610", "cu2610"}) {
        observe(start + 1000, 10, symbol);
        observe(start + 60000, 11, symbol);
    }
    // cu halts. rb's next minute alone delivers nothing until no cu quote of it can arrive.
    observe(start + 120000, 12, "rb2610");
    EXPECT_TRUE(steps.empty());
    observe(start + 130000, 13, "rb2610");
    ASSERT_EQ(steps.size(), 1);
    EXPECT_EQ(steps[0].bars_size(), 2);
    // rb goes on alone for longer than the old backlog allowed.
    for (int minute = 3; minute < 300; ++minute)
        observe(start + minute * 60000 + 1000, 13 + minute, "rb2610");
    // cu resumes: its quote of a minute already delivered is not delivered again.
    observe(start + 125000, 15, "cu2610");
    observe(start + 299 * 60000 + 1000, 16, "cu2610");
    observe(start + 300 * 60000, 400, "rb2610");
    observe(start + 300 * 60000, 17, "cu2610");
    ASSERT_EQ(steps.size(), 299);
    std::int64_t previous = 0;
    for (std::size_t n = 0; n < steps.size(); ++n) {
        EXPECT_EQ(steps[n].sequence(), n + 1);
        // One time per step, each once, as a backtest groups bars by time.
        for (const auto& bar : steps[n].bars())
            EXPECT_EQ(bar.end_ms(), steps[n].bars(0).end_ms());
        EXPECT_GT(steps[n].bars(0).end_ms(), previous);
        previous = steps[n].bars(0).end_ms();
        EXPECT_EQ(steps[n].bars_size(), n == 0 || n + 1 == steps.size() ? 2 : 1);
    }
    // The late quote's volume goes to cu's next bar.
    EXPECT_EQ(steps.back().bars(1).volume(), 5);
}

TEST(StrategyBars, SessionBoundariesAvoidPhantomSubsessionClosingBarsAndInterruptSkipsPartialBars)
{
    constexpr std::int64_t day0 = 1790524800000LL; // 2026-09-28 00:00 Beijing
    const auto bj = [&](int h, int m, int s = 0, int ms = 0) {
        return day0 + ((h * 60LL + m) * 60LL + s) * 1000LL + ms;
    };
    wire::Instance config;
    config.set_period(::common::PERIOD_MINUTES_5);
    auto* rb = config.add_instruments();
    rb->set_exchange("SHFE");
    rb->set_symbol("rb2610");
    auto* bond = config.add_instruments();
    bond->set_exchange("CFFEX");
    bond->set_symbol("T2612");

    const auto quote =
        [](const char* venue, const char* symbol, std::int64_t t, int vol, const char* px) {
            MarketQuote q;
            q.instrument = {venue, symbol};
            q.source_ms = q.received_ms = t;
            q.trading_day = "20260928";
            q.last = Decimal::parse(px);
            q.volume = vol;
            return q;
        };
    StrategyBars five(config, bj(9, 25));
    EXPECT_TRUE(five.observe(quote("SHFE", "rb2610", bj(9, 26, 0), 100, "3200")).empty());
    EXPECT_TRUE(five.observe(quote("CFFEX", "T2612", bj(9, 29, 30), 5, "105.1")).empty());
    EXPECT_TRUE(five.observe(quote("CFFEX", "T2612", bj(9, 35, 0), 8, "105.2")).empty());
    const auto bond_closed = five.observe(quote("CFFEX", "T2612", bj(9, 40, 11), 12, "105.3"));
    ASSERT_EQ(bond_closed.size(), 1U);
    EXPECT_EQ(bond_closed[0].bars(0).instrument().symbol(), "T2612");
    EXPECT_EQ(bond_closed[0].bars(0).end_ms(), bj(9, 40));
    EXPECT_FALSE(bond_closed[0].bars(0).has_open_interest());
    // 10:15:00.500 folds into 10:10..10:15 instead of spawning a 1-tick 10:15..10:20 bar.
    EXPECT_EQ(five.observe(quote("SHFE", "rb2610", bj(10, 10, 5), 110, "3201")).size(), 1U);
    EXPECT_TRUE(five.observe(quote("SHFE", "rb2610", bj(10, 15, 0, 500), 118, "3205")).empty());
    const auto resumed = five.observe(quote("SHFE", "rb2610", bj(10, 30, 1), 120, "3206"));
    ASSERT_EQ(resumed.size(), 1U);
    EXPECT_EQ(resumed[0].bars(0).instrument().symbol(), "rb2610");
    EXPECT_EQ(resumed[0].bars(0).end_ms(), bj(10, 15));
    EXPECT_EQ(resumed[0].bars(0).close().text(), "3205");
    EXPECT_EQ(resumed[0].bars(0).volume(), 18);

    // Interrupt marks the in-progress bar partial and drops unseen disconnect volume on reconnect.
    StrategyBars one(configuration(::common::PERIOD_MINUTES_1), bj(9, 0));
    EXPECT_TRUE(one.observe(tick(bj(9, 0, 10), 10, "100")).empty());
    EXPECT_TRUE(one.observe(tick(bj(9, 1, 5), 15, "101")).empty());
    one.interrupt();
    EXPECT_TRUE(one.observe(tick(bj(9, 2, 10), 515, "102")).empty());
    EXPECT_TRUE(one.observe(tick(bj(9, 3, 2), 518, "103")).empty());
    EXPECT_TRUE(one.observe(tick(bj(9, 3, 40), 522, "104")).empty());
    const auto clean = one.observe(tick(bj(9, 4, 1), 525, "105"));
    ASSERT_EQ(clean.size(), 1U);
    EXPECT_EQ(clean[0].bars(0).end_ms(), bj(9, 4));
    EXPECT_EQ(clean[0].bars(0).open().text(), "103");
    EXPECT_EQ(clean[0].bars(0).close().text(), "104");
    EXPECT_EQ(clean[0].bars(0).volume(), 7);
}

TEST(StrategyBars, ManualClockDrivesStartStalenessAndSkewChecks)
{
    using namespace std::chrono_literals;
    auto clock = std::make_shared<ManualClock>(start * 1'000'000);
    StrategyBars bars(configuration(::common::PERIOD_MINUTES_1), clock);

    auto arrival_from_clock = [&](std::int64_t source_ms, int volume, const char* price) {
        auto q = tick(source_ms, volume, price);
        q.received_ms = 0;
        return q;
    };

    // Before start, skewed ahead of clock, or stale against clock are dropped.
    EXPECT_TRUE(bars.observe(arrival_from_clock(start - 1000, 10, "100")).empty());
    EXPECT_TRUE(bars.observe(arrival_from_clock(start + 5000, 11, "101")).empty());

    clock->advance(15s);
    EXPECT_TRUE(bars.observe(arrival_from_clock(start + 1000, 12, "101")).empty());

    // Quotes within the freshness window of the manual clock aggregate normally.
    EXPECT_TRUE(bars.observe(arrival_from_clock(start + 15000, 12, "101")).empty());
    clock->advance(45s);
    EXPECT_TRUE(bars.observe(arrival_from_clock(start + 60000, 15, "102")).empty());
    clock->advance(30s);
    EXPECT_TRUE(bars.observe(arrival_from_clock(start + 90000, 18, "104")).empty());
    clock->advance(30s);
    const auto closed = bars.observe(arrival_from_clock(start + 120000, 20, "105"));
    ASSERT_EQ(closed.size(), 1U);
    EXPECT_EQ(closed[0].bars(0).end_ms(), start + 120000);
    EXPECT_EQ(closed[0].bars(0).open().text(), "102");
    EXPECT_EQ(closed[0].bars(0).high().text(), "104");
    EXPECT_EQ(closed[0].bars(0).close().text(), "104");
    EXPECT_EQ(closed[0].bars(0).volume(), 6);
}
