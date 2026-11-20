#include <asterion/futures/intraday_bars.hpp>
#include <asterion/protocol/market.hpp>
#include <gtest/gtest.h>
using namespace asterion;

namespace {

MarketQuote
quote(std::int64_t ms, const char* last, std::int64_t volume, const char* day = "20260930")
{
    MarketQuote q;
    q.instrument = {"SHFE", "br2611"};
    q.trading_day = day;
    q.source_ms = ms;
    q.last = Decimal::parse(last);
    q.volume = volume;
    q.previous_settlement = Decimal::parse("15390");
    return q;
}

constexpr std::int64_t base = 1'790'000'040'000; // A minute boundary.

} // namespace

TEST(IntradayBars, AggregatesMinutesFromCumulativeVolume)
{
    IntradayBars bars;
    bars.observe(quote(base + 1000, "100", 50));
    bars.observe(quote(base + 20000, "103", 60));
    bars.observe(quote(base + 59000, "99", 65));
    bars.observe(quote(base + 61000, "101", 70));
    const auto series = bars.series({"SHFE", "br2611"});
    ASSERT_TRUE(series);
    EXPECT_EQ(series->first_observation_ms, base + 1000);
    EXPECT_FALSE(series->interrupted);
    ASSERT_EQ(series->bars.size(), 2u);
    const auto& first = series->bars[0];
    EXPECT_EQ(first.start_ms, base);
    EXPECT_EQ(first.open.str(), Decimal::parse("100").str());
    EXPECT_EQ(first.high.str(), Decimal::parse("103").str());
    EXPECT_EQ(first.low.str(), Decimal::parse("99").str());
    EXPECT_EQ(first.close.str(), Decimal::parse("99").str());
    // The first observation of a day is only the cumulative baseline.
    EXPECT_EQ(first.volume, 15);
    EXPECT_EQ(series->bars[1].volume, 5);
    EXPECT_FALSE(bars.series({"SHFE", "ru2701"}));
}

TEST(IntradayBars, OneMinuteChangeNeedsAnObservationAMinuteOld)
{
    IntradayBars bars;
    const InstrumentId id{"SHFE", "br2611"};
    bars.observe(quote(base, "100", 1));
    bars.observe(quote(base + 30000, "101", 2));
    EXPECT_FALSE(bars.change_1m_percent(id));
    bars.observe(quote(base + 60000, "102", 3));
    EXPECT_EQ(bars.change_1m_percent(id)->str(), Decimal::parse("2").str());
    bars.observe(quote(base + 95000, "100.99", 4));
    // Anchor moves to base + 30000 (101), the newest observation >= 60 s old.
    EXPECT_EQ(bars.change_1m_percent(id)->str(), Decimal::parse("-0.00990099").str());
    bars.interrupt();
    EXPECT_FALSE(bars.change_1m_percent(id));
    EXPECT_TRUE(bars.series(id)->interrupted);
}

TEST(IntradayBars, NewTradingDayStartsANewSeries)
{
    IntradayBars bars;
    bars.observe(quote(base, "100", 10));
    bars.interrupt();
    bars.observe(quote(base + 3'600'000, "105", 3, "20261001"));
    // A stale quote from an earlier trading day cannot reset the active day.
    bars.observe(quote(base + 3'605'000, "99", 20, "20260930"));
    const auto series = bars.series({"SHFE", "br2611"});
    EXPECT_EQ(series->trading_day, "20261001");
    EXPECT_FALSE(series->interrupted);
    ASSERT_EQ(series->bars.size(), 1u);
    EXPECT_EQ(series->bars[0].volume, 0);

    constexpr std::int64_t morning = 1790557200000; // 2026-09-28 09:00 Beijing
    bars.observe(quote(morning - 10000, "106", 4, "20261002")); // 08:59:50 auction
    bars.observe(quote(morning + 30000, "107", 9, "20261002"));
    bars.observe(quote(morning + 6 * 3600000LL - 10000, "108", 12, "20261002")); // 14:59:50
    bars.observe(quote(morning + 6 * 3600000LL, "109", 15, "20261002")); // 15:00:00 close
    const auto next = bars.series({"SHFE", "br2611"});
    ASSERT_EQ(next->bars.size(), 2u);
    EXPECT_EQ(next->bars[0].start_ms, morning);
    EXPECT_FALSE(next->bars[0].partial);
    EXPECT_EQ(next->bars[0].open.str(), "106");
    EXPECT_EQ(next->bars[0].close.str(), "107");
    EXPECT_EQ(next->bars[0].volume, 9);
    EXPECT_EQ(next->bars[1].start_ms, morning + 6 * 3600000LL - 60000LL);
    EXPECT_EQ(next->bars[1].close.str(), "109");
    EXPECT_EQ(next->bars[1].volume, 6);
}

