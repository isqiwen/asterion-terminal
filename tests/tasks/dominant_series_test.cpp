#include "bar_dataset_source.hpp"
#include "tasks/paper_input.hpp"
#include "data/history_requests.hpp"
#include "data/bar_fixture.hpp"
#include "engine.hpp"
#include "factor_engine.hpp"
#include "history_daily.hpp"
#include "task_store.hpp"
#include "tasks/task_store_support.hpp"
#include "data/data_fixture.hpp"
#include <asterion/kernel/process/child.hpp>
#include <gtest/gtest.h>
using namespace asterion;
namespace {
Decimal d(const char* text) {
  return Decimal::parse(text);
}
const std::vector<std::string> days{"2026-09-21", "2026-09-22", "2026-09-23",
                                    "2026-09-24", "2026-09-25", "2026-09-28"};
// Three one-minute bars on a day, closing at first, first + step, first + 2 * step.
// Each bar's range lets a limit at the previous close fill on it.
std::vector<MarketBar> day_bars(std::size_t day, int first, int step, const char* volume = "10") {
  std::vector<MarketBar> result;
  for (int i = 0; i < 3; ++i) {
    const auto price = first + i * step;
    const auto at = 1'790'000'000'000'000'000 +
                    static_cast<std::int64_t>(day) * 86'400'000'000'000 + i * 60'000'000'000LL;
    result.push_back(test::bar(days[day], at, std::to_string(price).c_str(),
                               std::to_string(price + 1).c_str(), std::to_string(price - 2).c_str(),
                               std::to_string(price).c_str(), volume));
  }
  return result;
}
std::vector<MarketBar> join(std::initializer_list<std::vector<MarketBar>> parts) {
  std::vector<MarketBar> result;
  for (const auto& part : parts)
    result.insert(result.end(), part.begin(), part.end());
  return result;
}
// rb2610 is dominant on the first two days at about twice the price of
// rb2701, which takes over on the third day. Raw prices fall by half at the
// roll; back-adjusted they keep rising.
backtest::v1::BacktestInput rolling(const char* outgoing_volume = "10") {
  const auto near = test::dataset(
      join({day_bars(0, 200, 2), day_bars(1, 206, 2), day_bars(2, 212, 0, outgoing_volume),
            day_bars(3, 212, 0, outgoing_volume)}),
      {}, test::contract("SHFE", "rb2610", "rb", "2026-10"));
  const auto far =
      test::dataset(join({day_bars(2, 106, 1), day_bars(3, 109, 1), day_bars(4, 112, 1)}), {},
                    test::contract("SHFE", "rb2701", "rb", "2027-01"));
  const auto costs = test::cost_schedule({{"margin_per_lot", "100"},
                                          {"open_fee", "2"},
                                          {"close_today_fee", "3"},
                                          {"close_yesterday_fee", "4"},
                                          {"margin_rate", "0"},
                                          {"open_fee_rate", "0"},
                                          {"close_today_fee_rate", "0"},
                                          {"close_yesterday_fee_rate", "0"}});
  backtest::v1::BacktestInput input;
  input.set_version(10);
  *input.mutable_paper() = testing_support::paper_input(
      {{"version", 4},
       {"type", "historical_paper"},
       {"risk",
        {{"max_order_quantity", "1"},
         {"max_gross_quantity", "1"},
         {"max_working_orders", std::uint64_t{2}}}},
       {"deposit", "100000"},
       {"contracts",
        {{{"dataset", protocol::decode_bar_dataset(near)}, {"cost_schedule", costs}},
         {{"dataset", protocol::decode_bar_dataset(far)}, {"cost_schedule", costs}}}}});
  *input.add_strategies() = testing_support::moving_average(1, 4);
  input.set_dataset_revision(protocol::dataset_revision(input.paper()));
  auto* series = input.add_series();
  for (const auto& [day, contract, factor] :
       {std::tuple{days[0], 0U, "0.5"}, std::tuple{days[2], 1U, "1"}}) {
    auto* roll = series->add_rolls();
    roll->set_trading_day(day);
    roll->set_contract(contract);
    roll->mutable_factor()->set_units(d(factor).raw());
  }
  return input;
}
} // namespace
TEST(DominantSeries, RollClosesTheOldMonthThenOpensTheNewOneAtRealPrices) {
  const auto input = rolling();
  const auto result = backtest::run(input);
  EXPECT_EQ(result.SerializeAsString(), backtest::run(input).SerializeAsString());
  // Bought rb2610 at its real price, sold it on the roll day, and only then
  // bought rb2701: the signal stayed long because the adjusted series kept
  // rising where the raw prices halved.
  ASSERT_EQ(result.account().fills_size(), 3);
  EXPECT_EQ(result.account().fills(0).price().units(), d("206").raw());
  EXPECT_EQ(result.account().fills(1).price().units(), d("212").raw());
  EXPECT_EQ(result.account().fills(2).price().units(), d("106").raw());
  ASSERT_EQ(result.account().orders_size(), 3);
  EXPECT_EQ(result.account().orders(1).symbol(), "rb2610");
  EXPECT_EQ(result.account().orders(2).symbol(), "rb2701");
  ASSERT_EQ(result.account().positions_size(), 1);
  EXPECT_EQ(result.account().positions(0).symbol(), "rb2701");
  // The max gross quantity of 1 was never exceeded: the account did not hold
  // both months at once.
  EXPECT_EQ(result.account().fees().units(), d("8").raw());
  // (212 - 206) * 10 on rb2610, then rb2701 from 106 to its last settlement 112.
  EXPECT_EQ(result.account().equity().units(), d("100112").raw());
  // A month has a settlement row only on the days it trades.
  ASSERT_EQ(result.settlements_size(), 5);
  EXPECT_EQ(result.settlements(0).contracts_size(), 1);
  EXPECT_EQ(result.settlements(0).contracts(0).symbol(), "rb2610");
  EXPECT_EQ(result.settlements(2).contracts_size(), 2);
  EXPECT_EQ(result.settlements(4).contracts_size(), 1);
  EXPECT_EQ(result.settlements(4).contracts(0).symbol(), "rb2701");
}
TEST(DominantSeries, AMonthWithAnEarlierBarTakesOverOnTheBarTheOldOneIsClosedOn) {
  auto input = rolling();
  // rb2701 also has the bars of the day before it becomes dominant, closing
  // at 103, 104 and 105. Nothing is traded in it that day.
  auto* far = input.mutable_paper()->mutable_contracts(1)->mutable_dataset();
  *far = test::dataset(
      join({day_bars(1, 103, 1), day_bars(2, 106, 1), day_bars(3, 109, 1), day_bars(4, 112, 1)}),
      {}, test::contract("SHFE", "rb2701", "rb", "2027-01"));
  input.set_dataset_revision(protocol::dataset_revision(input.paper()));
  const auto result = backtest::run(input);
  // rb2610 is sold on the first bar of the roll day as before. rb2701 no
  // longer waits a bar: its order rests at its own last close of 105 and
  // fills on that same bar, whose low is 104, instead of at 106 a bar later.
  ASSERT_EQ(result.account().fills_size(), 3);
  EXPECT_EQ(result.account().fills(1).symbol(), "rb2610");
  EXPECT_EQ(result.account().fills(1).price().units(), d("212").raw());
  EXPECT_EQ(result.account().fills(2).symbol(), "rb2701");
  EXPECT_EQ(result.account().fills(2).price().units(), d("105").raw());
  // Both fills carry the equity mark of that one bar: the account was never
  // flat at a bar's end between the two months.
  const auto& sold = result.account().fills(1);
  const auto& bought = result.account().fills(2);
  const auto order = [&](const std::string& id) {
    return *std::ranges::find(result.account().orders(), id,
                              [](const auto& item) { return item.id(); });
  };
  EXPECT_EQ(order(sold.order_id()).side(), protocol::v1::SELL);
  EXPECT_EQ(order(bought.order_id()).side(), protocol::v1::BUY);
  EXPECT_EQ(order(bought.order_id()).limit_price().units(), d("105").raw());
  // (212 - 206) * 10 on rb2610, rb2701 from 105 to its last settlement 112.
  EXPECT_EQ(result.account().equity().units(), d("100122").raw());
  // The gross limit of one lot holds: the old month is closed first.
  EXPECT_EQ(result.account().fees().units(), d("8").raw());
}
TEST(DominantSeries, WithoutAdjustmentTheSameBarsWouldSellAtTheRoll) {
  auto input = rolling();
  input.mutable_series(0)->mutable_rolls(0)->mutable_factor()->set_units(d("1").raw());
  const auto result = backtest::run(input);
  // Seen raw, the price halves at the roll: the average stays above the price
  // for the rest of the roll day and rb2701 is bought a day later, at 109
  // instead of 107.
  ASSERT_EQ(result.account().fills_size(), 3);
  EXPECT_EQ(result.account().fills(2).price().units(), d("109").raw());
}
TEST(DominantSeries, ARankingRuleTakesASeriesAsOneOfTheContractsItRanks) {
  auto input = rolling();
  // hc2610 stays at 100 on the same bars; rb, read back-adjusted, rises by
  // one a bar and is the stronger of the two at every bar.
  auto* flat = input.mutable_paper()->add_contracts();
  *flat = input.paper().contracts(0);
  *flat->mutable_dataset() =
      test::dataset(join({day_bars(0, 100, 0), day_bars(1, 100, 0), day_bars(2, 100, 0),
                          day_bars(3, 100, 0), day_bars(4, 100, 0)}),
                    {}, test::contract("SHFE", "hc2610", "hc", "2026-10"));
  input.set_dataset_revision(protocol::dataset_revision(input.paper()));
  // 1500 is nearest one lot of either month: about 2000 for rb2610 at its
  // real prices and 1100 for rb2701.
  *input.mutable_strategies(0) = protocol::encode_strategy({{"sides", "long"},
                                                            {"rule",
                                                             {{"kind", "cross_momentum"},
                                                              {"reverse", false},
                                                              {"lookback", 1},
                                                              {"rebalance", 1},
                                                              {"count", 1},
                                                              {"notional", "1500"},
                                                              {"volatility", 0}}}});
  const auto result = backtest::run(input);
  // Ranked from the second bar: rb2610 is bought at that bar's real close of
  // 202, sold on the roll day, and rb2701 bought once it is. hc never trades.
  ASSERT_EQ(result.account().fills_size(), 3);
  EXPECT_EQ(result.account().fills(0).symbol(), "rb2610");
  EXPECT_EQ(result.account().fills(0).price().units(), d("202").raw());
  EXPECT_EQ(result.account().fills(1).symbol(), "rb2610");
  EXPECT_EQ(result.account().fills(1).price().units(), d("212").raw());
  EXPECT_EQ(result.account().fills(2).symbol(), "rb2701");
  EXPECT_EQ(result.account().fills(2).price().units(), d("106").raw());
  // (212 - 202) * 10 on rb2610, rb2701 from 106 to 112, and fees of 8.
  EXPECT_EQ(result.account().equity().units(), d("100152").raw());
}
TEST(DominantSeries, ARankingByTheTermStructureHoldsTheSeriesWithTheHigherCarry) {
  auto input = rolling();
  // A second series of one month, hc2610, steady at 150.
  auto* flat = input.mutable_paper()->add_contracts();
  *flat = input.paper().contracts(0);
  *flat->mutable_dataset() =
      test::dataset(join({day_bars(0, 150, 0), day_bars(1, 150, 0), day_bars(2, 150, 0),
                          day_bars(3, 150, 0), day_bars(4, 150, 0)}),
                    {}, test::contract("SHFE", "hc2610", "hc", "2026-10"));
  input.set_dataset_revision(protocol::dataset_revision(input.paper()));
  auto* only = input.add_series()->add_rolls();
  only->set_trading_day(days[0]);
  only->set_contract(2);
  only->mutable_factor()->set_units(d("1").raw());
  // No day begins knowing a term structure before the second. On it rb2610
  // settled above rb2701 and hc level with its later month; from the roll day
  // rb2701 stands below its later month and hc above.
  const auto term = [&](int series, std::size_t day, const char* near, const char* far,
                        const char* month) {
    auto* point = input.mutable_series(series)->add_terms();
    point->set_trading_day(days[day]);
    point->mutable_near()->set_units(d(near).raw());
    point->mutable_far()->set_units(d(far).raw());
    point->set_far_month(month);
  };
  term(0, 1, "204", "200", "2027-01");
  for (const std::size_t day : {2, 3, 4})
    term(0, day, "108", "110", "2027-05");
  term(1, 1, "150", "150", "2027-01");
  for (const std::size_t day : {2, 3, 4})
    term(1, day, "150", "148", "2027-01");
  const Json rule{{"kind", "cross_term_structure"},
                  {"reverse", false},
                  {"lookback", 1},
                  {"rebalance", 1},
                  {"count", 1},
                  {"notional", "1500"},
                  {"volatility", 0}};
  *input.mutable_strategies(0) = protocol::encode_strategy({{"sides", "long"}, {"rule", rule}});
  const auto result = backtest::run(input);
  // Ranked from the first bar of the second day: rb2610 is bought at that
  // bar's 206. On the roll day hc ranks higher: rb2610 is sold as it leaves
  // the series, rb2701 is never bought, and hc2610 is bought at 150.
  ASSERT_EQ(result.account().fills_size(), 3);
  const auto fill = [&](int index) {
    const auto& value = result.account().fills(index);
    return value.symbol() + " at " + Decimal::from_raw(value.price().units()).str();
  };
  EXPECT_EQ(fill(0), "rb2610 at 206");
  EXPECT_EQ(fill(1), "rb2610 at 212");
  EXPECT_EQ(fill(2), "hc2610 at 150");
  ASSERT_EQ(result.account().positions_size(), 1);
  EXPECT_EQ(result.account().positions(0).symbol(), "hc2610");
  // (212 - 206) * 10 on rb2610, and fees of 2 + 4 + 2.
  EXPECT_EQ(result.account().equity().units(), d("100052").raw());
  // A contract outside any series has no later month to stand against.
  input.mutable_series()->RemoveLast();
  EXPECT_THROW(backtest::validate(input), std::invalid_argument);
}
TEST(DominantSeries, APositionThatCannotBeClosedFailsInsteadOfBeingDropped) {
  // No volume in the outgoing month on and after the roll day.
  EXPECT_THROW(backtest::run(rolling("0")), std::invalid_argument);
}
TEST(DominantSeries, ScheduleMustNameLaterMonthsOfOneProductAndEndUnadjusted) {
  auto input = rolling();
  input.mutable_series(0)->mutable_rolls(1)->mutable_factor()->set_units(d("2").raw());
  EXPECT_THROW(backtest::validate(input), std::invalid_argument) << "last factor is 1";
  input = rolling();
  input.mutable_series(0)->mutable_rolls(1)->set_contract(0);
  EXPECT_THROW(backtest::validate(input), std::invalid_argument) << "a month appears once";
  input = rolling();
  input.mutable_series(0)->mutable_rolls(1)->set_trading_day(days[0]);
  EXPECT_THROW(backtest::validate(input), std::invalid_argument) << "rolls ascend";
  // Months that do not share their trading days need a series.
  input = rolling();
  input.clear_series();
  EXPECT_THROW(backtest::validate(input), std::invalid_argument);
}
namespace {
struct Directory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() / ("asterion-dominant-" + unique_process_id());
  Directory() { std::filesystem::create_directories(path); }
  ~Directory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};
