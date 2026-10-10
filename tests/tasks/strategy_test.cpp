#include "strategy.hpp"
#include "data/bar_fixture.hpp"
#include <asterion/protocol/trading.hpp>
#include <gtest/gtest.h>
using namespace asterion;
namespace {
Decimal d(const char* value) {
  return Decimal::parse(value);
}
Instrument instrument() {
  return {{"SHFE", "rb2610"}, "CNY", d("1"), d("1"), d("10")};
}
protocol::v1::Strategy strategy(Json rule, const char* sides = "both", const char* quantity = "1") {
  return protocol::encode_strategy(
      {{"quantity", quantity}, {"sides", sides}, {"rule", std::move(rule)}});
}
// A rule over several contracts: it carries a notional and takes no quantity.
protocol::v1::Strategy ranking(Json rule, const char* sides = "both") {
  return protocol::encode_strategy({{"sides", sides}, {"rule", std::move(rule)}});
}
using Targets = std::vector<std::optional<Decimal>>;
// The target after each of these closes, fed as bars without a range.
Targets targets(const protocol::v1::Strategy& definition,
                std::initializer_list<const char*> closes) {
  auto run = make_strategy(definition, instrument());
  run->start();
  Targets result;
  std::int64_t time = 100;
  for (const auto* close : closes)
    result.push_back(run->on_bar(test::flat("2026-09-25", time++, close, "1")));
  return result;
}
const Json average{{"kind", "moving_average"}, {"fast", 1}, {"slow", 2}};
} // namespace
TEST(Strategy, BarsArriveInOrderWhileItRunsAndARestartForgetsThem) {
  auto run = make_strategy(strategy({{"kind", "moving_average"}, {"fast", 1}, {"slow", 3}}, "long"),
                           instrument());
  auto bar = test::flat("2026-09-25", 100, "100", "1");
  EXPECT_THROW(run->on_bar(bar), std::logic_error);
  run->start();
  EXPECT_THROW(run->start(), std::logic_error);
  EXPECT_FALSE(run->on_bar(bar));
  bar.timestamp_ns++;
  bar.open = bar.high = bar.low = bar.close = d("101");
  EXPECT_FALSE(run->on_bar(bar));
  bar.timestamp_ns++;
  bar.open = bar.high = bar.low = bar.close = d("102");
  EXPECT_EQ(run->on_bar(bar), d("1"));
  bar.timestamp_ns--;
  EXPECT_THROW(run->on_bar(bar), std::invalid_argument);
  run->stop();
  run->start();
  EXPECT_FALSE(run->on_bar(bar));
}
TEST(Strategy, ATargetIsTheQuantityOnTheSideTheRuleTakesWhereItMayBeHeld) {
  // With windows of 1 and 2 the close is above, below, equal to and above its average.
  const auto closes = {"100", "102", "101", "101", "103"};
  EXPECT_EQ(targets(strategy(average, "both", "2"), closes),
            (Targets{{}, d("2"), d("-2"), d("0"), d("2")}));
  EXPECT_EQ(targets(strategy(average, "long", "2"), closes),
            (Targets{{}, d("2"), d("0"), d("0"), d("2")}));
  EXPECT_EQ(targets(strategy(average, "short", "2"), closes),
            (Targets{{}, d("0"), d("-2"), d("0"), d("0")}));
}
TEST(Strategy, BreakoutEntersBeyondTheEntryChannelAndLeavesAtTheExitChannel) {
  // Four bars form the channel. 18 and 19 close above it; 17 falls below the
  // low of the two bars before it and gives the long up; 15 breaks the low of
  // the four before it; 18 passes the high of the two before it and covers.
  EXPECT_EQ(targets(strategy({{"kind", "breakout"}, {"entry", 4}, {"exit", 2}}),
                    {"10", "12", "14", "16", "18", "19", "17", "17", "15", "18"}),
            (Targets{{}, {}, {}, {}, d("1"), d("1"), d("0"), d("0"), d("-1"), d("0")}));
  // The channel is made of highs and lows, not closes: a close inside the
  // range of the earlier bars is no breakout.
  auto run =
      make_strategy(strategy({{"kind", "breakout"}, {"entry", 2}, {"exit", 1}}), instrument());
  run->start();
  EXPECT_FALSE(run->on_bar(test::bar("2026-09-25", 100, "10", "15", "9", "10", "1")));
  EXPECT_FALSE(run->on_bar(test::bar("2026-09-25", 101, "10", "12", "8", "11", "1")));
  EXPECT_EQ(run->on_bar(test::flat("2026-09-25", 102, "14", "1")), d("0"));
  EXPECT_EQ(run->on_bar(test::flat("2026-09-25", 103, "15", "1")), d("1"));
}
TEST(Strategy, MomentumComparesTheCloseWithTheOneLookbackBarsEarlier) {
  // 12 is above 10, 11 equals 11, 9 is below 12.
  EXPECT_EQ(
      targets(strategy({{"kind", "momentum"}, {"lookback", 2}}), {"10", "11", "12", "11", "9"}),
      (Targets{{}, {}, d("1"), d("0"), d("-1")}));
}
TEST(Strategy, ReversionFadesACloseBeyondTheBandAndLeavesAtTheMean) {
  // Over four closes with a band of one deviation: 14 is three above a mean of
  // 11 and is sold; 11 is back under the mean and the short is covered; 6 is
  // far below and is bought; 9 is still under its mean and the long stays; 12
  // is more than a deviation above and is sold outright.
  EXPECT_EQ(targets(strategy({{"kind", "reversion"}, {"window", 4}, {"width", "1"}}),
                    {"10", "10", "10", "14", "11", "6", "9", "12"}),
            (Targets{{}, {}, {}, d("-1"), d("0"), d("1"), d("1"), d("-1")}));
  // Closes that do not vary have no band to leave.
  EXPECT_EQ(
      targets(strategy({{"kind", "reversion"}, {"window", 2}, {"width", "2"}}), {"10", "10", "10"}),
      (Targets{{}, d("0"), d("0")}));
}
TEST(Strategy, ADefinitionStatesWindowsItsRuleCanWorkWith) {
  for (const auto& [rule, warmup] : std::vector<std::pair<Json, std::size_t>>{
           {{{"kind", "moving_average"}, {"fast", 5}, {"slow", 20}}, 20},
           {{{"kind", "breakout"}, {"entry", 20}, {"exit", 10}}, 21},
           {{{"kind", "momentum"}, {"lookback", 10}}, 11},
           {{{"kind", "reversion"}, {"window", 20}, {"width", "2.5"}}, 20}}) {
    const auto definition = strategy(rule);
    EXPECT_NO_THROW(make_strategy(definition, instrument())) << rule.dump();
    EXPECT_EQ(protocol::strategy_warmup(definition), warmup) << rule.dump();
    EXPECT_EQ(protocol::decode_strategy(definition).at("rule"), rule);
  }
  for (const Json& rule :
       std::vector<Json>{{{"kind", "moving_average"}, {"fast", 20}, {"slow", 20}},
                         {{"kind", "moving_average"}, {"fast", 0}, {"slow", 20}},
                         {{"kind", "breakout"}, {"entry", 10}, {"exit", 11}},
                         {{"kind", "breakout"}, {"entry", 10}, {"exit", 0}},
                         {{"kind", "momentum"}, {"lookback", 0}},
                         {{"kind", "reversion"}, {"window", 1}, {"width", "2"}},
                         {{"kind", "reversion"}, {"window", 20}, {"width", "0"}},
                         {{"kind", "reversion"}, {"window", 20}, {"width", "10.5"}}})
    EXPECT_THROW(make_strategy(strategy(rule), instrument()), std::invalid_argument) << rule.dump();
  // A quantity the contract cannot trade, no sides and no rule make no strategy.
  EXPECT_THROW(make_strategy(strategy(average, "both", "0"), instrument()), std::invalid_argument);
  EXPECT_THROW(make_strategy(strategy(average, "both", "1.5"), instrument()),
               std::invalid_argument);
  auto incomplete = strategy(average);
  incomplete.clear_sides();
  EXPECT_THROW(make_strategy(incomplete, instrument()), std::invalid_argument);
  incomplete = strategy(average);
  incomplete.clear_rule();
  EXPECT_THROW(make_strategy(incomplete, instrument()), std::invalid_argument);
  for (const Json& rule : std::vector<Json>{{{"kind", "grid"}, {"step", 1}},
                                            {{"kind", "momentum"}},
                                            {{"kind", "momentum"}, {"lookback", 20000}},
                                            {{"kind", "momentum"}, {"lookback", 2.5}},
                                            {{"kind", "reversion"}, {"window", 20}, {"width", 2}}})
    EXPECT_THROW(strategy(rule), std::exception) << rule.dump();
}
TEST(Strategy, ARankingRuleHoldsTheStrongestLongAndTheWeakestShortOnBarsAllUnitsHave) {
  const Json rule{{"kind", "cross_momentum"}, {"reverse", false}, {"lookback", 1},
                  {"rebalance", 2},           {"count", 1},       {"notional", "1000"},
                  {"volatility", 0}};
  CrossSection ranked(ranking(rule), 4);
  using Wanted = std::optional<std::vector<double>>;
  const auto bars = [](CrossSection& run, std::int64_t time,
                       std::initializer_list<const char*> closes) {
    std::size_t unit = 0;
    for (const auto* close : closes) {
      if (*close)
        run.on_bar(unit, time, d(close));
      ++unit;
    }
    return run.rank();
  };
  // One shared bar has nothing a lookback of 1 can compare it with.
  EXPECT_EQ(bars(ranked, 1, {"100", "100", "100", "100"}), Wanted());
  // 1.10, 1.05, 1.00, 0.95 of the bar before: the first long, the last short.
  EXPECT_EQ(bars(ranked, 2, {"110", "105", "100", "95"}), Wanted({1, 0, 0, -1}));
  // A bar the fourth unit lacks is no bar of them all, and the next shared
  // one is not the second since the ranking.
  EXPECT_EQ(bars(ranked, 3, {"1", "1", "1", ""}), Wanted());
  EXPECT_EQ(bars(ranked, 4, {"110", "126", "90", "95"}), Wanted());
  // 0.9, 1.0, 1.1, 1.1 of the shared bar before, at time 4: of the equal
  // strongest the one named first is held.
  EXPECT_EQ(bars(ranked, 5, {"99", "126", "99", "104.5"}), Wanted({-1, 0, 1, 0}));

  CrossSection longs(ranking(rule, "long"), 2);
  EXPECT_EQ(bars(longs, 1, {"100", "100"}), Wanted());
  EXPECT_EQ(bars(longs, 2, {"99", "101"}), Wanted({0, 1}));

  // Reversed, the weakest is held long and the strongest short; of the equal
  // strongest it is now the one named last that is at the end.
  auto other = rule;
  other["reverse"] = true;
  CrossSection reversed(ranking(other), 4);
  EXPECT_EQ(bars(reversed, 1, {"100", "100", "100", "100"}), Wanted());
  EXPECT_EQ(bars(reversed, 2, {"110", "105", "100", "95"}), Wanted({-1, 0, 0, 1}));
  EXPECT_EQ(bars(reversed, 4, {"110", "126", "90", "95"}), Wanted());
  EXPECT_EQ(bars(reversed, 5, {"99", "126", "99", "104.5"}), Wanted({1, 0, 0, -1}));
  EXPECT_EQ(protocol::decode_strategy(ranking(other)).at("rule"), other);

  // A unit is never wanted on both sides; bars keep their order; a rule over
  // several contracts is no strategy for one.
  EXPECT_THROW(CrossSection(ranking(rule), 1), std::invalid_argument);
  EXPECT_THROW(longs.on_bar(0, 1, d("100")), std::invalid_argument);
  longs.on_bar(0, 3, d("100"));
  EXPECT_THROW(longs.on_bar(0, 3, d("100")), std::invalid_argument);
  EXPECT_THROW(make_strategy(ranking(rule), instrument()), std::invalid_argument);
  EXPECT_THROW(CrossSection(strategy(average), 4), std::invalid_argument);
  EXPECT_EQ(protocol::strategy_warmup(ranking(rule)), 2U);
  EXPECT_EQ(protocol::decode_strategy(ranking(rule)), (Json{{"sides", "both"}, {"rule", rule}}));
  for (const auto& [field, value] :
       {std::pair{"lookback", Json(0)}, std::pair{"rebalance", Json(0)},
        std::pair{"count", Json(0)}, std::pair{"count", Json(11)},
        std::pair{"notional", Json("0")}}) {
    auto invalid = rule;
    invalid[field] = value;
    EXPECT_THROW(protocol::validate_strategy(ranking(invalid), d("1")), std::invalid_argument)
        << field;
  }
  // It sizes by its notional: a quantity beside it is refused, and a rule on
  // one contract cannot go without one.
  EXPECT_THROW(strategy(rule), Error);
  auto sized = ranking(rule);
  sized.mutable_quantity()->set_units(d("1").raw());
  EXPECT_THROW(protocol::validate_strategy(sized, d("1")), std::invalid_argument);
  EXPECT_THROW(ranking(average), Error);
}
TEST(Strategy, ARankingByTheTermStructureAveragesTheCarryOverItsWindow) {
  const Json rule{{"kind", "cross_term_structure"},
                  {"reverse", false},
                  {"lookback", 2},
                  {"rebalance", 1},
                  {"count", 1},
                  {"notional", "1000"},
                  {"volatility", 0}};
  CrossSection ranked(ranking(rule), 2);
  using Wanted = std::optional<std::vector<double>>;
  const auto bars = [&](std::int64_t time, const char* first, const char* second) {
    ranked.on_bar(0, time, d("100"), d(first));
    ranked.on_bar(1, time, d("100"), d(second));
    return ranked.rank();
  };
  // A window of two has nothing to rank by after one bar.
  EXPECT_EQ(bars(1, "0.4", "0.1"), Wanted());
  // Means of 0.2 and 0.15: the first is the higher.
  EXPECT_EQ(bars(2, "0", "0.2"), Wanted({1, -1}));
  // A carry may be negative; means of -0.2 and 0.2.
  EXPECT_EQ(bars(3, "-0.4", "0.2"), Wanted({-1, 1}));
  EXPECT_EQ(protocol::strategy_warmup(ranking(rule)), 2U);
  EXPECT_EQ(protocol::decode_strategy(ranking(rule)).at("rule"), rule);
  EXPECT_EQ(ranking(rule).cross().factor(), protocol::v1::TERM_STRUCTURE);
  // A ranking names what it ranks by.
  auto unnamed = ranking(rule);
  unnamed.mutable_cross()->clear_factor();
  EXPECT_THROW(protocol::validate_strategy(unnamed, d("1")), std::invalid_argument);
}
TEST(Strategy, AVolatilityWindowSplitsWhatIsHeldByTheInverseOfEachUnitsVolatility) {
  // Momentum over one bar, sized by the volatility of the last two returns:
  // three closes are needed before the first ranking.
  const Json rule{{"kind", "cross_momentum"}, {"reverse", false}, {"lookback", 1},
                  {"rebalance", 1},           {"count", 1},       {"notional", "1000"},
                  {"volatility", 2}};
  const auto bars = [](CrossSection& run, std::int64_t time,
                       std::initializer_list<const char*> closes) {
    std::size_t unit = 0;
    for (const auto* close : closes)
      run.on_bar(unit++, time, d(close));
    return run.rank();
  };
  CrossSection ranked(ranking(rule), 3);
  EXPECT_FALSE(bars(ranked, 1, {"100", "10000", "50"}));
  EXPECT_FALSE(bars(ranked, 2, {"110", "10100", "52"}));
  // 0.90, 0.99 and 0.95 of the bar before: the second is held long and the
  // first short. Their returns were +10% and -10% against +1% and -1%, ten
  // times the volatility: of the two units' worth they hold together the
  // first takes one eleventh.
  const auto shares = bars(ranked, 3, {"99", "9999", "49.4"});
  ASSERT_TRUE(shares);
  EXPECT_NEAR((*shares)[0], -2.0 / 11, 1e-9);
  EXPECT_NEAR((*shares)[1], 20.0 / 11, 1e-9);
  EXPECT_EQ((*shares)[2], 0);
  // A unit that never moved has no volatility to size by: it ranks highest
  // here and is not held, and the other holds one unit's worth.
  CrossSection still(ranking(rule), 2);
  EXPECT_FALSE(bars(still, 1, {"100", "100"}));
  EXPECT_FALSE(bars(still, 2, {"100", "110"}));
  const auto alone = bars(still, 3, {"100", "99"});
  ASSERT_TRUE(alone);
  EXPECT_EQ((*alone)[0], 0);
  EXPECT_DOUBLE_EQ((*alone)[1], -1);
  EXPECT_EQ(protocol::strategy_warmup(ranking(rule)), 3U);
  EXPECT_EQ(protocol::decode_strategy(ranking(rule)).at("rule"), rule);
  // One return has no standard deviation.
  auto single = rule;
  single["volatility"] = 1;
  EXPECT_THROW(protocol::validate_strategy(ranking(single), d("1")), std::invalid_argument);
}