TEST(IntradayBars, DelayedAggregationAndInterruptionHaveIndependentRevisions)
{
    IntradayBars bars;
    const InstrumentId id{"SHFE", "br2611"};
    EXPECT_TRUE(bars.changed_after(0).empty());
    bars.observe(quote(base, "100", 1));
    const auto baseline = bars.revision();
    EXPECT_TRUE(bars.changed_after(baseline).empty());
    bars.observe(quote(base + 60000, "102", 2));
    EXPECT_EQ(bars.changed_after(baseline), std::vector<InstrumentId>{id});
    EXPECT_EQ(bars.change_1m_percent(id)->str(), "2");
    const auto aggregated = bars.revision();
    bars.interrupt();
    EXPECT_EQ(bars.changed_after(aggregated), std::vector<InstrumentId>{id});
    EXPECT_FALSE(bars.change_1m_percent(id));
    const auto interrupted = bars.revision();
    bars.interrupt();
    EXPECT_EQ(bars.revision(), interrupted);
    EXPECT_THROW(bars.changed_after(interrupted + 1), std::invalid_argument);
}

TEST(IntradayBars, ProtocolCarriesBarsAndThePreviousSettlement)
{
    IntradayBars bars;
    bars.observe(quote(base, "100", 10));
    const auto encoded = protocol::encode_minutes(*bars.series({"SHFE", "br2611"}));
    EXPECT_EQ(encoded.bars_size(), 1);
    EXPECT_EQ(encoded.previous_settlement().text(), Decimal::parse("15390").str());
}

TEST(IntradayBars, IsolatedReplayDoesNotInflateDuplicatesOrFillMissingMinutes)
{
    IntradayBars bars;
    const InstrumentId id{"SHFE", "br2611"};
    bars.observe(quote(base + 1000, "100", 50));
    bars.observe(quote(base + 20000, "103", 60));
    bars.observe(quote(base + 20000, "103", 60)); // duplicate cumulative quote: no new volume
    bars.observe(quote(base + 61000, "101", 70));
    bars.observe(
        quote(base + 1000, "999", 50)); // earlier minute: never overwrite the current series
    bars.interrupt(); // replay a lost event batch, as market-data/session.cpp does
    bars.observe(quote(base + 241000, "104", 80));
    const auto series = bars.series(id);
    ASSERT_TRUE(series);
    ASSERT_EQ(series->bars.size(), 3);
    EXPECT_TRUE(series->interrupted);
    EXPECT_EQ(series->bars[0].close.str(), "103");
    EXPECT_EQ(series->bars[0].volume, 10);
    EXPECT_EQ(series->bars[1].volume, 10);
    EXPECT_EQ(series->bars[2].volume, 10);
    EXPECT_EQ(series->bars[2].start_ms - series->bars[1].start_ms, 180000);
    EXPECT_EQ(protocol::encode_minutes(*series).bars_size(), 3);
}

