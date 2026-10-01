#include <asterion/domain/intraday_bars.hpp>
#include <asterion/protocol/market.hpp>
#include <gtest/gtest.h>
using namespace asterion;
namespace {
MarketQuote quote(std::int64_t ms, const char* last, std::int64_t volume,
                  const char* day = "20260930") {
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
TEST(IntradayBars, AggregatesMinutesFromCumulativeVolume) {
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
TEST(IntradayBars, OneMinuteChangeNeedsAnObservationAMinuteOld) {
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
TEST(IntradayBars, NewTradingDayStartsANewSeries) {
  IntradayBars bars;
  bars.observe(quote(base, "100", 10));
  bars.interrupt();
  bars.observe(quote(base + 3'600'000, "105", 3, "20261001"));
  const auto series = bars.series({"SHFE", "br2611"});
  EXPECT_EQ(series->trading_day, "20261001");
  EXPECT_FALSE(series->interrupted);
  ASSERT_EQ(series->bars.size(), 1u);
  EXPECT_EQ(series->bars[0].volume, 0);
}
TEST(IntradayBars, ProtocolRoundTripValidatesBars) {
  IntradayBars bars;
  bars.observe(quote(base, "100", 10));
  auto encoded = protocol::encode_minutes(*bars.series({"SHFE", "br2611"}));
  const auto decoded = protocol::decode_minutes(encoded);
  EXPECT_EQ(decoded.at("bars").size(), 1u);
  EXPECT_EQ(decoded.at("previous_settlement"), Decimal::parse("15390").str());
  encoded.mutable_bars(0)->set_high("1");
  EXPECT_THROW(protocol::decode_minutes(encoded), std::invalid_argument);
}