struct Row {
  std::string day;
  int price, open_interest;
};
// A completed daily download of one month of a SHFE product; daily bars are
// both its bars and its settlements. A bar opens and closes at its price and
// ranges `range` either side of it.
data::BarDatasetSources month(data::Store& store, const std::string& delivery,
                              const std::vector<Row>& rows, const std::string& product = "rb",
                              int range = 0) {
  class Daily final : public HistoricalDailyPort {
  public:
    std::vector<Row> rows;
    int range = 0;
    HistorySemantics semantics() const override {
      return {"tushare.fut_daily", "test.confirmed.v1", "Asia/Shanghai", "trading_day"};
    }
    std::vector<HistoricalDailyBar> read(const HistoricalDailyRange& r, std::stop_token) override {
      std::vector<HistoricalDailyBar> result;
      for (const auto& row : rows) {
        const auto day = parse_trading_date(row.day);
        const auto price = Decimal::parse(std::to_string(row.price));
        if (day >= r.begin && day <= r.end)
          result.push_back({day,
                            price,
                            Decimal::parse(std::to_string(row.price + range)),
                            Decimal::parse(std::to_string(row.price - range)),
                            price,
                            d("100"),
                            d("1000"),
                            Decimal::parse(std::to_string(row.open_interest)),
                            {},
                            {},
                            price});
      }
      return result;
    }
  } provider;
  provider.rows = rows;
  provider.range = range;
  const auto code = delivery.substr(2, 2) + delivery.substr(5);
  const auto symbol = product + code;
  auto listed = product;
  std::ranges::transform(listed, listed.begin(), [](unsigned char c) { return std::toupper(c); });
  const auto task = "daily-" + symbol;
  const auto input =
      asterion::testing_support::daily_request({{"version", 2},
                                                {"contract_id", "SHFE/" + product + "/" + delivery},
                                                {"source", "tushare.fut_daily"},
                                                {"source_instrument", listed + code + ".SHF"},
                                                {"begin_day", rows.front().day},
                                                {"end_day", rows.back().day},
                                                {"requests_per_minute", 500}});
  const auto id =
      test::publish_download(store, task, input, provider).daily_result().manifest_sha256();
  return store.sources(
      protocol::encode_bar_dataset_request({{"source_dataset_ids", Json::array({id})},
                                            {"settlement_dataset_ids", Json::array({id})},
                                            {"begin_day", ""},
                                            {"end_day", ""},
                                            {"contract",
                                             {{"venue", "SHFE"},
                                              {"symbol", symbol},
                                              {"product", product},
                                              {"delivery_month", delivery},
                                              {"currency", "CNY"},
                                              {"price_increment", "1"},
                                              {"quantity_increment", "1"},
                                              {"multiplier", "10"}}}}));
}
} // namespace
TEST(DominantSeries, PersistedRollingResultRejectsPreviousEngineAndSurvivesRestart) {
  Directory directory;
  const auto input = rolling();
  const auto expected = backtest::run(input);
  {
    tasks::Store store(directory.path, tasks::Identity{"task", "historical-data"});
    tasks::submit(store, "rolling", input);
    const auto token = store.commit(store.claim("rolling")).token();
    auto old = expected;
    old.set_engine_version("asterion.backtest.v11");
    EXPECT_THROW(store.commit(tasks::finish(store, "rolling", token, old)), std::invalid_argument);
    EXPECT_EQ(store.get("rolling").state(), task::v1::RUNNING);
    store.commit(tasks::finish(store, "rolling", token, expected));
  }
  tasks::Store restored(directory.path, tasks::Identity{"task", "historical-data"});
  EXPECT_EQ(restored.get("rolling").state(), task::v1::SUCCEEDED);
  EXPECT_EQ(restored.get("rolling").input().SerializeAsString(), input.SerializeAsString());
  EXPECT_EQ(tasks::result(restored, "rolling").SerializeAsString(), expected.SerializeAsString());
}
TEST(DominantSeries, ScheduleFollowsThePreviousDaysOpenInterestAndNeverMovesBack) {
  Directory directory;
  data::Store store(directory.path, "historical-data", "task");
  // Open interest moves to rb2701 on the third day, so it is dominant from
  // the fourth; rb2610's late surge does not bring it back.
  const auto near = month(store, "2026-10",
                          {{days[0], 200, 100},
                           {days[1], 204, 100},
                           {days[2], 200, 40},
                           {days[3], 202, 30},
                           {days[4], 204, 1000},
                           {days[5], 206, 1000}});
  const auto far = month(store, "2027-01",
                         {{days[0], 100, 10},
                          {days[1], 102, 60},
                          {days[2], 110, 90},
                          {days[3], 111, 95},
                          {days[4], 112, 99},
                          {days[5], 113, 99}});
  // Listed out of order: the series sorts by delivery month.
  const auto series = data::resolve_dominant_series({far, near});
  ASSERT_EQ(series.months, (std::vector<std::size_t>{1, 0}));
  ASSERT_EQ(series.schedule.rolls_size(), 2);
  EXPECT_EQ(series.schedule.rolls(0).trading_day(), days[1]) << "begins on the second day";
  EXPECT_EQ(series.schedule.rolls(0).contract(), 0U);
  EXPECT_EQ(series.schedule.rolls(1).trading_day(), days[3]);
  EXPECT_EQ(series.schedule.rolls(1).contract(), 1U);
  // 110 / 200 on the day before the roll.
  EXPECT_EQ(series.schedule.rolls(0).factor().units(), d("0.55").raw());
  EXPECT_EQ(series.schedule.rolls(1).factor().units(), d("1").raw());
  // rb2610: from the start through the roll day and the day after it.
  ASSERT_EQ(series.datasets[0].days_size(), 4);
  EXPECT_EQ(series.datasets[0].days(0).trading_day(), days[1]);
  EXPECT_EQ(series.datasets[0].days(3).trading_day(), days[4]);
  // rb2701: from the day before it takes over, for a price to take over at.
  ASSERT_EQ(series.datasets[1].days_size(), 4);
  EXPECT_EQ(series.datasets[1].days(0).trading_day(), days[2]);
  for (const auto& dataset : series.datasets)
    EXPECT_EQ(dataset.revision(), protocol::bar_dataset_revision(dataset));
  // While rb2610 is dominant each day begins knowing how it settled against
  // rb2701 the day before. Once rb2701 is dominant no later month is left.
  ASSERT_EQ(series.schedule.terms_size(), 2);
  EXPECT_EQ(
      protocol::decode_term_points(series.schedule),
      Json::array(
          {{{"trading_day", days[1]}, {"near", "200"}, {"far", "100"}, {"far_month", "2027-01"}},
           {{"trading_day", days[2]}, {"near", "204"}, {"far", "102"}, {"far_month", "2027-01"}}}));
  // Twice the later month, three delivery months away: a year's worth is 4.
  EXPECT_EQ(protocol::term_carry(series.schedule.terms(0), "2026-10"), d("4"));
}
TEST(DominantSeries, ResolutionFailsWhereItWouldHaveToGuess) {
  Directory directory;
  data::Store store(directory.path, "historical-data", "task");
  const auto far =
      month(store, "2027-01",
            {{days[0], 100, 10}, {days[1], 102, 60}, {days[2], 110, 90}, {days[3], 111, 95}});
  EXPECT_THROW(data::resolve_dominant_series({far}), std::invalid_argument);
  // rb2610 ends the day before rb2701 takes over: nothing to close on.
  const auto near =
      month(store, "2026-10", {{days[0], 200, 100}, {days[1], 204, 100}, {days[2], 200, 40}});
  EXPECT_THROW(data::resolve_dominant_series({near, far}), std::invalid_argument);
}
// The path a study of several products takes on daily bars: Data resolves each
// product's months as its dominant series, a ranking rule holds one long and
// one short, two windows are compared by rolling, and every fill gives a
// price increment up.
TEST(DominantSeries, DailySeriesOfTwoProductsAreRankedRolledAndComparedByRolling) {
  Directory directory;
  for (const auto* part : {"data", "tasks"})
    std::filesystem::create_directory(directory.path / part);
  data::Store store(directory.path / "data", "historical-data", "task");
  const auto day = [](int index) {
    using namespace std::chrono;
    return format_trading_date(
        year_month_day{sys_days{year{2026} / June / 1} + std::chrono::days{index}});
  };
  // Sixty days. rb rises ten a day to day 30 and falls from there; hc does
  // the opposite. A bar ranges 15 either side of its close, so a limit at
  // the close before it is always reached.
  const auto rb = [](int i) { return i < 30 ? 3000 + 10 * i : 3300 - 10 * (i - 30); };
  const auto hc = [](int i) { return i < 30 ? 4000 - 10 * i : 3700 + 10 * (i - 30); };
  const auto rows = [&](const auto& price, int spread, int taken_over_from) {
    std::vector<Row> result;
    for (int i = 0; i < 60; ++i)
      result.push_back({day(i), price(i) + spread,
                        // A near month keeps 1000 lots of interest; a far one
                        // has few until the day its interest passes that.
                        spread == 0           ? 1000
                        : i < taken_over_from ? 100
                                              : 5000});
    return result;
  };
  // rb2701, 50 dearer, has the larger interest from day 43 and is dominant
  // from day 44; hc2701, 30 cheaper, from day 51 and day 52.
  const auto rb_series =
      data::resolve_dominant_series({month(store, "2026-10", rows(rb, 0, 0), "rb", 15),
                                     month(store, "2027-01", rows(rb, 50, 43), "rb", 15)});
  const auto hc_series =
      data::resolve_dominant_series({month(store, "2026-10", rows(hc, 0, 0), "hc", 15),
                                     month(store, "2027-01", rows(hc, -30, 51), "hc", 15)});
  ASSERT_EQ(rb_series.schedule.rolls_size(), 2);
  EXPECT_EQ(rb_series.schedule.rolls(1).trading_day(), day(44));
  // The later month comes with the day before it takes over.
  EXPECT_EQ(rb_series.datasets[1].days(0).trading_day(), day(43));
  EXPECT_EQ(hc_series.schedule.rolls(1).trading_day(), day(52));
  // Each day a near month is dominant begins knowing a term structure.
  EXPECT_EQ(rb_series.schedule.terms_size(), 43);
  EXPECT_EQ(hc_series.schedule.terms_size(), 51);

  backtest::v1::BacktestInput input;
  input.set_version(10);
  auto* paper = input.mutable_paper();
  paper->mutable_deposit()->set_units(d("1000000").raw());
  *paper->mutable_risk() = protocol::encode_risk({{"max_order_quantity", "100"},
                                                  {"max_gross_quantity", "100"},
                                                  {"max_working_orders", std::uint64_t{100}}});
  const auto costs =
      protocol::encode_cost_schedule(test::cost_schedule({{"margin_per_lot", "100"},
                                                          {"open_fee", "2"},
                                                          {"close_today_fee", "3"},
                                                          {"close_yesterday_fee", "4"},
                                                          {"margin_rate", "0"},
                                                          {"open_fee_rate", "0"},
                                                          {"close_today_fee_rate", "0"},
                                                          {"close_yesterday_fee_rate", "0"}},
                                                         day(0)));
  for (const auto* resolved : {&rb_series, &hc_series}) {
    auto schedule = resolved->schedule;
    for (auto& roll : *schedule.mutable_rolls())
      roll.set_contract(roll.contract() + static_cast<unsigned>(paper->contracts_size()));
    for (const auto& dataset : resolved->datasets) {
      auto* contract = paper->add_contracts();
      *contract->mutable_dataset() = dataset;
      *contract->mutable_cost_schedule() = costs;
      contract->set_slippage_ticks(1);
    }
    *input.add_series() = std::move(schedule);
  }
  // Momentum over one day and over five; 30000 is nearest one lot of either
  // product at ten times its price.
  backtest::v1::BacktestRequest request;
  for (const int lookback : {1, 5})
    *request.add_strategies() = protocol::encode_strategy({{"sides", "both"},
                                                           {"rule",
                                                            {{"kind", "cross_momentum"},
                                                             {"reverse", false},
                                                             {"lookback", lookback},
                                                             {"rebalance", 1},
                                                             {"count", 1},
                                                             {"notional", "30000"},
                                                             {"volatility", 0}}}});
  request.mutable_walk_forward()->set_training_days(20);
  request.mutable_walk_forward()->set_validation_days(10);
  protocol::set_backtest_strategies(input, request);
  input.set_dataset_revision(protocol::dataset_revision(input.paper()));
  const auto result = backtest::run(input);

  // Both series begin on the second day: 59 trading days and four rounds.
  ASSERT_EQ(result.settlements_size(), 59);
  ASSERT_EQ(result.folds_size(), 4);
  for (const auto& fold : result.folds()) {
    EXPECT_EQ(fold.candidates_size(), 2);
    EXPECT_TRUE(fold.has_selected());
  }
  // What the account holds of a contract after the settlement of a day.
  const auto held = [&](int on, const char* symbol) {
    for (const auto& row : result.settlements(on - 1).contracts())
      if (row.symbol() == symbol)
        return row.position_quantity().units();
    return std::int64_t{0};
  };
  const auto one = d("1").raw();
  // Nothing before the first round has a strategy to follow. On its first
  // day the rising rb is bought and the falling hc sold: orders resting at
  // the closes before, 3200 and 3800, filled a price increment worse.
  for (int on = 1; on <= 20; ++on)
    for (const auto* symbol : {"rb2610", "hc2610"})
      EXPECT_EQ(held(on, symbol), 0) << on << symbol;
  EXPECT_EQ(held(21, "rb2610"), one);
  EXPECT_EQ(held(21, "hc2610"), -one);
  const auto price = [](const auto& fill) { return Decimal::from_raw(fill.price().units()).str(); };
  const auto fills = [&](const char* symbol) {
    std::vector<std::string> prices;
    for (const auto& fill : result.account().fills())
      if (fill.symbol() == symbol)
        prices.push_back(price(fill));
    return prices;
  };
  EXPECT_EQ(fills("rb2610").front(), "3201");
  EXPECT_EQ(fills("hc2610").front(), "3799");
  // By day 43 the two have changed places. On day 44 rb rolls on one daily
  // bar: rb2610 is bought back at the open of 3160 and rb2701 sold at its own
  // close before, 3220, each a price increment worse; the account is not
  // flat in rb at the end of that day.
  EXPECT_EQ(held(43, "rb2610"), -one);
  EXPECT_EQ(held(44, "rb2610"), 0);
  EXPECT_EQ(held(44, "rb2701"), -one);
  EXPECT_EQ(fills("rb2610").back(), "3161");
  EXPECT_EQ(fills("rb2701").front(), "3219");
  // And hc on day 52: sold at the open of 3920, bought at 3880.
  EXPECT_EQ(held(51, "hc2610"), one);
  EXPECT_EQ(held(52, "hc2610"), 0);
  EXPECT_EQ(held(52, "hc2701"), one);
  EXPECT_EQ(fills("hc2610").back(), "3919");
  EXPECT_EQ(fills("hc2701").front(), "3881");
  // Two months of a product are never held over a day's end together.
  for (int on = 1; on <= 59; ++on) {
    EXPECT_FALSE(held(on, "rb2610") && held(on, "rb2701")) << on;
    EXPECT_FALSE(held(on, "hc2610") && held(on, "hc2701")) << on;
  }
  // The Task service accepts this result as that of the input and keeps it.
  {
    tasks::Store kept(directory.path / "tasks", tasks::Identity{"task", "historical-data"});
    tasks::submit(kept, "daily", input);
    const auto token = kept.commit(kept.claim("daily")).token();
    kept.commit(tasks::finish(kept, "daily", token, result));
    EXPECT_EQ(kept.get("daily").state(), task::v1::SUCCEEDED);
  }

  // The same series as factor series: momentum reads every day of rb, the
  // term structure the 51 days of hc that have a term point.
  const auto factor_of = [](const data::DominantSeries& resolved, protocol::v1::Factor factor,
                            unsigned lookback) {
    factor::v1::FactorInput value;
    value.set_version(8);
    value.set_factor(factor);
    value.set_full_sample(true);
    value.add_lookbacks(lookback);
    value.set_horizon(1);
    auto* series = value.add_series()->mutable_dominant();
    for (const auto& dataset : resolved.datasets)
      *series->add_months() = dataset;
    *series->mutable_schedule() = resolved.schedule;
    value.set_dataset_revision(protocol::factor_revision(value.series()));
    return value;
  };
  const auto momentum = factor::run(factor_of(rb_series, protocol::v1::PRICE_MOMENTUM, 5));
  EXPECT_EQ(momentum.input_count(), 59U);
  EXPECT_EQ(momentum.samples_size(), 53); // 59 - warmup 5 - horizon 1
  const auto carry = factor::run(factor_of(hc_series, protocol::v1::TERM_STRUCTURE, 1));
  EXPECT_EQ(carry.input_count(), 51U);
  ASSERT_EQ(carry.samples_size(), 49); // 51 - warmup 1 - horizon 1
  // The second day began knowing settlements of 3990 and 3960, three delivery
  // months apart: (3990 / 3960 - 1) x 12 / 3 in eight decimal places.
  EXPECT_EQ(carry.samples(0).event_index(), 1U);
  EXPECT_NEAR(carry.samples(0).value(), 0.03030304, 1e-12);
}
