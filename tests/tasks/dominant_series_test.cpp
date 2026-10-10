#include "bar_dataset_source.hpp"
#include "tasks/paper_input.hpp"
#include "data/history_requests.hpp"
#include "data/bar_fixture.hpp"
#include "engine.hpp"
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
  input.set_version(9);
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
                                                              {"lookback", 1},
                                                              {"rebalance", 1},
                                                              {"count", 1},
                                                              {"notional", "1500"}}}});
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
// A completed daily download of one rb month; daily bars are both its bars
// and its settlements.
data::BarDatasetSources month(data::Store& store, const std::string& delivery,
                              const std::vector<Row>& rows) {
  class Daily final : public HistoricalDailyPort {
  public:
    std::vector<Row> rows;
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
                            price,
                            price,
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
  const auto symbol = "rb" + delivery.substr(2, 2) + delivery.substr(5);
  const auto task = "daily-" + symbol;
  const auto input = asterion::testing_support::daily_request(
      {{"version", 2},
       {"contract_id", "SHFE/rb/" + delivery},
       {"source", "tushare.fut_daily"},
       {"source_instrument", "RB" + delivery.substr(2, 2) + delivery.substr(5) + ".SHF"},
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
                                              {"product", "rb"},
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
  ASSERT_EQ(series.datasets[1].days_size(), 3);
  EXPECT_EQ(series.datasets[1].days(0).trading_day(), days[3]);
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
