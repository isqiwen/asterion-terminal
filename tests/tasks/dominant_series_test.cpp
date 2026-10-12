#include "bar_dataset_source.hpp"
#include "tasks/paper_input.hpp"
#include "data/history_requests.hpp"
#include "data/history_fixture.hpp"
#include "engine.hpp"
#include "factor_engine.hpp"
#include "history_daily.hpp"
#include "task_store.hpp"
#include "tasks/task_store_support.hpp"
#include "data/data_fixture.hpp"
#include <asterion/kernel/process/child.hpp>
#include <asterion/protocol/task_execution.hpp>
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
// One month of a SHFE product in the market-wide daily table: every row is a
// day it traded, opening and closing at its price and ranging `range` either
// side of it, settled at that price with the open interest given.
void month(const std::filesystem::path& root, const std::string& delivery,
           const std::vector<Row>& rows, const std::string& product = "rb", int range = 0) {
  // A data directory begins as the store's; the tables are seeded into it.
  {
    data::Store created(root, "historical-data", "task");
  }
  std::vector<test::DailyRow> days;
  for (const auto& row : rows)
    days.push_back(
        {row.day, row.price, row.open_interest,
         test::DailyTrade{row.price, row.price + range, row.price - range, row.price, 100}});
  (void)test::seed_daily_table(root, "SHFE/" + product + "/" + delivery, days);
}
// The product as the daily table has it once its months are seeded: each
// seeding keeps what the earlier ones stored.
data::v1::ProductBarsRequest product_bars(const data::Store& store, const std::string& product,
                                          const std::string& begin, const std::string& end) {
  return protocol::encode_product_bars_request({{"venue", "SHFE"},
                                                {"product", product},
                                                {"price_increment", "1"},
                                                {"multiplier", "10"},
                                                {"begin_day", begin},
                                                {"end_day", end},
                                                {"daily",
                                                 {{"snapshot", store.tables().head()},
                                                  {"daily_source", "fixture.daily.v1"},
                                                  {"calendar_source", "fixture.calendar.v1"}}}});
}
// Its dominant series of daily bars over [begin, end].
data::DominantSeries series_of(const std::filesystem::path& root, const std::string& product,
                               const std::string& begin, const std::string& end) {
  data::Store store(root, "historical-data", "task");
  return data::resolve_product_series(store.tables(), product_bars(store, product, begin, end));
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
    // Refused for what it is, so that a stored result of another engine reads
    // as one to submit again and not as damaged evidence.
    try {
      store.commit(tasks::finish(store, "rolling", token, old));
      FAIL() << "a result of another engine was accepted";
    } catch (const std::invalid_argument& error) {
      EXPECT_STREQ(error.what(), "unsupported backtest result version");
    }
    EXPECT_EQ(store.get("rolling").state(), task::v1::RUNNING);
    store.commit(tasks::finish(store, "rolling", token, expected));
  }
  tasks::Store restored(directory.path, tasks::Identity{"task", "historical-data"});
  EXPECT_EQ(restored.get("rolling").state(), task::v1::SUCCEEDED);
  EXPECT_EQ(restored.get("rolling").input().SerializeAsString(), input.SerializeAsString());
  EXPECT_EQ(tasks::result(restored, "rolling").SerializeAsString(), expected.SerializeAsString());
}
namespace {
const auto fixed_costs = test::cost_schedule({{"margin_per_lot", "100"},
                                              {"open_fee", "2"},
                                              {"close_today_fee", "3"},
                                              {"close_yesterday_fee", "4"},
                                              {"margin_rate", "0"},
                                              {"open_fee_rate", "0"},
                                              {"close_today_fee_rate", "0"},
                                              {"close_yesterday_fee_rate", "0"}});
// One daily bar of a contract, closing at 100 plus the day and ranging far
// enough for a limit at the close before it to fill.
MarketBar daily_bar(int day) {
  using namespace std::chrono;
  const auto date =
      format_trading_date(year_month_day{sys_days{year{2026} / June / 1} + std::chrono::days{day}});
  const auto close = 100 + day;
  return test::bar(date, 1'790'000'000'000'000'000 + day * 86'400'000'000'000LL,
                   std::to_string(close).c_str(), std::to_string(close + 1).c_str(),
                   std::to_string(close - 2).c_str(), std::to_string(close).c_str());
}
backtest::v1::BacktestInput traded(Json contracts) {
  backtest::v1::BacktestInput input;
  input.set_version(10);
  *input.mutable_paper() =
      testing_support::paper_input({{"version", 4},
                                    {"type", "historical_paper"},
                                    {"risk",
                                     {{"max_order_quantity", "1"},
                                      {"max_gross_quantity", "1"},
                                      {"max_working_orders", std::uint64_t{2}}}},
                                    {"deposit", "100000"},
                                    {"contracts", std::move(contracts)}});
  *input.add_strategies() = testing_support::moving_average(1, 2);
  input.set_dataset_revision(protocol::dataset_revision(input.paper()));
  return input;
}
// A product whose months roll every day: month m is dominant on day m, with a
// bar the day before for a price to take over at and one the day after to be
// closed on. Every month closes at the same price, so every factor is 1.
backtest::v1::BacktestInput rolling_daily(int months) {
  Json contracts = Json::array();
  for (int m = 0; m < months; ++m) {
    std::vector<MarketBar> bars;
    for (int day = std::max(m - 1, 0); day <= std::min(m + 1, months - 1); ++day)
      bars.push_back(daily_bar(day));
    char symbol[8], delivery[8];
    std::snprintf(symbol, sizeof symbol, "cu%02d%02d", 27 + m / 12, m % 12 + 1);
    std::snprintf(delivery, sizeof delivery, "20%02d-%02d", 27 + m / 12, m % 12 + 1);
    contracts.push_back(
        {{"dataset", protocol::decode_bar_dataset(
                         test::dataset(bars, {}, test::contract("SHFE", symbol, "cu", delivery)))},
         {"cost_schedule", fixed_costs}});
  }
  auto input = traded(std::move(contracts));
  auto* series = input.add_series();
  for (int m = 0; m < months; ++m) {
    auto* roll = series->add_rolls();
    roll->set_trading_day(daily_bar(m).trading_day);
    roll->set_contract(static_cast<unsigned>(m));
    roll->mutable_factor()->set_units(d("1").raw());
  }
  return input;
}
} // namespace
// Daily bars give a roll one bar a day to close the month it leaves. When the
// price runs away for two days, the position is closed on the day a bar
// reaches the limit again, which the month's bars must last to.
TEST(DominantSeries, AMonthLeftOnDailyBarsIsClosedOnTheDayItsLimitIsReached) {
  Directory directory;
  {
    data::Store created(directory.path, "historical-data", "task");
  }
  const auto day = [](int index) { return daily_bar(index).trading_day; };
  // The series begins on the second day. rb2610 rises two a day and is bought
  // at 202 on the fourth, the day rb2701 comes to hold more: the fifth is the
  // roll day. rb2610 is to be sold at 204, its close before, but ranges
  // 175-185 that day, and 155-165 on the sixth against a limit of 180. On the
  // seventh it opens at 170, above the limit of 160, and is sold there.
  (void)test::seed_daily_table(directory.path, "SHFE/rb/2026-10",
                               {{day(0), 198, 100, test::DailyTrade{198, 199, 197, 198, 100}},
                                {day(1), 200, 100, test::DailyTrade{200, 201, 199, 200, 100}},
                                {day(2), 202, 100, test::DailyTrade{202, 203, 201, 202, 100}},
                                {day(3), 204, 40, test::DailyTrade{204, 205, 201, 204, 100}},
                                {day(4), 180, 30, test::DailyTrade{180, 185, 175, 180, 100}},
                                {day(5), 160, 20, test::DailyTrade{160, 165, 155, 160, 100}},
                                {day(6), 170, 10, test::DailyTrade{170, 185, 155, 170, 100}}});
  // rb2701 rises two a day from 94, ranging three either side.
  std::vector<test::DailyRow> later;
  for (int i = 0; i < 9; ++i) {
    const auto price = 94 + 2 * i;
    later.push_back({day(i), price, i < 3 ? 10 : 200,
                     test::DailyTrade{price, price + 3, price - 3, price, 100}});
  }
  (void)test::seed_daily_table(directory.path, "SHFE/rb/2027-01", later);
  const auto series = series_of(directory.path, "rb", day(0), day(8));
  ASSERT_EQ(series.schedule.rolls_size(), 2);
  EXPECT_EQ(series.schedule.rolls(1).trading_day(), day(4));
  // rb2610 is read to the last day it trades, not only to the day after the roll.
  EXPECT_EQ(series.datasets[0].days().rbegin()->trading_day(), day(6));
  Json contracts = Json::array();
  for (const auto& dataset : series.datasets)
    contracts.push_back(
        {{"dataset", protocol::decode_bar_dataset(dataset)}, {"cost_schedule", fixed_costs}});
  auto input = traded(std::move(contracts));
  *input.add_series() = series.schedule;
  const auto result = backtest::run(input);
  ASSERT_EQ(result.account().fills_size(), 3);
  const auto fill = [&](int index) {
    const auto& fill = result.account().fills(index);
    return fill.symbol() + " " + Decimal::from_raw(fill.price().units()).str();
  };
  EXPECT_EQ(fill(0), "rb2610 202");
  EXPECT_EQ(fill(1), "rb2610 170");
  // Opened the same day, at rb2701's close of the day before.
  EXPECT_EQ(fill(2), "rb2701 104");
  // 32 lost a lot of ten in rb2610, 6 gained in rb2701 to its last settlement
  // of 110, and fees of 2 to open twice and 4 to close a position of an
  // earlier day.
  EXPECT_EQ(result.account().equity().units(), d("99732").raw());
  EXPECT_EQ(result.settlements_size(), 8);
}
// An exchange settles a cash-settled contract on its last day at an average
// with more places than its prices have: stock index futures trade in steps
// of 0.2 and settle then to two places.
TEST(DailyBars, ASettlementPriceNeedNotBeOnThePriceIncrement) {
  Directory directory;
  {
    data::Store created(directory.path, "historical-data", "task");
  }
  const auto day = [](int index) { return daily_bar(index).trading_day; };
  // Prices in steps of two, rising two a day; the last day settles at 107.
  std::vector<test::DailyRow> rows;
  for (int i = 0; i < 4; ++i) {
    const auto price = 100 + 2 * i;
    rows.push_back({day(i), i == 3 ? 107 : price, 100,
                    test::DailyTrade{price, price + 2, price - 2, price, 100}});
  }
  const auto table = test::seed_daily_table(directory.path, "SHFE/rb/2026-10", rows);
  data::Store store(directory.path, "historical-data", "task");
  data::v1::BarDatasetRequest request;
  *request.mutable_daily() = protocol::encode_daily_table(table);
  *request.mutable_contract() = test::contract();
  request.mutable_contract()->mutable_price_increment()->set_units(d("2").raw());
  request.set_begin_day(day(0));
  request.set_end_day(day(3));
  const auto dataset = data::resolve_bar_dataset(store.tables(), store.sources(request));
  EXPECT_EQ(dataset.days(3).settlement_price().units(), d("107").raw());
  Json contracts = Json::array(
      {{{"dataset", protocol::decode_bar_dataset(dataset)}, {"cost_schedule", fixed_costs}}});
  const auto result = backtest::run(traded(std::move(contracts)));
  // Bought at 102 on the third day, a lot of ten is worth five more at the
  // last settlement; opening it cost 2.
  ASSERT_EQ(result.account().fills_size(), 1);
  EXPECT_EQ(result.account().fills(0).price().units(), d("102").raw());
  EXPECT_EQ(result.account().equity().units(), d("100048").raw());
  // A price a bar trades at is on the increment still.
  auto off = dataset;
  off.mutable_bars(0)->mutable_close()->set_units(d("101").raw());
  off.set_revision(protocol::bar_dataset_revision(off));
  EXPECT_THROW(protocol::validate_bar_dataset(off), std::invalid_argument);
}
TEST(DominantSeries, ASeriesBringsEveryMonthItTradesAndIsOneOfThePortfoliosTwenty) {
  const auto result = backtest::run(rolling_daily(24));
  EXPECT_EQ(result.account().contracts_size(), 24);
  EXPECT_EQ(result.settlements_size(), 24);
  // The series rises one a day, so a lot is held from the third day: bought
  // in the third month, then sold and bought again at each of the 21 rolls.
  ASSERT_EQ(result.account().fills_size(), 43);
  EXPECT_EQ(result.account().fills(0).symbol(), "cu2703");
  EXPECT_EQ(result.account().fills(41).symbol(), "cu2811");
  EXPECT_EQ(result.account().fills(42).symbol(), "cu2812");
  // An account keeps a contract for every month, up to 256 of them.
  EXPECT_NO_THROW(backtest::validate(rolling_daily(256)));
  EXPECT_THROW(rolling_daily(257), std::invalid_argument);
  // Contracts outside a series are each one of the twenty.
  const auto alone = [](int count) {
    Json contracts = Json::array();
    for (int c = 0; c < count; ++c) {
      const std::string product{'a', static_cast<char>('a' + c)};
      contracts.push_back({{"dataset", protocol::decode_bar_dataset(test::dataset(
                                           {daily_bar(0), daily_bar(1)}, {},
                                           test::contract("SHFE", (product + "2610").c_str(),
                                                          product.c_str(), "2026-10")))},
                           {"cost_schedule", fixed_costs}});
    }
    return traded(std::move(contracts));
  };
  EXPECT_NO_THROW(backtest::validate(alone(20)));
  try {
    backtest::validate(alone(21));
    FAIL() << "twenty-one contracts were taken as a portfolio";
  } catch (const std::invalid_argument& error) {
    EXPECT_STREQ(error.what(), "a portfolio holds at most 20 contracts and dominant series");
  }
}
TEST(DominantSeries, ScheduleFollowsThePreviousDaysOpenInterestAndNeverMovesBack) {
  Directory directory;
  // Open interest moves to rb2701 on the third day, so it is dominant from
  // the fourth; rb2610's late surge does not bring it back.
  month(directory.path, "2027-01",
        {{days[0], 100, 10},
         {days[1], 102, 60},
         {days[2], 110, 90},
         {days[3], 111, 95},
         {days[4], 112, 99},
         {days[5], 113, 99}});
  month(directory.path, "2026-10",
        {{days[0], 200, 100},
         {days[1], 204, 100},
         {days[2], 200, 40},
         {days[3], 202, 30},
         {days[4], 204, 1000},
         {days[5], 206, 1000}});
  // The months are the table's own, in delivery order whatever order they
  // were stored in.
  const auto series = series_of(directory.path, "rb", days[0], days[5]);
  ASSERT_EQ(series.months, (std::vector<std::size_t>{0, 1}));
  ASSERT_EQ(series.schedule.rolls_size(), 2);
  EXPECT_EQ(series.schedule.rolls(0).trading_day(), days[1]) << "begins on the second day";
  EXPECT_EQ(series.schedule.rolls(0).contract(), 0U);
  EXPECT_EQ(series.schedule.rolls(1).trading_day(), days[3]);
  EXPECT_EQ(series.schedule.rolls(1).contract(), 1U);
  // 110 / 200 on the day before the roll.
  EXPECT_EQ(series.schedule.rolls(0).factor().units(), d("0.55").raw());
  EXPECT_EQ(series.schedule.rolls(1).factor().units(), d("1").raw());
  // rb2610: from the start to the last day it trades, well after it is left.
  ASSERT_EQ(series.datasets[0].days_size(), 5);
  EXPECT_EQ(series.datasets[0].days(0).trading_day(), days[1]);
  EXPECT_EQ(series.datasets[0].days(4).trading_day(), days[5]);
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
  // A worker reads each fixed month again by itself, over the days it speaks
  // of, and must get the same bytes; a daily bar is of the table's source and
  // of no archive version.
  data::Store store(directory.path, "historical-data", "task");
  for (const auto& dataset : series.datasets) {
    EXPECT_EQ(dataset.interval_minutes(), 0U);
    EXPECT_EQ(dataset.source(), "fixture.daily.v1");
    EXPECT_EQ(dataset.source_dataset_ids_size(), 0);
    const auto again = protocol::bar_dataset_selection(dataset);
    EXPECT_EQ(again.source_dataset_ids_size(), 0);
    EXPECT_EQ(data::resolve_bar_dataset(store.tables(), store.sources(again)).SerializeAsString(),
              dataset.SerializeAsString());
  }
}
TEST(DominantSeries, ResolutionFailsWhereItWouldHaveToGuess) {
  Directory directory;
  month(directory.path, "2027-01",
        {{days[0], 100, 10}, {days[1], 102, 60}, {days[2], 110, 90}, {days[3], 111, 95}});
  // One month that traded is no series.
  EXPECT_THROW(series_of(directory.path, "rb", days[0], days[3]), std::invalid_argument);
  // rb2610 ends the day before rb2701 takes over: nothing to close on.
  month(directory.path, "2026-10", {{days[0], 200, 100}, {days[1], 204, 100}, {days[2], 200, 40}});
  EXPECT_THROW(series_of(directory.path, "rb", days[0], days[3]), std::invalid_argument);
}
// The path a study of several products takes on daily bars: Data resolves each
// product's months as its dominant series, a ranking rule holds one long and
// one short, two windows are compared by rolling, and every fill gives a
// price increment up.
TEST(DominantSeries, DailySeriesOfTwoProductsAreRankedRolledAndComparedByRolling) {
  Directory directory;
  for (const auto* part : {"data", "tasks"})
    std::filesystem::create_directory(directory.path / part);
  const auto data = directory.path / "data";
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
  month(data, "2026-10", rows(rb, 0, 0), "rb", 15);
  month(data, "2027-01", rows(rb, 50, 43), "rb", 15);
  month(data, "2026-10", rows(hc, 0, 0), "hc", 15);
  month(data, "2027-01", rows(hc, -30, 51), "hc", 15);
  const auto rb_series = series_of(data, "rb", day(0), day(59));
  const auto hc_series = series_of(data, "hc", day(0), day(59));
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