TEST(IntradayBars, ProductSessionsFoldAuctionsAndSubsessionClosesWithoutMisaligningNightOrTreasury)
{
    constexpr std::int64_t day0 = 1790524800000LL; // 2026-09-28 00:00 Beijing
    const auto bj = [&](int h, int m, int s = 0, int ms = 0) {
        return day0 + ((h * 60LL + m) * 60LL + s) * 1000LL + ms;
    };
    // SHFE/rb: closes at 10:15, 11:30, 15:00 and 23:00.
    EXPECT_EQ(minute_bucket_ms({"SHFE", "rb2610"}, bj(8, 59, 0, 500)), bj(9, 0));
    EXPECT_EQ(minute_bucket_ms({"SHFE", "rb2610"}, bj(10, 15, 0, 500)), bj(10, 14));
    EXPECT_EQ(minute_bucket_ms({"SHFE", "rb2610"}, bj(11, 30, 0, 0)), bj(11, 29));
    EXPECT_EQ(minute_bucket_ms({"SHFE", "rb2610"}, bj(15, 0, 0, 500)), bj(14, 59));
    EXPECT_EQ(minute_bucket_ms({"SHFE", "rb2610"}, bj(20, 59, 10, 0)), bj(21, 0));
    EXPECT_EQ(minute_bucket_ms({"SHFE", "rb2610"}, bj(23, 0, 0, 0)), bj(22, 59));
    // SHFE/cu: trades past 23:00 until 01:00; SHFE/au: trades past 01:00 until 02:30.
    EXPECT_EQ(minute_bucket_ms({"SHFE", "cu2610"}, bj(23, 0, 0, 0)), bj(23, 0));
    EXPECT_EQ(minute_bucket_ms({"SHFE", "cu2610"}, bj(1, 0, 0, 500)), bj(0, 59));
    EXPECT_EQ(minute_bucket_ms({"SHFE", "au2612"}, bj(1, 0, 0, 0)), bj(1, 0));
    EXPECT_EQ(minute_bucket_ms({"SHFE", "au2612"}, bj(2, 30, 0, 0)), bj(2, 29));
    // CFFEX/IF & T: 09:29 opening auction joins 09:30; no 10:15 break; treasury closes at 15:15.
    EXPECT_EQ(minute_bucket_ms({"CFFEX", "IF2609"}, bj(9, 29, 15, 0)), bj(9, 30));
    EXPECT_EQ(minute_bucket_ms({"CFFEX", "IF2609"}, bj(10, 15, 0, 0)), bj(10, 15));
    EXPECT_EQ(minute_bucket_ms({"CFFEX", "IF2609"}, bj(15, 0, 0, 0)), bj(14, 59));
    EXPECT_EQ(minute_bucket_ms({"CFFEX", "T2612"}, bj(9, 29, 0, 500)), bj(9, 30));
    EXPECT_EQ(minute_bucket_ms({"CFFEX", "T2612"}, bj(15, 0, 0, 0)), bj(15, 0));
    EXPECT_EQ(minute_bucket_ms({"CFFEX", "T2612"}, bj(15, 15, 0, 500)), bj(15, 14));
}

TEST(IntradayBars, ColdStartPreservesOpeningAuctionVolumeAndMarksTruncatedBarsPartial)
{
    constexpr std::int64_t day0 = 1790524800000LL; // 2026-09-28 00:00 Beijing
    const auto bj = [&](int h, int m, int s = 0) {
        return day0 + ((h * 60LL + m) * 60LL + s) * 1000LL;
    };
    const auto mk = [](const char* venue, const char* symbol, std::int64_t ms, std::int64_t vol) {
        MarketQuote q;
        q.instrument = {venue, symbol};
        q.trading_day = "20260929";
        q.source_ms = ms;
        q.last = Decimal::parse("3500");
        q.volume = vol;
        return q;
    };
    IntradayBars bars;
    // Night-session contract cold-starting at 20:59:50 preserves full night opening auction volume.
    bars.observe(mk("SHFE", "rb2610", bj(20, 59, 50), 120));
    bars.observe(mk("SHFE", "rb2610", bj(21, 0, 30), 150));
    const auto rb = bars.series({"SHFE", "rb2610"});
    ASSERT_TRUE(rb && rb->bars.size() == 1U);
    EXPECT_FALSE(rb->bars[0].partial);
    EXPECT_EQ(rb->bars[0].volume, 150);

    // Day-only contract cold-starting at 09:29:50 preserves full day opening auction volume.
    bars.observe(mk("CFFEX", "IF2609", bj(9, 29, 50), 40));
    bars.observe(mk("CFFEX", "IF2609", bj(9, 30, 30), 55));
    const auto idx = bars.series({"CFFEX", "IF2609"});
    ASSERT_TRUE(idx && idx->bars.size() == 1U);
    EXPECT_FALSE(idx->bars[0].partial);
    EXPECT_EQ(idx->bars[0].volume, 55);

    // Night-session contract cold-starting at 08:59:50 (after unseen night volume) stays partial
    // and never attributes accumulated night volume into 09:00.
    bars.observe(mk("SHFE", "cu2610", bj(8, 59, 50), 50000));
    bars.observe(mk("SHFE", "cu2610", bj(9, 0, 20), 50030));
    const auto cu = bars.series({"SHFE", "cu2610"});
    ASSERT_TRUE(cu && cu->bars.size() == 1U);
    EXPECT_TRUE(cu->bars[0].partial);
    EXPECT_EQ(cu->bars[0].volume, 30);
}
