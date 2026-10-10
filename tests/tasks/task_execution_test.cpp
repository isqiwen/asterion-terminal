#include <asterion/protocol/data_client.hpp>
#include "tasks/paper_input.hpp"
#include "performance.hpp"
#include "support/local_listener.hpp"
#include <asterion/protocol/factor.hpp>
#include "history_update.hpp"
#include "sqlite_database.hpp"
#include "task_events.hpp"
#include <asterion/kernel/durable_file.hpp>
#include "engine.hpp"
#include "factor_engine.hpp"
#include "risk_module.hpp"
#include "data/bar_fixture.hpp"
#include "data/history_fixture.hpp"
#include "bar_dataset_source.hpp"
#include "task_store.hpp"
#include "tasks/task_store_support.hpp"
#include "support/timing.hpp"
#include "verification_slots.hpp"
#include <asterion/protocol/task_client.hpp>
#include <asterion/protocol/task_execution.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/child.hpp>
#include <fstream>
#include <cstdlib>
#include <future>
#include <gtest/gtest.h>
using namespace asterion;
namespace {
Decimal d(const char* text) {
  return Decimal::parse(text);
}
// One contract's bars on an account of 10000 with fixed costs, traded by a
// long-only moving average of 1 and 3.
backtest::v1::BacktestInput input_of(const data::v1::BarDataset& dataset) {
  const auto manifest =
      Json{{"version", 4},
           {"type", "historical_paper"},
           {"risk",
            {{"max_order_quantity", "100"},
             {"max_gross_quantity", "100"},
             {"max_working_orders", std::uint64_t{100}}}},
           {"deposit", "10000"},
           {"contracts",
            {{{"dataset", protocol::decode_bar_dataset(dataset)},
              {"cost_schedule", test::cost_schedule({{"margin_per_lot", "100"},
                                                     {"open_fee", "2"},
                                                     {"close_today_fee", "3"},
                                                     {"close_yesterday_fee", "4"},
                                                     {"margin_rate", "0"},
                                                     {"open_fee_rate", "0"},
                                                     {"close_today_fee_rate", "0"},
                                                     {"close_yesterday_fee_rate", "0"}})}}}}};
  backtest::v1::BacktestInput result;
  result.set_version(9);
  *result.mutable_paper() = testing_support::paper_input(manifest);
  *result.add_strategies() = testing_support::moving_average(1, 3);
  result.set_dataset_revision(protocol::dataset_revision(result.paper()));
  return result;
}
backtest::v1::BacktestInput input(const std::vector<int>& prices = {100, 101, 102, 101, 100, 101,
                                                                    103},
                                  const char* settlement = "103") {
  std::vector<MarketBar> bars;
  std::int64_t time = 1790298000000000000LL;
  for (const auto price : prices) {
    bars.push_back(test::flat("2026-09-25", time, std::to_string(price).c_str(), "10"));
    time += 1000000000;
  }
  return input_of(test::dataset(bars, {{"2026-09-25", d(settlement)}}));
}
// Thirty trading days of one bar each, rising two a day with a dip every
// third. A bar opens at the close before it and trades one below, so a limit
// at that close fills. Two moving averages of 1 and 2 are compared on the 25
// days before 2026-10-20: one may only be short, the other only long.
backtest::v1::BacktestInput comparison() {
  using namespace std::chrono;
  std::vector<MarketBar> bars;
  std::vector<DaySettlement> settlements;
  int previous = 100;
  for (int i = 0; i < 30; ++i) {
    const int close = 100 + 2 * i - (i % 3 == 2 ? 3 : 0);
    const auto day =
        format_trading_date(year_month_day{sys_days{year{2026} / September / 25} + days{i}});
    const auto text = [](int value) { return std::to_string(value); };
    bars.push_back(test::bar(day, 1790298000000000000LL + i * 86400LL * 1000000000,
                             text(previous).c_str(), text(std::max(previous, close)).c_str(),
                             text(std::min(previous, close) - 1).c_str(), text(close).c_str(),
                             "10"));
    settlements.push_back({day, d(text(close).c_str())});
    previous = close;
  }
  auto result = input_of(test::dataset(bars, settlements));
  *result.mutable_strategies(0) = testing_support::moving_average(1, 2, "1", "short");
  *result.add_strategies() = testing_support::moving_average(1, 2, "1", "long");
  protocol::set_backtest_holdout(result, "2026-10-20");
  return result;
}
// The bars of a factor input's one series, which is added when there is none yet.
data::v1::BarDataset* factor_bars(factor::v1::FactorInput& input) {
  if (input.series().empty())
    input.add_series();
  return input.mutable_series(0)->mutable_bars();
}
factor::v1::FactorInput maximum_factor_input() {
  factor::v1::FactorInput spec;
  spec.set_version(6);
  spec.set_full_sample(true);
  spec.add_lookbacks(2);
  spec.set_horizon(1);
  auto* dataset = factor_bars(spec);
  *dataset = input().paper().contracts(0).dataset();
  const auto first = dataset->bars(0);
  dataset->clear_bars();
  for (std::size_t i = 0; i < protocol::max_dataset_bars; ++i) {
    auto* bar = dataset->add_bars();
    *bar = first;
    bar->set_timestamp_ns(first.timestamp_ns() + static_cast<std::int64_t>(i) * 1000000);
  }
  dataset->set_revision(protocol::bar_dataset_revision(*dataset));
  spec.set_dataset_revision(dataset->revision());
  return spec;
}
} // namespace
TEST(Backtest, HashUsesExactSnapshotAndExcludesExperimentCosts) {
  EXPECT_EQ(sha256_bytes("abc"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  auto spec = input();
  const auto revision = spec.dataset_revision();
  spec.mutable_paper()->mutable_deposit()->set_units(d("20000").raw());
  EXPECT_EQ(protocol::dataset_revision(spec.paper()), revision);
  spec.mutable_paper()
      ->mutable_contracts(0)
      ->mutable_dataset()
      ->mutable_bars(0)
      ->mutable_close()
      ->set_units(d("99").raw());
  EXPECT_THROW(protocol::dataset_revision(spec.paper()), std::invalid_argument);
  EXPECT_THROW(backtest::validate(spec), std::invalid_argument);
}
TEST(Backtest, DeterministicNextTickExecutionFeesAndDrawdown) {
  const auto spec = input();
  const auto result = backtest::run(spec);
  EXPECT_EQ(result.SerializeAsString(), backtest::run(spec).SerializeAsString());
  ASSERT_EQ(result.account().fills_size(), 2);
  EXPECT_EQ(result.account().fills(0).price().units(), d("101").raw());
  EXPECT_EQ(result.account().fills(1).price().units(), d("101").raw());
  EXPECT_EQ(result.account().fees().units(), d("5").raw());
  EXPECT_EQ(result.account().equity().units(), d("9995").raw());
  EXPECT_EQ(result.max_drawdown().units(), d("12").raw());
  ASSERT_EQ(result.equity_size(), 8);
  EXPECT_EQ(result.equity(2).equity().units(), d("10000").raw());
  EXPECT_EQ(result.equity(3).equity().units(), d("9998").raw());
  // A sell limit at 101 must not fill on the following 100 trade.
  EXPECT_EQ(result.equity(4).equity().units(), d("9988").raw());
  EXPECT_EQ(result.account().frozen().units(), 0);
  EXPECT_EQ(result.account().positions_size(), 0);
}
TEST(Backtest, DailySignalsTradeNextDayWithNewCostsAndYesterdayPositions) {
  auto spec = input();
  std::vector<MarketBar> bars;
  const std::vector<std::string> days{"2026-09-21", "2026-09-22", "2026-09-23", "2026-09-24",
                                      "2026-09-25"};
  const std::vector<const char*> prices{"100", "102", "104", "100", "101"};
  for (std::size_t i = 0; i < days.size(); ++i)
    bars.push_back(test::flat(
        days[i], 1'790'000'000'000'000'000LL + static_cast<std::int64_t>(i) * 86'400'000'000'000LL,
        prices[i]));
  auto* item = spec.mutable_paper()->mutable_contracts(0);
  *item->mutable_dataset() = test::dataset(bars, {}, test::contract(), 0);
  auto* cost = item->mutable_cost_schedule()->add_versions();
  *cost = item->cost_schedule().versions(0);
  cost->set_effective_from(days[3]);
  cost->mutable_values()->mutable_open_fee()->set_units(d("7").raw());
  cost->mutable_values()->mutable_close_yesterday_fee()->set_units(d("9").raw());
  spec.set_dataset_revision(protocol::dataset_revision(spec.paper()));
  const auto result = backtest::run(spec);
  ASSERT_EQ(result.account().fills_size(), 2);
  EXPECT_EQ(result.account().fills(0).price().units(), d("100").raw());
  EXPECT_EQ(result.account().fills(1).price().units(), d("101").raw());
  EXPECT_EQ(result.account().orders(1).offset(), protocol::v1::CLOSE_YESTERDAY);
  EXPECT_EQ(result.settlements(2).contracts(0).position_quantity().units(), 0);
  EXPECT_EQ(result.settlements(3).contracts(0).position_quantity().units(), d("1").raw());
  EXPECT_EQ(result.account().fees().units(), d("16").raw());
  EXPECT_EQ(result.account().equity().units(), d("9994").raw());
  EXPECT_EQ(result.account().positions_size(), 0);
  EXPECT_EQ(result.account().frozen().units(), 0);
  // Submission must use the new day's margin, before it can match.
  cost->mutable_values()->mutable_margin_per_lot()->set_units(d("20000").raw());
  EXPECT_THROW(backtest::run(spec), std::invalid_argument);
}
TEST(Backtest, RejectsUnsupportedDaysAndStopsCooperatively) {
  auto spec = input();
  spec.mutable_paper()->mutable_contracts(0)->mutable_dataset()->mutable_bars(6)->set_trading_day(
      "2026-09-26");
  spec.mutable_paper()->mutable_contracts(0)->mutable_dataset()->mutable_bars(6)->set_timestamp_ns(
      spec.paper().contracts(0).dataset().bars(6).timestamp_ns() + 86400LL * 1000000000);
  spec.mutable_paper()->mutable_contracts(0)->mutable_dataset()->set_revision(
      protocol::bar_dataset_revision(spec.paper().contracts(0).dataset()));
  spec.set_dataset_revision(spec.paper().contracts(0).dataset().revision());
  EXPECT_THROW(backtest::run(spec), std::invalid_argument);
  spec = input();
  spec.mutable_strategies(0)->mutable_moving_average()->set_fast(3);
  EXPECT_THROW(backtest::run(spec), std::invalid_argument);
  spec = input();
  std::stop_source stop;
  std::size_t done = 0;
  EXPECT_THROW(backtest::run(spec, stop.get_token(),
                             [&](auto n, auto) {
                               done = n;
                               stop.request_stop();
                             }),
               std::runtime_error);
  EXPECT_EQ(done, 1U);
}
TEST(Backtest, ShortTargetsSellToOpenAndSettleAsNegativePositions) {
  // The close falls below its average at the third bar; the fourth lets the sell fill.
  auto spec = input({103, 102, 101, 101, 99}, "99");
  spec.mutable_strategies(0)->set_sides(protocol::v1::LONG_AND_SHORT);
  const auto result = backtest::run(spec);
  ASSERT_EQ(result.account().fills_size(), 1);
  EXPECT_EQ(result.account().fills(0).price().units(), d("101").raw());
  // At its target the run sends nothing more.
  ASSERT_EQ(result.account().orders_size(), 1);
  EXPECT_EQ(result.account().orders(0).side(), protocol::v1::SELL);
  EXPECT_EQ(result.account().orders(0).offset(), protocol::v1::OPEN);
  ASSERT_EQ(result.account().positions_size(), 1);
  EXPECT_EQ(result.account().positions(0).side(), protocol::v1::SELL);
  ASSERT_EQ(result.settlements_size(), 1);
  EXPECT_EQ(result.settlements(0).contracts(0).position_quantity().units(), d("-1").raw());
  // One lot sold at 101 and settled at 99, ten per point, less the opening fee.
  EXPECT_EQ(result.account().equity().units(), d("10018").raw());
  // The same bars with long positions only: nothing to hold.
  spec.mutable_strategies(0)->set_sides(protocol::v1::LONG_ONLY);
  const auto flat = backtest::run(spec);
  EXPECT_EQ(flat.account().fills_size(), 0);
  EXPECT_EQ(flat.account().equity().units(), d("10000").raw());
  // No sides is not a strategy.
  spec.mutable_strategies(0)->clear_sides();
  EXPECT_THROW(backtest::run(spec), std::invalid_argument);
}
TEST(Backtest, AReversalBuysTheShortBackBeforeItBuysToOpen) {
  // Short from the fourth bar; the close rises above its average at the seventh.
  auto spec = input({103, 102, 101, 101, 99, 98, 100, 100, 100}, "100");
  spec.mutable_strategies(0)->set_sides(protocol::v1::LONG_AND_SHORT);
  const auto result = backtest::run(spec);
  std::vector<std::pair<protocol::v1::Side, protocol::v1::Offset>> filled;
  for (const auto& order : result.account().orders())
    if (order.state() == protocol::v1::FILLED)
      filled.emplace_back(order.side(), order.offset());
  using Filled = std::vector<std::pair<protocol::v1::Side, protocol::v1::Offset>>;
  EXPECT_EQ(filled, (Filled{{protocol::v1::SELL, protocol::v1::OPEN},
                            {protocol::v1::BUY, protocol::v1::CLOSE_TODAY},
                            {protocol::v1::BUY, protocol::v1::OPEN}}));
  ASSERT_EQ(result.account().positions_size(), 1);
  EXPECT_EQ(result.account().positions(0).side(), protocol::v1::BUY);
  EXPECT_EQ(result.settlements(0).contracts(0).position_quantity().units(), d("1").raw());
  // Sold at 101 and bought back at 100, less an open, a close and another open.
  EXPECT_EQ(result.account().equity().units(), d("10003").raw());
}
TEST(Backtest, ARuleOtherThanAveragesRunsThroughTheSameEngine) {
  auto spec = input();
  *spec.mutable_strategies(0) = protocol::encode_strategy(
      {{"quantity", "1"}, {"sides", "both"}, {"rule", {{"kind", "momentum"}, {"lookback", 2}}}});
  const auto result = backtest::run(spec);
  // 102 is above the close two bars before it: bought at the next bar's 101.
  // 100 is below 102: the long is sold at the following 101, and no later
  // bar lets the short open.
  ASSERT_EQ(result.account().fills_size(), 2);
  EXPECT_EQ(result.account().fills(0).price().units(), d("101").raw());
  EXPECT_EQ(result.account().fills(1).price().units(), d("101").raw());
  EXPECT_EQ(result.account().positions_size(), 0);
  EXPECT_EQ(result.account().equity().units(), d("9995").raw());
  EXPECT_EQ(protocol::decode_backtest(spec, protocol::DatasetView::metadata)
                .at("strategies")
                .at(0)
                .at("rule"),
            Json({{"kind", "momentum"}, {"lookback", 2}}));
  // Seven bars cannot give a rule its first target at the eighth.
  spec.mutable_strategies(0)->mutable_momentum()->set_lookback(7);
  EXPECT_THROW(backtest::run(spec), std::invalid_argument);
}
TEST(Backtest, SeveralStrategiesAreComparedOnTheDaysBeforeTheHoldoutAlone) {
  const auto spec = comparison();
  EXPECT_EQ(spec.holdout_day(), "2026-10-20");
  std::size_t reported = 0, units = 0;
  const auto result = backtest::run(spec, {}, [&](auto done, auto total) {
    EXPECT_GE(done, reported);
    reported = done;
    units = total;
  });
  // Every bar once, and the 25 development bars once more for each strategy.
  EXPECT_EQ(units, 30U + 2 * 25);
  EXPECT_EQ(reported, units);
  EXPECT_EQ(protocol::backtest_work_units(spec), units);
  // In a rising market selling the dips loses and holding the rises gains:
  // the long-only average has the higher ratio and is replayed over all
  // thirty days.
  ASSERT_EQ(result.candidates_size(), 2);
  EXPECT_LT(result.candidates(0).total_return(), 0);
  EXPECT_GT(result.candidates(1).total_return(), 0);
  EXPECT_GT(result.candidates(1).sharpe(), result.candidates(0).sharpe());
  EXPECT_EQ(result.selected(), 1U);
  auto alone = spec;
  alone.mutable_strategies()->DeleteSubrange(0, 1);
  alone.clear_holdout_day();
  const auto single = backtest::run(alone);
  EXPECT_EQ(result.account().SerializeAsString(), single.account().SerializeAsString());
  EXPECT_EQ(result.settlements_size(), 30);
  EXPECT_EQ(single.candidates_size(), 0);
  for (int i = 0; i < result.equity_size(); ++i)
    ASSERT_EQ(result.equity(i).equity().units(), single.equity(i).equity().units());
  // What was recorded of a strategy is what its first 25 days alone show.
  std::vector<EquityDay> days;
  for (int i = 0; i < 25; ++i)
    days.push_back({std::chrono::sys_days(parse_trading_date(single.settlements(i).trading_day())),
                    Decimal::from_raw(single.settlements(i).equity().units())});
  std::vector<Decimal> marks;
  for (int i = 0; i < 50; ++i) // a mark and a settlement for each day
    marks.push_back(Decimal::from_raw(single.equity(i).equity().units()));
  const auto record = performance(d("10000"), days, marks);
  EXPECT_DOUBLE_EQ(result.candidates(1).total_return(), record.total_return);
  EXPECT_DOUBLE_EQ(result.candidates(1).sharpe(), *record.sharpe);
  EXPECT_DOUBLE_EQ(result.candidates(1).max_drawdown(), record.max_drawdown);

  // The holdout decides nothing: other prices from its first day on leave
  // the scores and the choice as they were.
  auto changed = spec;
  auto* dataset = changed.mutable_paper()->mutable_contracts(0)->mutable_dataset();
  for (int i = 25; i < 30; ++i) {
    auto* bar = dataset->mutable_bars(i);
    for (auto* price :
         {bar->mutable_open(), bar->mutable_high(), bar->mutable_low(), bar->mutable_close()})
      price->set_units(d(std::to_string(90 - i).c_str()).raw());
    dataset->mutable_days(i)->mutable_settlement_price()->set_units(
        d(std::to_string(90 - i).c_str()).raw());
  }
  dataset->set_revision(protocol::bar_dataset_revision(*dataset));
  changed.set_dataset_revision(protocol::dataset_revision(changed.paper()));
  const auto other = backtest::run(changed);
  EXPECT_EQ(other.selected(), result.selected());
  for (int i = 0; i < 2; ++i)
    EXPECT_EQ(other.candidates(i).SerializeAsString(), result.candidates(i).SerializeAsString());
  EXPECT_NE(other.account().equity().units(), result.account().equity().units());
}
TEST(Backtest, AComparisonNeedsDifferentStrategiesAndTwentyDaysBeforeItsHoldout) {
  const auto spec = comparison();
  EXPECT_NO_THROW(backtest::validate(spec));
  auto invalid = spec;
  invalid.clear_holdout_day();
  EXPECT_THROW(backtest::validate(invalid), std::invalid_argument);
  invalid = spec;
  invalid.set_holdout_day("2026-10-14"); // the twentieth day: nineteen before it
  EXPECT_THROW(backtest::validate(invalid), std::invalid_argument);
  invalid.set_holdout_day("2026-10-15");
  EXPECT_NO_THROW(backtest::validate(invalid));
  invalid.set_holdout_day("2026-11-30"); // not one of the trading days
  EXPECT_THROW(backtest::validate(invalid), std::invalid_argument);
  invalid = spec;
  *invalid.mutable_strategies(1) = invalid.strategies(0);
  EXPECT_THROW(backtest::validate(invalid), std::invalid_argument);
  invalid = spec;
  invalid.mutable_strategies()->RemoveLast();
  EXPECT_THROW(backtest::validate(invalid), std::invalid_argument) << "one strategy, a holdout";
  // A date that is no trading day begins the holdout on the next one; one
  // strategy takes no date; a date after the data has no day to begin on.
  auto dated = spec;
  protocol::set_backtest_holdout(dated, "2026-10-19");
  EXPECT_EQ(dated.holdout_day(), "2026-10-19");
  EXPECT_THROW(protocol::set_backtest_holdout(dated, "2026-12-01"), std::invalid_argument);
  EXPECT_THROW(protocol::set_backtest_holdout(dated, "soon"), std::invalid_argument);
  dated.mutable_strategies()->RemoveLast();
  EXPECT_THROW(protocol::set_backtest_holdout(dated, "2026-10-19"), std::invalid_argument);
  // Rules that take their first side only after the development days leave
  // the account untouched there: nothing varies, so there is nothing to
  // choose by.
  auto idle = spec;
  for (int i = 0; i < 2; ++i)
    *idle.mutable_strategies(i) =
        protocol::encode_strategy({{"quantity", "1"},
                                   {"sides", "both"},
                                   {"rule", {{"kind", "momentum"}, {"lookback", 28 + i}}}});
  EXPECT_THROW(backtest::run(idle), std::invalid_argument);
}
TEST(Backtest, InputDecodingRejectsCorruptOldOrIncompleteInputs) {
  const auto spec = input();
  const auto json = protocol::decode_backtest(spec);
  auto metadata = json;
  for (auto& contract : metadata.at("paper").at("contracts")) {
    contract.at("dataset").erase("bars");
    contract.at("dataset").erase("days");
  }
  EXPECT_EQ(protocol::decode_backtest(spec, protocol::DatasetView::metadata), metadata);
  auto corrupt = spec;
  corrupt.mutable_paper()->mutable_contracts(0)->mutable_dataset()->mutable_bars(0)->clear_close();
  EXPECT_THROW(protocol::decode_backtest(corrupt, protocol::DatasetView::metadata),
               std::invalid_argument);
  auto old = spec;
  old.set_version(1);
  EXPECT_THROW(protocol::decode_backtest(old), std::invalid_argument);
  auto missing = spec;
  missing.mutable_strategies(0)->clear_quantity();
  EXPECT_THROW(protocol::decode_backtest(missing), std::invalid_argument);
}

namespace {
struct TaskDirectory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() / ("asterion-backtest-factor-" + unique_process_id());
  TaskDirectory() { std::filesystem::create_directory(path); }
  ~TaskDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};
} // namespace
TEST(TaskEventSegments, FailedIndexCommitKeepsActiveEvidenceAndConfirmsOnlyTheOriginalSegment) {
  TaskDirectory directory;
  const auto root = std::filesystem::canonical(directory.path);
  const auto segments = root / "event-segments";
  {
    sqlite::Database database(root / "events.sqlite");
    database.execute("CREATE TABLE task_events(task_id TEXT, sequence INTEGER, body TEXT)");
    sqlite::TaskEvents log(database, segments, false);
    {
      sqlite::Database::Transaction transaction(database);
      for (unsigned sequence = 1; sequence <= 1000; ++sequence)
        log.append("fixture-task", sequence, "fixture event");
      transaction.commit();
    }
    sqlite::fail_next_commits_for_testing(1);
    EXPECT_THROW(log.rotate(), std::runtime_error);
    sqlite::fail_next_commits_for_testing(0);
    ASSERT_TRUE(std::filesystem::exists(segments / "1.pb"));
    const auto hash = sha256_file(segments / "1.pb");
    unsigned count = 0;
    log.read([&](const auto& id, auto sequence, const auto& body) {
      EXPECT_EQ(id, "fixture-task");
      EXPECT_EQ(sequence, ++count);
      EXPECT_EQ(body, "fixture event");
    });
    EXPECT_EQ(count, 1000);
    log.rotate();
    EXPECT_EQ(sha256_file(segments / "1.pb"), hash);
    sqlite::Database::Transaction transaction(database);
    log.append("fixture-task", 1001, "last event");
    transaction.commit();
  }
  sqlite::Database database(root / "events.sqlite", sqlite::Database::Access::read_only);
  sqlite::TaskEvents restored(database, segments, true);
  unsigned count = 0;
  restored.read([&](const auto&, auto sequence, const auto&) { EXPECT_EQ(sequence, ++count); });
  EXPECT_EQ(count, 1001);
  const auto database_hash = sha256_file(root / "events.sqlite");
  replace_file_durably(segments / "1.pb", "corrupted fixture");
  EXPECT_THROW(restored.read([](const auto&, auto, const auto&) {}), std::invalid_argument);
  EXPECT_EQ(sha256_file(root / "events.sqlite"), database_hash);
}

TEST(TaskStore, SegmentedEventsKeepAttemptFencingAndOwnedResultsAcrossRecovery) {
  TaskDirectory directory;
  auto spec = input();
  auto* dataset = spec.mutable_paper()->mutable_contracts(0)->mutable_dataset();
  const auto first = dataset->bars(0);
  dataset->clear_bars();
  for (std::int64_t i = 0; i < 1100; ++i) {
    auto* bar = dataset->add_bars();
    *bar = first;
    bar->set_timestamp_ns(first.timestamp_ns() + i * 1000000000);
  }
  dataset->set_revision(protocol::bar_dataset_revision(*dataset));
  spec.set_dataset_revision(protocol::dataset_revision(spec.paper()));
  const auto expected = backtest::run(spec);
  std::string token, input_hash, result_hash, segment_hash;
  {
    tasks::Store store(directory.path, tasks::Identity{"task", "historical-data"});
    tasks::submit(store, "long-run", spec);
    input_hash = sha256_file(directory.path / "long-run/input.pb");
    token = store.commit(store.claim("long-run")).token();
    for (unsigned completed = 1; completed <= 1001; ++completed)
      store.commit(store.progress("long-run", token, completed));
    store.commit(tasks::finish(store, "long-run", token, expected));
    EXPECT_EQ(store.describe("long-run").state(), task::v1::SUCCEEDED);
    result_hash = sha256_file(directory.path / "long-run/results/1.pb");
    segment_hash = sha256_file(directory.path / "event-segments/1.pb");
  }
  {
    tasks::Store restored(directory.path, tasks::Identity{"task", "historical-data"});
    EXPECT_EQ(restored.describe("long-run").submission_sequence(), 1);
    EXPECT_EQ(restored.describe("long-run").attempt(), 1);
    EXPECT_EQ(tasks::result(restored, "long-run").SerializeAsString(),
              expected.SerializeAsString());
    EXPECT_THROW(restored.commit(restored.progress("long-run", token, 1002)),
                 std::invalid_argument);
    EXPECT_EQ(sha256_file(directory.path / "long-run/input.pb"), input_hash);
    EXPECT_EQ(sha256_file(directory.path / "long-run/results/1.pb"), result_hash);
    EXPECT_EQ(sha256_file(directory.path / "event-segments/1.pb"), segment_hash);
  }
  {
    sqlite::Database database(directory.path / "tasks.sqlite");
    database.execute("DELETE FROM task_events WHERE task_id='long-run' AND sequence="
                     "(SELECT MAX(sequence) FROM task_events WHERE task_id='long-run')");
  }
  const auto database_hash = sha256_file(directory.path / "tasks.sqlite");
  EXPECT_THROW((tasks::Store(directory.path, tasks::Identity{"task", "historical-data"})),
               std::invalid_argument);
  EXPECT_EQ(sha256_file(directory.path / "tasks.sqlite"), database_hash);
}

TEST(TaskStore, HistoryPageCursorDoesNotHideQueuedWorkOrShiftAfterNewSubmissions) {
  TaskDirectory directory;
  tasks::Store store(directory.path, tasks::Identity{"task", "historical-data"});
  const auto spec = input();
  tasks::submit(store, "first", spec);
  tasks::submit(store, "second", spec);
  const auto token = store.commit(store.claim("second")).token();
  store.commit(store.fail("second", token, "fixture failure"));
  tasks::submit(store, "third", spec);
  store.commit(store.cancel("third")).task();
  const auto recent = store.list(2);
  ASSERT_EQ(recent.tasks_size(), 2);
  EXPECT_EQ(recent.tasks(0).id(), "second");
  EXPECT_EQ(recent.tasks(1).id(), "third");
  EXPECT_EQ(recent.next_before_sequence(), 2);
  EXPECT_EQ(recent.failed_count(), 1);
  ASSERT_EQ(recent.active_tasks_size(), 1);
  EXPECT_EQ(recent.active_tasks(0).id(), "first");
  tasks::submit(store, "new-arrival", spec);
  const auto earlier = store.list(2, recent.next_before_sequence());
  ASSERT_EQ(earlier.tasks_size(), 1);
  EXPECT_EQ(earlier.tasks(0).id(), "first");
  EXPECT_EQ(earlier.next_before_sequence(), 0);
  EXPECT_EQ(earlier.failed_count(), 1);
  EXPECT_EQ(earlier.active_tasks_size(), 2);
  task::v1::TaskDispatch allowance;
  allowance.set_launch_slots(2);
  const auto launches = store.dispatch(allowance);
  ASSERT_EQ(launches.launches_size(), 2);
  EXPECT_EQ(launches.launches(0).task_id(), "first");
  EXPECT_EQ(launches.launches(1).task_id(), "new-arrival");
  EXPECT_THROW(store.list(0), std::invalid_argument);
  EXPECT_THROW(store.list(201), std::invalid_argument);
}

TEST(TaskStore, DuplicateSubmissionAndStaleAttemptsAreFenced) {
  TaskDirectory directory;
  tasks::Store store(directory.path, tasks::Identity{"task", "historical-data"});
  auto spec = input();
  const auto first = tasks::submit(store, "job1", spec);
  EXPECT_EQ(tasks::submit(store, "job1", spec).SerializeAsString(), first.SerializeAsString());
  spec.mutable_strategies(0)->mutable_quantity()->set_units(d("2").raw());
  EXPECT_THROW(tasks::submit(store, "job1", spec), std::invalid_argument);
  const auto old = store.commit(store.claim("job1")).token();
  store.commit(store.progress("job1", old, 2));
  EXPECT_THROW(store.commit(store.progress("job1", old, 1)), std::invalid_argument);
  EXPECT_EQ(store.commit(store.cancel("job1")).task().state(), task::v1::CANCEL_REQUESTED);
  store.commit(tasks::finish(store, "job1", old, backtest::run(input())));
  EXPECT_EQ(store.get("job1").state(), task::v1::CANCELLED);
  EXPECT_THROW(tasks::result(store, "job1"), std::invalid_argument);
  store.commit(store.retry("job1")).task();
  const auto next = store.commit(store.claim("job1")).token();
  EXPECT_NE(old, next);
  EXPECT_THROW(store.commit(tasks::finish(store, "job1", old, backtest::run(input()))),
               std::invalid_argument);
  store.commit(tasks::finish(store, "job1", next, backtest::run(input())));
  EXPECT_EQ(store.get("job1").attempt(), 2U);
  EXPECT_EQ(store.get("job1").state(), task::v1::SUCCEEDED);
  EXPECT_EQ(tasks::result(store, "job1").SerializeAsString(),
            backtest::run(input()).SerializeAsString());
  EXPECT_THROW(store.commit(store.retry("job1")).task(), std::invalid_argument);
}
TEST(TaskStore, DispatchOwnsSubmissionOrderCapacityAndDoesNotClaimBeforeLaunch) {
  TaskDirectory directory;
  tasks::Store store(directory.path, tasks::Identity{"task", "historical-data"});
  for (const auto* id : {"z-first", "a-second", "m-third"})
    tasks::submit(store, id, input());
  task::v1::TaskDispatch processes;
  EXPECT_THROW(store.dispatch(processes), std::invalid_argument);
  processes.set_launch_slots(0);
  EXPECT_EQ(store.dispatch(processes).launches_size(), 0);
  processes.set_launch_slots(1);
  ASSERT_EQ(store.dispatch(processes).launches_size(), 1);
  EXPECT_EQ(store.dispatch(processes).launches(0).task_id(), "z-first");
  processes.set_launch_slots(2);
  auto launches = store.dispatch(processes);
  ASSERT_EQ(launches.launches_size(), 2);
  EXPECT_EQ(launches.launches(0).task_id(), "z-first");
  EXPECT_EQ(launches.launches(1).task_id(), "a-second");
  EXPECT_EQ(launches.launches(0).program(), task::v1::BACKTEST_PROGRAM);

  EXPECT_EQ(store.dispatch(processes, false).launches_size(), 0);
  EXPECT_EQ(store.get("z-first").state(), task::v1::QUEUED);
  EXPECT_EQ(store.get("z-first").attempt(), 0U);
  // An OS process may be running before it claims its attempt.
  processes.add_running("z-first");
  launches = store.dispatch(processes);
  ASSERT_EQ(launches.launches_size(), 1);
  EXPECT_EQ(launches.launches(0).task_id(), "a-second");
  processes.add_running("a-second");
  EXPECT_EQ(store.dispatch(processes).launches_size(), 0);
  processes.clear_running();
  store.commit(store.cancel("z-first")).task();
  const auto token = store.commit(store.claim("a-second")).token();
  launches = store.dispatch(processes);
  ASSERT_EQ(launches.launches_size(), 1);
  EXPECT_EQ(launches.launches(0).task_id(), "m-third");
  store.commit(store.interrupt("a-second", token, "test worker exited"));
  EXPECT_EQ(store.dispatch(processes).launches_size(), 1);
  store.commit(store.retry("a-second")).task();
  launches = store.dispatch(processes);
  EXPECT_EQ(launches.launches(0).task_id(), "a-second");
  processes.add_running("a-second");
  processes.add_running("a-second");
  EXPECT_THROW(store.dispatch(processes), std::invalid_argument);
}
TEST(TaskStore, RestartRetainsQueueAndResultsButInterruptsUnconfirmedWork) {
  TaskDirectory directory;
  {
    tasks::Store store(directory.path, tasks::Identity{"task", "historical-data"});
    tasks::submit(store, "queued", input());
    tasks::submit(store, "working", input());
    tasks::submit(store, "done", input());
    const auto token = store.commit(store.claim("working")).token();
    store.commit(store.progress("working", token, 2));
    store.commit(tasks::finish(store, "done", store.commit(store.claim("done")).token(),
                               backtest::run(input())));
    EXPECT_THROW(tasks::Store other(directory.path, tasks::Identity{"task", "historical-data"}),
                 std::runtime_error);
  }
  {
    tasks::Store restored(directory.path, tasks::Identity{"task", "historical-data"});
    EXPECT_EQ(restored.get("queued").state(), task::v1::QUEUED);
    EXPECT_EQ(restored.get("working").state(), task::v1::INTERRUPTED);
    EXPECT_EQ(restored.get("working").completed(), 2U);
    EXPECT_EQ(restored.get("done").state(), task::v1::SUCCEEDED);
    EXPECT_EQ(tasks::result(restored, "done").SerializeAsString(),
              backtest::run(input()).SerializeAsString());
    restored.commit(restored.retry("working")).task();
    EXPECT_EQ(restored.get("working").state(), task::v1::QUEUED);
  }
  tasks::Store again(directory.path, tasks::Identity{"task", "historical-data"});
  EXPECT_EQ(again.get("working").state(), task::v1::QUEUED);
}
TEST(TaskStore, InstanceBindingRejectsWrongOwnerBeforeRecovery) {
  TaskDirectory directory;
  const tasks::Identity identity{"task-one", "data-one"};
  {
    tasks::Store store(directory.path, identity);
    tasks::submit(store, "working", input());
    (void)store.commit(store.claim("working")).token();
  }
  const auto database = directory.path / "tasks.sqlite";
  const auto seal = directory.path / "instance.json";
  const auto database_hash = sha256_file(database);
  const auto seal_hash = sha256_file(seal);
  for (const tasks::Identity wrong :
       {tasks::Identity{"task-two", "data-one"}, tasks::Identity{"task-one", "data-two"}}) {
    EXPECT_THROW(tasks::Store rejected(directory.path, wrong), std::invalid_argument);
    EXPECT_THROW(tasks::Store::inspect_history_usage(directory.path, wrong, std::string(64, 'a')),
                 std::invalid_argument);
    EXPECT_EQ(sha256_file(database), database_hash);
    EXPECT_EQ(sha256_file(seal), seal_hash);
  }
  tasks::Store restored(directory.path, identity);
  EXPECT_EQ(restored.get("working").state(), task::v1::INTERRUPTED);
  EXPECT_EQ(sha256_file(seal), seal_hash);
}

TEST(TaskStore, MissingOrUnsupportedIdentityIsNeverFilledIntoExistingStore) {
  for (const bool missing : {true, false}) {
    SCOPED_TRACE(missing);
    TaskDirectory directory;
    const tasks::Identity identity{"task-one", "data-one"};
    {
      tasks::Store store(directory.path, identity);
      tasks::submit(store, "working", input());
      (void)store.commit(store.claim("working")).token();
    }
    const auto seal = directory.path / "instance.json";
    if (missing)
      std::filesystem::remove(seal);
    else
      replace_file_durably(seal, Json{{"version", 0},
                                      {"task_instance", identity.instance},
                                      {"data_instance", identity.data_instance}}
                                     .dump());
    const auto database_hash = sha256_file(directory.path / "tasks.sqlite");
    const auto seal_hash = missing ? std::string{} : sha256_file(seal);
    EXPECT_THROW(tasks::Store rejected(directory.path, identity), std::invalid_argument);
    EXPECT_THROW(
        tasks::Store::inspect_history_usage(directory.path, identity, std::string(64, 'a')),
        std::invalid_argument);
    EXPECT_EQ(sha256_file(directory.path / "tasks.sqlite"), database_hash);
    EXPECT_EQ(std::filesystem::exists(seal), !missing);
    if (!missing)
      EXPECT_EQ(sha256_file(seal), seal_hash);
  }
}

TEST(TaskStore, ModifiedResultNeverLoadsAsSuccess) {
  TaskDirectory directory;
  {
    tasks::Store store(directory.path, tasks::Identity{"task", "historical-data"});
    tasks::submit(store, "done", input());
    store.commit(tasks::finish(store, "done", store.commit(store.claim("done")).token(),
                               backtest::run(input())));
  }
  {
    std::ofstream file(directory.path / "done" / "results" / "1.pb", std::ios::app);
    file << " ";
  }
  EXPECT_THROW(tasks::Store corrupted(directory.path, tasks::Identity{"task", "historical-data"}),
               std::invalid_argument);
}
TEST(TaskStore, DirectorySyncFailureCannotCommitTaskSubmissionOrSuccessfulResult) {
  {
    TaskDirectory directory;
    const tasks::Identity identity{"task-one", "data-one"};
    fail_next_directory_syncs_for_testing(1);
    EXPECT_THROW(tasks::Store store(directory.path, identity), std::runtime_error);
    fail_next_directory_syncs_for_testing(0);
    EXPECT_FALSE(std::filesystem::exists(directory.path / "tasks.sqlite"));
    tasks::Store restored(directory.path, identity);
    EXPECT_TRUE(restored.list().tasks().empty());
  }
  {
    TaskDirectory directory;
    tasks::Store store(directory.path, tasks::Identity{"task", "historical-data"});
    fail_next_directory_syncs_for_testing(1);
    EXPECT_THROW(tasks::submit(store, "unpublished", input()), std::runtime_error);
    fail_next_directory_syncs_for_testing(0);
    EXPECT_TRUE(store.list().tasks().empty());
    EXPECT_EQ(store.list().capacity().retained_tasks(), 0U);
    EXPECT_EQ(store.list().capacity().uncommitted(), 1U);
    EXPECT_EQ(store.list().capacity().active_reserved(), 0U);
    EXPECT_EQ(store.list().capacity().active_limit(), 1000U);
  }
  TaskDirectory directory;
  {
    tasks::Store store(directory.path, tasks::Identity{"task", "historical-data"});
    tasks::submit(store, "job", input());
    const auto token = store.commit(store.claim("job")).token();
    const auto result = backtest::run(input());
    fail_next_directory_syncs_for_testing(1);
    EXPECT_THROW(store.commit(tasks::finish(store, "job", token, result)), std::runtime_error);
    fail_next_directory_syncs_for_testing(0);
    EXPECT_EQ(store.get("job").state(), task::v1::RUNNING);
    EXPECT_THROW(tasks::result(store, "job"), std::invalid_argument);
  }
  tasks::Store restored(directory.path, tasks::Identity{"task", "historical-data"});
  EXPECT_EQ(restored.get("job").state(), task::v1::INTERRUPTED);
  EXPECT_TRUE(std::filesystem::is_regular_file(directory.path / "job/results/1.pb"));
  restored.commit(restored.retry("job")).task();
  restored.commit(tasks::finish(restored, "job", restored.commit(restored.claim("job")).token(),
                                backtest::run(input())));
  EXPECT_EQ(restored.get("job").state(), task::v1::SUCCEEDED);
}
TEST(TaskStore, SubmissionPreparationLeavesConfirmedStateAvailable) {
  TaskDirectory directory;
  tasks::Store store(directory.path, tasks::Identity{"task", "historical-data"});
  tasks::submit(store, "existing", input());
  auto pending = store.submission("new", input());
  store.admit_submission(pending);
  EXPECT_TRUE(store.has_submission());
  EXPECT_FALSE(std::filesystem::exists(directory.path / "new"));
  EXPECT_EQ(store.list().capacity().active_reserved(), 1U);
  EXPECT_EQ(store.list().capacity().active_used(), 1U);
  EXPECT_EQ(store.list().capacity().retained_tasks(), 1U);
  EXPECT_EQ(store.list().capacity().uncommitted(), 0U);
  auto prepared = std::async(std::launch::async, [&] { pending.prepare_files(); });
  store.commit(store.cancel("existing"));
  prepared.get();
  EXPECT_TRUE(store.active_tasks().empty());
  auto registered = store.register_submission(pending);
  auto persisted = std::async(std::launch::async, [&] { registered.persist(); });
  persisted.get();
  EXPECT_TRUE(store.active_tasks().empty());
  EXPECT_TRUE(store.has_submission());
  store.confirm(registered);
  EXPECT_FALSE(store.has_submission());
  EXPECT_EQ(store.describe("new").submission_sequence(), 2U);
  EXPECT_EQ(store.list().capacity().active_reserved(), 0U);
  EXPECT_EQ(store.list().capacity().active_used(), 1U);

  // Duplicate input verification cannot restore an obsolete state snapshot.
  auto duplicate = store.submission("new", input());
  store.admit_submission(duplicate);
  auto verified = std::async(std::launch::async, [&] { duplicate.prepare_files(); });
  store.commit(store.cancel("new"));
  verified.get();
  EXPECT_EQ(store.commit(store.register_submission(duplicate)).task().state(), task::v1::CANCELLED);
  EXPECT_TRUE(store.active_tasks().empty());
}
TEST(TaskStore, JournalWaitKeepsActiveStateAvailableAndRequiresOwnerConfirmation) {
  using namespace std::chrono_literals;
  TaskDirectory directory;
  tasks::Store store(directory.path, tasks::Identity{"task", "historical-data"});
  tasks::submit(store, "job", input());
  tasks::submit(store, "other", input());
  auto claimed = store.claim("job");
  sqlite::hold_commits_for_testing(true);
  auto persisted = std::async(std::launch::async, [&] { claimed.persist(); });
  struct Release {
    ~Release() { sqlite::hold_commits_for_testing(false); }
  } release;
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (!sqlite::commit_waiting_for_testing() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  ASSERT_TRUE(sqlite::commit_waiting_for_testing());
  EXPECT_EQ(store.describe("job").state(), task::v1::QUEUED);
  EXPECT_EQ(store.active_tasks().size(), 2U);
  // Another task can be decided while the writer is held at its durable barrier.
  auto cancelled = store.cancel("other");
  EXPECT_EQ(cancelled.task().state(), task::v1::CANCELLED);
  EXPECT_EQ(store.describe("other").state(), task::v1::QUEUED);
  sqlite::hold_commits_for_testing(false);
  persisted.get();
  EXPECT_EQ(store.describe("job").state(), task::v1::QUEUED);
  store.confirm(claimed);
  EXPECT_EQ(store.describe("job").state(), task::v1::RUNNING);
  store.commit(std::move(cancelled));
  ASSERT_EQ(store.active_tasks().size(), 1U);
  EXPECT_EQ(store.active_tasks().front().id(), "job");
}
TEST(TaskStore, FailedStateCommitKeepsTheLastConfirmedActiveState) {
  TaskDirectory directory;
  {
    tasks::Store store(directory.path, tasks::Identity{"task", "historical-data"});
    tasks::submit(store, "job", input());
    const auto token = store.commit(store.claim("job")).token();
    store.commit(store.progress("job", token, 1));
    const auto confirmed = store.describe("job").SerializeAsString();
    sqlite::fail_next_commits_for_testing(1);
    EXPECT_THROW(store.commit(store.cancel("job")).task(), std::runtime_error);
    sqlite::fail_next_commits_for_testing(0);
    EXPECT_EQ(store.describe("job").SerializeAsString(), confirmed);
    const auto active = store.active_tasks();
    ASSERT_EQ(active.size(), 1);
    EXPECT_EQ(active.front().SerializeAsString(), confirmed);
    EXPECT_EQ(store.list().capacity().active_used(), 1U);
    EXPECT_THROW(store.commit(store.progress("job", token, 2)), std::runtime_error);
  }
  tasks::Store restored(directory.path, tasks::Identity{"task", "historical-data"});
  EXPECT_EQ(restored.describe("job").state(), task::v1::INTERRUPTED);
  EXPECT_EQ(restored.describe("job").completed(), 1U);
  EXPECT_TRUE(restored.active_tasks().empty());
  restored.commit(restored.retry("job")).task();
  EXPECT_EQ(restored.active_tasks().size(), 1U);
}
TEST(TaskStore, PreparedPayloadCannotBypassCancellationOrANewerAttempt) {
  for (const bool retry : {false, true}) {
    SCOPED_TRACE(retry);
    TaskDirectory directory;
    tasks::Store store(directory.path, tasks::Identity{"task", "historical-data"});
    tasks::submit(store, "job", input());
    task::v1::TaskFinish request;
    request.set_id("job");
    request.set_token(store.commit(store.claim("job")).token());
    *request.mutable_result() = backtest::run(input());
    auto pending = store.prepare_finish(request);
    EXPECT_THROW(store.commit(store.finish(pending)), std::invalid_argument);
    // File preparation owns only copies. The state owner can process progress,
    // cancellation and a new attempt while the old attempt writes its payload.
    auto preparation = std::async(std::launch::async, [&] { pending.prepare_payload(); });
    store.commit(store.progress("job", request.token(), 1));
    store.commit(store.cancel("job")).task();
    if (retry) {
      store.commit(store.acknowledge_cancel("job", request.token()));
      store.commit(store.retry("job")).task();
      EXPECT_NE(store.commit(store.claim("job")).token(), request.token());
    }
    preparation.get();
    EXPECT_TRUE(store.describe("job").result_digest().empty());
    if (retry) {
      EXPECT_THROW(store.commit(store.finish(std::move(pending))), std::invalid_argument);
      EXPECT_EQ(store.get("job").state(), task::v1::RUNNING);
      EXPECT_EQ(store.get("job").attempt(), 2U);
    } else {
      store.commit(store.finish(std::move(pending)));
      EXPECT_EQ(store.get("job").state(), task::v1::CANCELLED);
    }
    EXPECT_TRUE(std::filesystem::exists(directory.path / "job" / "results" / "1.pb"));
    EXPECT_THROW(tasks::result(store, "job"), std::invalid_argument);
  }
}
TEST(TaskStore, PreparedPayloadIsOwnedAndExpiredAttemptCannotCommit) {
  TaskDirectory directory;
  tasks::Store store(directory.path, tasks::Identity{"task", "historical-data"});
  tasks::submit(store, "job", input());
  task::v1::TaskFinish request;
  request.set_id("job");
  request.set_token(store.commit(store.claim("job")).token());
  *request.mutable_result() = backtest::run(input());
  auto pending = store.prepare_finish(request);
  request.mutable_result()->set_dataset_revision("changed.after.prepare");
  EXPECT_NO_THROW(pending.prepare_payload());
  store.commit(store.interrupt("job", request.token(), "test lease expired"));
  EXPECT_THROW(store.commit(store.finish(std::move(pending))), std::invalid_argument);
  EXPECT_EQ(store.get("job").state(), task::v1::INTERRUPTED);
  EXPECT_TRUE(std::filesystem::exists(directory.path / "job" / "results" / "1.pb"));
  EXPECT_THROW(tasks::result(store, "job"), std::invalid_argument);
}
TEST(TaskStore, InputReadPreservesConcurrentCancellationAndRejectsForeignOrChangedFiles) {
  TaskDirectory directory;
  tasks::Store store(directory.path, tasks::Identity{"task", "historical-data"});
  tasks::submit(store, "job", input());
  auto read = store.prepare_input("job");
  EXPECT_THROW(store.confirm_input(read), std::invalid_argument);
  EXPECT_THROW((void)store.claim(read), std::invalid_argument);
  auto loading = std::async(std::launch::async, [&] { read.load(); });
  store.commit(store.cancel("job")).task();
  loading.get();
  TaskDirectory foreign_directory;
  tasks::Store foreign(foreign_directory.path, tasks::Identity{"task", "historical-data"});
  tasks::submit(foreign, "job", input());
  EXPECT_THROW(foreign.confirm_input(read), std::invalid_argument);
  EXPECT_THROW((void)foreign.claim(read), std::invalid_argument);
  EXPECT_THROW((void)store.claim(read), std::invalid_argument);
  const auto value = store.confirm_input(std::move(read));
  EXPECT_EQ(value.state(), task::v1::CANCELLED);
  EXPECT_EQ(value.input().SerializeAsString(), input().SerializeAsString());
  EXPECT_THROW(store.commit(store.claim("job")).token(), std::invalid_argument);
  auto changed = store.prepare_input("job");
  std::ofstream(directory.path / "job/input.pb", std::ios::binary | std::ios::trunc) << "bad";
  EXPECT_THROW(changed.load(), std::invalid_argument);
  EXPECT_THROW(store.confirm_input(std::move(changed)), std::invalid_argument);
}
TEST(TaskStore, ResultReadOwnsItsSnapshotAndChecksFilesBeforeCacheConfirmation) {
  TaskDirectory directory;
  const auto expected = backtest::run(input());
  {
    tasks::Store store(directory.path, tasks::Identity{"task", "historical-data"});
    tasks::submit(store, "done", input());
    store.commit(tasks::finish(store, "done", store.commit(store.claim("done")).token(), expected));
  }
  tasks::Store store(directory.path, tasks::Identity{"task", "historical-data"});
  tasks::submit(store, "other", input());
  const auto token = store.commit(store.claim("other")).token();
  auto read = store.prepare_result("done");
  EXPECT_THROW(store.confirm_result(read), std::invalid_argument);
  auto verifying = std::async(std::launch::async, [&] { read.verify(); });
  store.commit(store.progress("other", token, 1));
  store.commit(store.cancel("other")).task();
  verifying.get();
  TaskDirectory other_directory;
  tasks::Store other(other_directory.path, tasks::Identity{"task", "historical-data"});
  tasks::submit(other, "done", input());
  other.commit(tasks::finish(other, "done", other.commit(other.claim("done")).token(), expected));
  EXPECT_THROW(other.confirm_result(read), std::invalid_argument);
  const auto response = store.confirm_result(std::move(read));
  EXPECT_EQ(response.backtest().SerializeAsString(), expected.SerializeAsString());
  EXPECT_EQ(response.result_task().input().SerializeAsString(), input().SerializeAsString());
  auto cached = store.prepare_result("done");
  std::ofstream(directory.path / "done/input.pb", std::ios::binary | std::ios::trunc) << "bad";
  EXPECT_THROW(cached.verify(), std::invalid_argument);
  EXPECT_THROW(store.confirm_result(std::move(cached)), std::invalid_argument);
}
TEST(TaskStore, VerificationAdmissionReservesBothWorkersAndReleasesAfterFailure) {
  tasks::VerificationSlots slots;
  {
    auto read = slots.acquire(false, "read");
    auto first = slots.acquire(true, "first");
    auto second = slots.acquire(true, "second");
    EXPECT_THROW(slots.acquire(false, "another-read"), Error);
    EXPECT_THROW(slots.acquire(true, "third"), Error);
    auto moved = std::move(second);
    EXPECT_THROW(slots.acquire(true, "second"), Error);
  }
  EXPECT_THROW(
      {
        auto read = slots.acquire(false, "failed-read");
        throw std::runtime_error("verification failed");
      },
      std::runtime_error);
  EXPECT_NO_THROW(slots.acquire(false, "next-read"));
  auto first = slots.acquire(true, "first");
  EXPECT_THROW(slots.acquire(true, "first"), Error);
  EXPECT_NO_THROW(slots.acquire(true, "second"));
}
TEST(TaskStore, RepeatedVerifiedReadsStillDetectChangedResultBytes) {
  TaskDirectory directory;
  tasks::Store store(directory.path, tasks::Identity{"task", "historical-data"});
  tasks::submit(store, "done", input());
  const auto expected = backtest::run(input());
  store.commit(tasks::finish(store, "done", store.commit(store.claim("done")).token(), expected));
  for (int i = 0; i < 3; ++i)
    EXPECT_EQ(tasks::result(store, "done").SerializeAsString(), expected.SerializeAsString());
  {
    std::ofstream file(directory.path / "done" / "results" / "1.pb", std::ios::app);
    file << " ";
  }
  EXPECT_THROW(tasks::result(store, "done"), std::invalid_argument);
}
TEST(TaskStore, InvalidInputAndQueuedCancellationDoNotRunAnything) {
  TaskDirectory directory;
  tasks::Store store(directory.path, tasks::Identity{"task", "historical-data"});
  auto spec = input();
  spec.set_dataset_revision("invalid");
  EXPECT_THROW(tasks::submit(store, "invalid", spec), std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(directory.path / "invalid"));
  EXPECT_THROW(tasks::submit(store, "../outside", input()), std::exception);
  tasks::submit(store, "queued", input());
  EXPECT_EQ(store.commit(store.cancel("queued")).task().state(), task::v1::CANCELLED);
  EXPECT_THROW(store.commit(store.claim("queued")).token(), std::invalid_argument);
  EXPECT_EQ(store.get("queued").attempt(), 0U);
}

#include <asterion/kernel/ipc/local_channel.hpp>
#include <thread>
namespace {
using namespace std::chrono_literals;
namespace task_wire = task::v1;
struct TaskProcess : testing::Test {
  TaskDirectory directory, warehouse;
  std::unique_ptr<ChildProcess> service, data_service;
  std::filesystem::path sockets;
  std::string endpoint;
  unsigned lease_seconds = 2;
  void SetUp() override {
    sockets = std::filesystem::path("/tmp") / ("ast-r-" + unique_process_id().substr(0, 12));
    std::filesystem::create_directory(sockets);
    std::filesystem::permissions(sockets, std::filesystem::perms::owner_all);
    endpoint = (sockets / "task.sock").string();
    start();
  }
  void TearDown() override {
    service.reset();
    data_service.reset();
    if (!sockets.empty()) {
      std::error_code ec;
      std::filesystem::remove_all(sockets, ec);
    }
  }
  task_wire::TaskResponse call(task_wire::TaskRequest request, const std::string& target = "",
                               std::chrono::milliseconds timeout = 2s) {
    request.set_version(1);
    request.set_service_id("task");
    request.set_correlation_id(unique_process_id());
    auto channel = ipc::Channel::connect(target.empty() ? endpoint : target, 2s);
    channel.send(request.SerializeAsString(), 2s);
    task_wire::TaskResponse response;
    if (!response.ParseFromString(channel.receive(timeout)))
      throw std::runtime_error("bad task response");
    protocol::validate_message(response);
    if (response.version() != 1 || response.service_id() != request.service_id() ||
        response.correlation_id() != request.correlation_id())
      throw std::runtime_error("wrong task response identity");
    if (response.has_error())
      throw std::runtime_error(response.error().message());
    return response;
  }
  void start_data() {
    data_service = std::make_unique<ChildProcess>(
        ASTERION_DATA_SERVICE_PATH,
        std::vector<std::string>{"--directory", warehouse.path.string(), "--session",
                                 "historical-data", "--task-instance", "task", "--endpoint",
                                 endpoint + ".data", "--worker-endpoint",
                                 endpoint + ".data-workers", "--file-workers", "1"});
    protocol::DataClient client(endpoint + ".data", "historical-data");
    const auto until = std::chrono::steady_clock::now() + 10s;
    for (;;) {
      if (std::chrono::steady_clock::now() >= until)
        throw std::runtime_error("data fixture did not initialize");
      try {
        data::v1::DataRequest request;
        request.mutable_heartbeat();
        if (client.call(request, 1s).health().initialized())
          return;
      } catch (const Error&) {
        if (data_service->exited() || std::chrono::steady_clock::now() >= until)
          throw;
      }
      std::this_thread::sleep_for(20ms);
    }
  }
  void launch() {
    std::filesystem::remove(endpoint); // This test exclusively owns the private directory.
    std::filesystem::remove(endpoint + ".worker");
    std::filesystem::remove(endpoint + ".health");
    const auto directory_utf8 = directory.path.u8string();
    service = std::make_unique<ChildProcess>(
        ASTERION_TASK_SERVICE_PATH,
        std::vector<std::string>{
            "--directory", std::string(directory_utf8.begin(), directory_utf8.end()), "--endpoint",
            endpoint, "--worker-endpoint", endpoint + ".worker", "--health-endpoint",
            endpoint + ".health", "--session", "task", "--data-instance", "historical-data",
            "--data-endpoint", endpoint + ".data-workers", "--worker-timeout",
            std::to_string(lease_seconds), "--file-workers", "1"});
  }
  void start(std::chrono::milliseconds timeout = 10s) {
    launch();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
      try {
        task_wire::TaskRequest req;
        req.mutable_heartbeat();
        if (call(req).health().initialized())
          break;
      } catch (const std::exception&) {
        if (service->exited())
          throw;
      }
      if (std::chrono::steady_clock::now() > deadline)
        throw std::runtime_error("task fixture did not initialize");
      std::this_thread::sleep_for(20ms);
    }
  }
  std::optional<backtest::v1::BacktestInput> fixed_input;
  data::v1::BarDataset published_dataset(const std::vector<int>& prices, const std::string& id) {
    data_service.reset();
    std::vector<std::string> days;
    const auto first = std::chrono::sys_days(std::chrono::year(2024) / 1 / 1);
    for (std::size_t day = 0; day < (prices.size() + 239) / 240; ++day)
      days.push_back(
          format_trading_date(std::chrono::year_month_day(first + std::chrono::days(day))));
    const auto selection = test::seed_history(warehouse.path, prices, id, days);
    data::v1::BarDatasetRequest request;
    for (const auto& version : selection.at("source_dataset_ids"))
      request.add_source_dataset_ids(version.get<std::string>());
    for (const auto& version : selection.at("settlement_dataset_ids"))
      request.add_settlement_dataset_ids(version.get<std::string>());
    *request.mutable_contract() = test::contract();
    data::v1::BarDataset dataset;
    {
      data::Store store(warehouse.path, "historical-data", "task");
      dataset = data::resolve_bar_dataset(store.sources(request));
    }
    start_data();
    return dataset;
  }
  const backtest::v1::BacktestInput& process_input() {
    if (!fixed_input) {
      fixed_input = input();
      *fixed_input->mutable_paper()->mutable_contracts(0)->mutable_dataset() =
          published_dataset({100, 101, 102, 101, 100, 101, 103}, "worker-input");
      fixed_input->set_dataset_revision(protocol::dataset_revision(fixed_input->paper()));
    }
    return *fixed_input;
  }
  void submit(const std::string& id) {
    service.reset();
    {
      tasks::Store store(directory.path, tasks::Identity{"task", "historical-data"});
      tasks::submit(store, id, process_input());
    }
    start();
  }
};
} // namespace
TEST_F(TaskProcess, WorkerRunsWithoutSubmittingClientAndResultSurvivesRestart) {
  submit("run1"); // Every request closes its connection; no UI client remains.
  ChildProcess worker(ASTERION_BACKTEST_PATH,
                      {"--endpoint", endpoint, "--session", "task", "--task", "run1"});
  ASSERT_TRUE(worker.wait(15s));
  ASSERT_EQ(worker.exit_code(), 0);
  task_wire::TaskRequest result;
  result.mutable_result()->set_id("run1");
  EXPECT_EQ(call(result).backtest().SerializeAsString(),
            backtest::run(process_input()).SerializeAsString());
  task_wire::TaskRequest list;
  list.mutable_list()->set_limit(200);
  auto response = call(list);
  ASSERT_EQ(response.tasks().tasks_size(), 1);
  EXPECT_FALSE(response.tasks().tasks(0).has_input());
  service.reset();
  start();
  EXPECT_EQ(call(result).backtest().SerializeAsString(),
            backtest::run(process_input()).SerializeAsString());
  submit("run1"); // Same task ID after restart remains idempotent.
  EXPECT_EQ(call(list).tasks().tasks_size(), 1);
}
TEST_F(TaskProcess, WorkerRejectsDataEvidenceDifferentFromTheAcceptedInput) {
  auto spec = process_input();
  auto* evidence =
      spec.mutable_paper()->mutable_contracts(0)->mutable_dataset()->mutable_history_evidence(0);
  evidence->set_acquired_at_ns(evidence->acquired_at_ns() + 1);
  service.reset();
  {
    tasks::Store store(directory.path, tasks::Identity{"task", "historical-data"});
    tasks::submit(store, "different-evidence", spec);
  }
  start();
  ChildProcess worker(ASTERION_BACKTEST_PATH, {"--endpoint", endpoint, "--session", "task",
                                               "--task", "different-evidence"});
  ASSERT_TRUE(worker.wait(15s));
  EXPECT_NE(worker.exit_code(), 0);
  task_wire::TaskRequest get;
  get.mutable_get()->set_id("different-evidence");
  const auto task = call(get).task();
  EXPECT_EQ(task.state(), task_wire::FAILED);
  EXPECT_EQ(task.error(), "calculation input does not match fixed task evidence");
  EXPECT_TRUE(task.result_digest().empty());
}
TEST_F(TaskProcess, RestartFencesClaimedWorkAndQueuedCancelStaysCancelled) {
  submit("interrupted");
  task_wire::TaskRequest claim;
  claim.mutable_claim()->set_kind(task::v1::BACKTEST);
  claim.mutable_claim()->set_id("interrupted");
  const auto attempt = call(claim).attempt();
  EXPECT_FALSE(attempt.task().has_input());
  EXPECT_TRUE(attempt.has_execution());
  EXPECT_LT(attempt.ByteSizeLong(), 4096U);
  EXPECT_EQ(attempt.data_instance(), "historical-data");
  const auto token = attempt.token();
  service.reset();
  start();
  task_wire::TaskRequest get;
  get.mutable_get()->set_id("interrupted");
  EXPECT_EQ(call(get).task().state(), task_wire::INTERRUPTED);
  task_wire::TaskRequest finish;
  auto* f = finish.mutable_finish();
  f->set_id("interrupted");
  f->set_token(token);
  *f->mutable_result() = backtest::run(process_input());
  EXPECT_THROW(call(finish), std::runtime_error);
  submit("cancelled");
  task_wire::TaskRequest cancel;
  cancel.mutable_cancel()->set_id("cancelled");
  EXPECT_EQ(call(cancel).task().state(), task_wire::CANCELLED);
  ChildProcess worker(ASTERION_BACKTEST_PATH,
                      {"--endpoint", endpoint, "--session", "task", "--task", "cancelled"});
  ASSERT_TRUE(worker.wait(10s));
  EXPECT_NE(worker.exit_code(), 0);
}
TEST_F(TaskProcess, ListenerServesRepeatedConnectionsAndRetainsOwnership) {
  for (int i = 0; i < 5; ++i) {
    task_wire::TaskRequest request;
    request.mutable_heartbeat();
    EXPECT_TRUE(call(request).has_health());
  }
  EXPECT_THROW(testing_support::LocalListener duplicate(endpoint), Error);
}

TEST_F(TaskProcess, WorkerLeaseExpiryInterruptsWithoutAutomaticRetry) {
  submit("lost");
  task_wire::TaskRequest claim;
  claim.mutable_claim()->set_kind(task::v1::BACKTEST);
  claim.mutable_claim()->set_id("lost");
  const auto token = call(claim).attempt().token();
  task_wire::TaskRequest get;
  get.mutable_get()->set_id("lost");
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (call(get).task().state() == task_wire::RUNNING &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(50ms);
  EXPECT_EQ(call(get).task().state(), task_wire::INTERRUPTED);
  EXPECT_EQ(call(get).task().attempt(), 1U);
  task_wire::TaskRequest progress;
  progress.mutable_progress()->set_id("lost");
  progress.mutable_progress()->set_token(token);
  progress.mutable_progress()->set_completed(1);
  EXPECT_THROW(call(progress), std::runtime_error);
}
TEST_F(TaskProcess, WorkerHeartbeatsWithoutEngineProgressAndObservesCancellation) {
  for (const bool cancel : {false, true}) {
    const auto id = cancel ? "cancel-initializing" : "initializing";
    submit(id);
    std::promise<void> entered;
    auto ready = entered.get_future();
    auto worker = std::async(std::launch::async, [&] {
      return protocol::run_task_worker(
          endpoint + ".worker", "", 0, {}, "task", id, task::v1::BACKTEST,
          [&](const auto& attempt, std::stop_token stop, const auto&) {
            entered.set_value();
            // No engine progress for longer than this service's two-second lease.
            const auto deadline = std::chrono::steady_clock::now() + 3500ms;
            while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline)
              std::this_thread::sleep_for(10ms);
            if (stop.stop_requested())
              throw std::runtime_error("initialization cancelled");
            task::v1::TaskFinish result;
            *result.mutable_result() = backtest::run(attempt.task().input());
            return result;
          },
          0);
    });
    ASSERT_EQ(ready.wait_for(5s), std::future_status::ready);
    std::this_thread::sleep_for(2500ms);
    task_wire::TaskRequest get;
    get.mutable_get()->set_id(id);
    ASSERT_EQ(call(get).task().state(), task::v1::RUNNING);
    if (cancel) {
      task_wire::TaskRequest request;
      request.mutable_cancel()->set_id(id);
      EXPECT_EQ(call(request).task().state(), task::v1::CANCEL_REQUESTED);
    }
    ASSERT_EQ(worker.wait_for(5s), std::future_status::ready);
    EXPECT_EQ(worker.get(), cancel ? 2 : 0);
    EXPECT_EQ(call(get).task().state(), cancel ? task::v1::CANCELLED : task::v1::SUCCEEDED);
  }
}
TEST_F(TaskProcess, WorkerLivenessFailureDoesNotPretendToBeUserCancellation) {
  submit("lost-service");
  std::promise<void> entered;
  auto ready = entered.get_future();
  auto worker = std::async(std::launch::async, [&] {
    return protocol::run_task_worker(
        endpoint + ".worker", "", 0, {}, "task", "lost-service", task::v1::BACKTEST,
        [&](const auto&, std::stop_token stop, const auto&) -> task::v1::TaskFinish {
          entered.set_value();
          const auto deadline = std::chrono::steady_clock::now() + 10s;
          while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(10ms);
          if (!stop.stop_requested())
            throw std::runtime_error("worker did not notice lost liveness");
          throw std::runtime_error("runner stopped after losing service");
        },
        0);
  });
  ASSERT_EQ(ready.wait_for(5s), std::future_status::ready);
  service.reset();
  start();
  ASSERT_EQ(worker.wait_for(10s), std::future_status::ready);
  EXPECT_THROW(static_cast<void>(worker.get()), std::exception);
  task_wire::TaskRequest get;
  get.mutable_get()->set_id("lost-service");
  EXPECT_EQ(call(get).task().state(), task::v1::INTERRUPTED);
}
TEST_F(TaskProcess, MaximumFactorInputCompletesAndRestartedResultReadKeepsControlsAvailable) {
  service.reset();
  auto spec = maximum_factor_input();
  *factor_bars(spec) =
      published_dataset(std::vector<int>(protocol::max_dataset_bars, 100), "maximum");
  spec.set_dataset_revision(spec.series(0).bars().revision());
  {
    tasks::Store store(directory.path, tasks::Identity{"task", "historical-data"});
    tasks::submit(store, "maximum", spec);
    tasks::submit(store, "other", input());
  }
  lease_seconds = 30; // The production lease, independent of verification time.
  start(testing_support::bound(10s));
  // Diagnostics must stay outside the strictly owned task-store contents.
  const auto worker_log = std::filesystem::temp_directory_path() /
                          ("asterion-maximum-worker-" + unique_process_id() + ".log");
  ChildProcess worker(
      ASTERION_FACTOR_PATH,
      {"--endpoint", endpoint + ".worker", "--session", "task", "--task", "maximum"}, false,
      worker_log, true);
  const auto worker_deadline =
      std::chrono::steady_clock::now() + protocol::task_verification_timeout + 30s;
  unsigned worker_observations = 0;
  std::chrono::steady_clock::duration worker_longest{};
  while (!worker.wait(20ms) && std::chrono::steady_clock::now() < worker_deadline) {
    task_wire::TaskRequest list;
    list.mutable_list()->set_limit(200);
    const auto begin = std::chrono::steady_clock::now();
    EXPECT_EQ(call(list).tasks().tasks_size(), 2);
    worker_longest = std::max(worker_longest, std::chrono::steady_clock::now() - begin);
    ++worker_observations;
  }
  ASSERT_TRUE(worker.exited());
  EXPECT_GT(worker_observations, 0U);
  EXPECT_LT(worker_longest, testing_support::bound(1s));
  RecordProperty("worker_status_observations", worker_observations);
  RecordProperty("worker_longest_status_ms",
                 std::chrono::duration_cast<std::chrono::milliseconds>(worker_longest).count());
  if (worker.exit_code() != 0) {
    std::ifstream log(worker_log);
    const std::string diagnostic{std::istreambuf_iterator<char>(log), {}};
    task_wire::TaskRequest list;
    list.mutable_list()->set_limit(200);
    std::filesystem::remove(worker_log);
    FAIL() << diagnostic << '\n' << call(list).tasks().DebugString();
  }
  std::filesystem::remove(worker_log);
  service.reset();
  start(testing_support::bound(10s));
  // The first read after restart validates saved result evidence without running the engine.
  task_wire::TaskRequest request;
  request.mutable_result()->set_id("maximum");
  auto pending = std::async(std::launch::async, [&] {
    return call(request, "", protocol::task_verification_timeout + 5s);
  });
  unsigned observations = 0;
  std::chrono::steady_clock::duration longest{};
  while (pending.wait_for(10ms) != std::future_status::ready) {
    task_wire::TaskRequest list;
    list.mutable_list()->set_limit(200);
    const auto begin = std::chrono::steady_clock::now();
    EXPECT_EQ(call(list).tasks().tasks_size(), 2);
    longest = std::max(longest, std::chrono::steady_clock::now() - begin);
    ++observations;
  }
  const auto response = pending.get();
  EXPECT_GT(observations, 0U);
  EXPECT_LT(longest, testing_support::bound(1s));
  EXPECT_EQ(response.factor().samples_size(), static_cast<int>(protocol::max_dataset_bars) - 3);
  EXPECT_EQ(response.result_task().factor().SerializeAsString(), spec.SerializeAsString());
  RecordProperty("factor_json_bytes",
                 protocol::decode_factor_result(spec, response.factor()).dump().size());
  task_wire::TaskRequest cancel;
  cancel.mutable_cancel()->set_id("other");
  EXPECT_EQ(call(cancel).task().state(), task::v1::CANCELLED);
  RecordProperty("concurrent_status_observations", observations);
  RecordProperty("longest_status_ms",
                 std::chrono::duration_cast<std::chrono::milliseconds>(longest).count());
}

TEST(TaskStore, RejectsMismatchedResultContractAndMetrics) {
  TaskDirectory directory;
  tasks::Store store(directory.path, tasks::Identity{"task", "historical-data"});
  tasks::submit(store, "job", input());
  const auto token = store.commit(store.claim("job")).token();
  auto result = backtest::run(input());
  result.mutable_account()->mutable_contracts(0)->mutable_contract()->set_symbol("rb2611");
  EXPECT_THROW(store.commit(tasks::finish(store, "job", token, result)), std::invalid_argument);
  result = backtest::run(input());
  result.mutable_max_drawdown()->set_units(0);
  EXPECT_THROW(store.commit(tasks::finish(store, "job", token, result)), std::invalid_argument);
  EXPECT_EQ(store.get("job").state(), task::v1::RUNNING);
  EXPECT_FALSE(std::filesystem::exists(directory.path / "job" / "results" / "1.pb"));
  store.commit(tasks::finish(store, "job", token, backtest::run(input())));
}

TEST(TaskStore, RejectsResultWithDifferentRiskConfiguration) {
  TaskDirectory directory;
  tasks::Store store(directory.path, tasks::Identity{"task", "historical-data"});
  const auto spec = input();
  tasks::submit(store, "risk-result", spec);
  const auto token = store.commit(store.claim("risk-result")).token();
  auto result = backtest::run(spec);
  result.mutable_account()->mutable_risk()->set_max_working_orders(999);
  EXPECT_THROW(store.commit(tasks::finish(store, "risk-result", token, result)),
               std::invalid_argument);
  EXPECT_EQ(store.get("risk-result").state(), task::v1::RUNNING);
  store.commit(tasks::finish(store, "risk-result", token, backtest::run(spec)));
  EXPECT_EQ(tasks::result(store, "risk-result").account().risk().SerializeAsString(),
            spec.paper().risk().SerializeAsString());
}

namespace {
struct TaskClock final : Clock {
  Nanoseconds value = 1790298000000000000LL;
  Nanoseconds utc_now() const override { return value; }
  Nanoseconds monotonic_now() const override { return 0; }
};
} // namespace
TEST(TaskStore, SubmissionOrderSurvivesEqualTimesClockRollbackAndRestart) {
  TaskDirectory directory;
  auto clock = std::make_shared<TaskClock>();
  task::v1::Task first, latest;
  {
    tasks::Store store(directory.path, tasks::Identity{"task", "historical-data"}, clock);
    first = tasks::submit(store, "z-first", input());
    tasks::submit(store, "a-second", input()); // Same millisecond, opposite ID order.
    clock->value -= 60000000000LL;
    latest = tasks::submit(store, "m-latest", input());
    EXPECT_LT(latest.submitted_at_ms(), first.submitted_at_ms());
    ASSERT_EQ(store.list().tasks_size(), 3);
    EXPECT_EQ(store.list().tasks(0).id(), "z-first");
    EXPECT_EQ(store.list().tasks(1).id(), "a-second");
    EXPECT_EQ(store.list().tasks(2).id(), "m-latest");
    EXPECT_EQ(tasks::submit(store, "z-first", input()).SerializeAsString(),
              first.SerializeAsString());
    EXPECT_EQ(latest.submission_sequence(), 3U);
  }
  tasks::Store restored(directory.path, tasks::Identity{"task", "historical-data"}, clock);
  EXPECT_EQ(restored.get("z-first").SerializeAsString(), first.SerializeAsString());
  EXPECT_EQ(restored.get("m-latest").SerializeAsString(), latest.SerializeAsString());
  EXPECT_EQ(restored.list().tasks(2).id(), "m-latest");
  EXPECT_EQ(tasks::submit(restored, "b-next", input()).submission_sequence(), 4U);
  EXPECT_EQ(restored.list().tasks(3).id(), "b-next");
}
TEST(TaskStore, MetadataUpdatesPreserveLargeImmutableInputsAcrossRecovery) {
  TaskDirectory directory;
  auto spec = input();
  auto* dataset = spec.mutable_paper()->mutable_contracts(0)->mutable_dataset();
  const auto first = dataset->bars(0);
  dataset->clear_bars();
  for (std::int64_t i = 0; i < 10000; ++i) {
    auto* bar = dataset->add_bars();
    *bar = first;
    bar->set_timestamp_ns(first.timestamp_ns() + i * 1000000000);
  }
  dataset->set_revision(protocol::bar_dataset_revision(*dataset));
  spec.set_dataset_revision(protocol::dataset_revision(spec.paper()));
  const auto original = spec.SerializeAsString();
  {
    tasks::Store store(directory.path, tasks::Identity{"task", "historical-data"});
    tasks::submit(store, "large", spec);
    EXPECT_EQ(store.list().capacity().retained_tasks(), 1U);
    EXPECT_EQ(store.list().capacity().uncommitted(), 0U);
    auto expected = store.get("large");
    expected.clear_definition();
    const auto begin = std::chrono::steady_clock::now();
    for (unsigned i = 0; i < 128; ++i) {
      const auto list = store.list();
      ASSERT_EQ(list.tasks_size(), 1);
      EXPECT_EQ(list.tasks(0).SerializeAsString(), expected.SerializeAsString());
      task::v1::TaskDispatch allowance;
      allowance.set_launch_slots(1);
      EXPECT_EQ(store.dispatch(allowance).launches_size(), 1);
    }
    RecordProperty("metadata_128_polls_us",
                   std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
                                      std::chrono::steady_clock::now() - begin)
                                      .count()));
    RecordProperty("immutable_input_bytes", std::to_string(original.size()));
    auto token = store.commit(store.claim("large")).token();
    for (unsigned completed = 1; completed <= 50; ++completed)
      store.commit(store.progress("large", token, completed));
    EXPECT_EQ(store.list().tasks(0).completed(), 50U);
    task::v1::TaskDispatch allowance;
    allowance.set_launch_slots(1);
    EXPECT_EQ(store.dispatch(allowance).launches_size(), 0);
    EXPECT_EQ(store.commit(store.cancel("large")).task().state(), task::v1::CANCEL_REQUESTED);
    store.commit(store.acknowledge_cancel("large", token));
    store.commit(store.retry("large")).task();
    token = store.commit(store.claim("large")).token();
    store.commit(store.progress("large", token, 40));
    store.commit(store.fail("large", token, "explicit test failure"));
    EXPECT_EQ(store.list().tasks(0).error(), "explicit test failure");
    store.commit(store.retry("large")).task();
    token = store.commit(store.claim("large")).token();
    store.commit(store.progress("large", token, 55));
    EXPECT_EQ(store.get("large").input().SerializeAsString(), original);
  }
  tasks::Store restored(directory.path, tasks::Identity{"task", "historical-data"});
  const auto recovered = restored.get("large");
  EXPECT_EQ(recovered.state(), task::v1::INTERRUPTED);
  EXPECT_EQ(recovered.attempt(), 3U);
  EXPECT_EQ(recovered.completed(), 55U);
  EXPECT_EQ(recovered.input().SerializeAsString(), original);
  EXPECT_EQ(restored.commit(restored.retry("large")).task().definition_case(),
            task::v1::Task::DEFINITION_NOT_SET);
  EXPECT_EQ(restored.get("large").input().SerializeAsString(), original);
  const auto queued = restored.list().tasks(0);
  EXPECT_EQ(restored.list().capacity().retained_tasks(), 1U);
  EXPECT_EQ(queued.state(), task::v1::QUEUED);
  EXPECT_EQ(queued.total(), 10000U);
  EXPECT_EQ(queued.completed(), 0U);
  EXPECT_TRUE(queued.error().empty());
  EXPECT_EQ(queued.definition_case(), task::v1::Task::DEFINITION_NOT_SET);
}
TEST(TaskStore, BinaryPayloadsRemainCompactAndDefinitionsLoadOnDemand) {
  TaskDirectory directory;
  const auto spec = input();
  const auto original = spec.SerializeAsString();
  {
    tasks::Store store(directory.path, tasks::Identity{"task", "historical-data"});
    tasks::submit(store, "binary", spec);
  }
  const auto path = directory.path / "binary/input.pb";
  EXPECT_EQ(std::filesystem::file_size(path), original.size());
  EXPECT_EQ(sha256_file(path), sha256_bytes(original));
  {
    sqlite::Database db(directory.path / "tasks.sqlite", sqlite::Database::Access::read_only);
    sqlite::Database::Statement read(db, "SELECT manifest FROM tasks WHERE id='binary'");
    ASSERT_TRUE(read.step());
    const auto manifest = Json::parse(read.text(0));
    EXPECT_EQ(manifest.at("input_bytes"), original.size());
    EXPECT_EQ(manifest.at("input_sha256"), sha256_bytes(original));
    EXPECT_FALSE(manifest.contains("input"));
    EXPECT_LT(read.text(0).size(), 1024U);
  }
  tasks::Store store(directory.path, tasks::Identity{"task", "historical-data"});
  // Metadata operations never read or retain the input. Explicit input reads
  // and worker claims must still reject missing or changed owned bytes.
  std::filesystem::remove(path);
  EXPECT_EQ(store.list().tasks(0).total(), 7U);
  EXPECT_EQ(store.commit(store.cancel("binary")).task().state(), task::v1::CANCELLED);
  EXPECT_EQ(store.commit(store.retry("binary")).task().definition_case(),
            task::v1::Task::DEFINITION_NOT_SET);
  EXPECT_THROW(store.get("binary"), std::invalid_argument);
  EXPECT_THROW(store.commit(store.claim("binary")).token(), std::invalid_argument);
  EXPECT_EQ(store.describe("binary").state(), task::v1::QUEUED);
  write_file_durably(path, original);
  EXPECT_EQ(store.get("binary").input().SerializeAsString(), original);
  const auto expected = backtest::run(spec);
  store.commit(
      tasks::finish(store, "binary", store.commit(store.claim("binary")).token(), expected));
  EXPECT_EQ(std::filesystem::file_size(directory.path / "binary/results/1.pb"),
            expected.ByteSizeLong());
  EXPECT_EQ(tasks::result(store, "binary").SerializeAsString(), expected.SerializeAsString());
  auto corrupt = original;
  corrupt.back() ^= 1;
  write_file_durably(path, corrupt);
  EXPECT_THROW(store.get("binary"), std::invalid_argument);
  EXPECT_THROW(tasks::result(store, "binary"), std::invalid_argument);
}

TEST(TaskStore, InvalidPayloadOrPreviousManifestIsRejectedWithoutRewritingFiles) {
  for (const auto& mode : {"missing", "truncated", "changed", "symlink", "protobuf", "old"}) {
    SCOPED_TRACE(mode);
    TaskDirectory directory;
    {
      tasks::Store store(directory.path, tasks::Identity{"task", "historical-data"});
      tasks::submit(store, "first", input()); // Same digest exercises restored metadata reuse.
      tasks::submit(store, "preserve", input());
      (void)store.commit(store.claim("preserve")).token();
    }
    const auto path = directory.path / "preserve/input.pb";
    const std::string kind = mode;
    if (kind == "missing")
      std::filesystem::remove(path);
    else if (kind == "truncated")
      std::filesystem::resize_file(path, 1);
    else if (kind == "changed") {
      auto raw = input().SerializeAsString();
      raw.back() ^= 1;
      write_file_durably(path, raw);
    } else if (kind == "symlink") {
      std::filesystem::rename(path, directory.path / "preserve/other.pb");
      std::filesystem::create_symlink("other.pb", path);
    } else {
      sqlite::Database db(directory.path / "tasks.sqlite");
      Json manifest;
      {
        sqlite::Database::Statement read(db, "SELECT manifest FROM tasks WHERE id='preserve'");
        ASSERT_TRUE(read.step());
        manifest = Json::parse(read.text(0));
      }
      if (kind == "old")
        manifest["version"] = 4;
      else {
        // A matching digest cannot make malformed Protobuf a valid definition.
        write_file_durably(path, "\xff");
        manifest["input_bytes"] = 1;
        manifest["input_sha256"] = sha256_bytes("\xff");
      }
      sqlite::Database::Statement update(db, "UPDATE tasks SET manifest=? WHERE id='preserve'");
      update.bind(1, manifest.dump()).step();
    }
    const auto db_hash = sha256_file(directory.path / "tasks.sqlite");
    const auto input_hash =
        std::filesystem::is_regular_file(path) && !std::filesystem::is_symlink(path)
            ? sha256_file(path)
            : std::string{};
    EXPECT_THROW(tasks::Store rejected(directory.path, tasks::Identity{"task", "historical-data"}),
                 std::exception);
    EXPECT_THROW(tasks::Store::inspect_history_usage(directory.path,
                                                     tasks::Identity{"task", "historical-data"},
                                                     std::string(64, 'a')),
                 std::exception);
    EXPECT_EQ(sha256_file(directory.path / "tasks.sqlite"), db_hash);
    if (!input_hash.empty())
      EXPECT_EQ(sha256_file(path), input_hash);
    if (kind == "missing")
      EXPECT_FALSE(std::filesystem::exists(path));
    if (kind == "symlink")
      EXPECT_TRUE(std::filesystem::is_symlink(path));
  }
}

TEST(TaskStore, BacktestBeyondTwentyThousandBarsHasReadableResult) {
  TaskDirectory directory;
  auto spec = input();
  auto* dataset = spec.mutable_paper()->mutable_contracts(0)->mutable_dataset();
  const auto first = dataset->bars(0);
  dataset->clear_bars();
  for (int i = 0; i < 20001; ++i) {
    auto* bar = dataset->add_bars();
    *bar = first;
    bar->set_timestamp_ns(first.timestamp_ns() + static_cast<std::int64_t>(i) * 1000000);
  }
  dataset->set_revision(protocol::bar_dataset_revision(*dataset));
  spec.set_dataset_revision(protocol::dataset_revision(spec.paper()));
  const auto expected = backtest::run(spec);
  tasks::Store store(directory.path, tasks::Identity{"task", "historical-data"});
  tasks::submit(store, "beyond-old-limit", spec);
  store.commit(tasks::finish(store, "beyond-old-limit",
                             store.commit(store.claim("beyond-old-limit")).token(), expected));
  EXPECT_EQ(tasks::result(store, "beyond-old-limit").SerializeAsString(),
            expected.SerializeAsString());
  auto maximum = expected.account();
  maximum.set_total(static_cast<unsigned>(protocol::max_dataset_bars));
  maximum.set_cursor(maximum.total());
  EXPECT_NO_THROW(protocol::decode_snapshot(maximum));
  maximum.set_total(maximum.total() + 1);
  EXPECT_THROW(protocol::decode_snapshot(maximum), std::invalid_argument);
}

TEST_F(TaskProcess, DistinctMaximumInputsExposeRecoveryHealthBeforeAdmissions) {
  service.reset();
  constexpr int count = 8;
  auto spec = maximum_factor_input();
  std::vector<std::string> expected;
  {
    tasks::Store store(directory.path, tasks::Identity{"task", "historical-data"});
    for (int i = 0; i < count; ++i) {
      auto* dataset = factor_bars(spec);
      auto* last = dataset->mutable_bars(dataset->bars_size() - 1);
      const auto units = d("100").raw() + static_cast<std::int64_t>(i) * d("1").raw();
      for (auto* price :
           {last->mutable_open(), last->mutable_high(), last->mutable_low(), last->mutable_close()})
        price->set_units(units);
      dataset->set_revision(protocol::bar_dataset_revision(*dataset));
      spec.set_dataset_revision(dataset->revision());
      expected.push_back(dataset->revision());
      tasks::submit(store, "unique-" + std::to_string(i), spec);
    }
  }
  const auto begin = std::chrono::steady_clock::now();
  launch();
  unsigned recovering = 0;
  std::string instance;
  const auto deadline = begin + testing_support::bound(60s);
  while (std::chrono::steady_clock::now() < deadline) {
    task_wire::TaskRequest ping;
    ping.mutable_heartbeat();
    task_wire::TaskResponse response;
    try {
      response = call(ping, endpoint + ".health");
    } catch (const std::exception&) {
      ASSERT_FALSE(service->exited());
      std::this_thread::sleep_for(10ms);
      continue;
    }
    ASSERT_TRUE(response.has_health());
    const auto& health = response.health();
    if (instance.empty())
      instance = health.instance_id();
    EXPECT_EQ(health.instance_id(), instance);
    EXPECT_FALSE(health.recovery_required());
    if (health.initialized())
      break;
    ++recovering;
    task_wire::TaskRequest dispatch;
    dispatch.mutable_dispatch()->set_launch_slots(2);
    EXPECT_THROW(call(dispatch, endpoint + ".health"), std::runtime_error);
    std::this_thread::sleep_for(20ms);
  }
  ASSERT_LT(std::chrono::steady_clock::now(), deadline);
  EXPECT_GT(recovering, 0U);
  task_wire::TaskRequest list;
  list.mutable_list()->set_limit(200);
  const auto tasks = call(list).tasks();
  ASSERT_EQ(tasks.tasks_size(), count);
  for (int i = 0; i < count; ++i) {
    EXPECT_EQ(tasks.tasks(i).state(), task::v1::QUEUED);
    EXPECT_EQ(tasks.tasks(i).attempt(), 0U);
    EXPECT_EQ(tasks.tasks(i).source_name(), expected[static_cast<std::size_t>(i)]);
    EXPECT_EQ(tasks.tasks(i).definition_case(), task::v1::Task::DEFINITION_NOT_SET);
  }
  RecordProperty("distinct_maximum_input_count", count);
  RecordProperty("recovery_heartbeats", recovering);
  RecordProperty("recovery_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - begin)
                                    .count());
}
TEST(TaskStore, ManyCompletedMaximumInputsRestoreWithOnePayloadWorkingSet) {
  TaskDirectory directory;
  const auto spec = maximum_factor_input();
  const auto expected = factor::run(spec);
  {
    tasks::Store store(directory.path, tasks::Identity{"task", "historical-data"});
    tasks::submit(store, "batch-0", spec);
    store.commit(
        tasks::finish(store, "batch-0", store.commit(store.claim("batch-0")).token(), expected));
  }
  // Build a current-format fixture of independent completed task identities.
  // Hard links only make the test setup cheaper; every task owns its path and
  // the reader must still check every referenced file, digest and transition.
  constexpr int count = 24;
  const auto input_bytes = std::filesystem::file_size(directory.path / "batch-0/input.pb");
  ASSERT_GT(input_bytes * count, 256ULL * 1024 * 1024);
  {
    sqlite::Database db(directory.path / "tasks.sqlite");
    Json base;
    task::v1::StoredTaskRecord base_record;
    {
      sqlite::Database::Statement read(db, "SELECT manifest, record FROM tasks WHERE id='batch-0'");
      ASSERT_TRUE(read.step());
      base = Json::parse(read.text(0));
      ASSERT_TRUE(base_record.ParseFromString(read.blob(1)));
    }
    sqlite::Database::Transaction transaction(db);
    for (int i = 1; i < count; ++i) {
      const auto id = "batch-" + std::to_string(i);
      const auto dest = directory.path / id;
      std::filesystem::create_directory(dest);
      for (const auto& item :
           std::filesystem::recursive_directory_iterator(directory.path / "batch-0")) {
        const auto target = dest / item.path().lexically_relative(directory.path / "batch-0");
        if (item.is_directory())
          std::filesystem::create_directory(target);
        else
          std::filesystem::create_hard_link(item.path(), target);
      }
      auto manifest = base;
      manifest["id"] = id;
      manifest["submission_sequence"] = i + 1;
      auto record = base_record;
      record.mutable_task()->set_id(id);
      record.mutable_task()->set_submission_sequence(i + 1);
      sqlite::Database::Statement add(
          db, "INSERT INTO tasks SELECT ?1, ?2, kind, ?3, state, attempt, updated_at_ms, ?4 "
              "FROM tasks WHERE id='batch-0'");
      add.bind(1, id)
          .bind(2, i + 1)
          .bind(3, manifest.dump())
          .bind_blob(4, record.SerializeAsString())
          .step();
      sqlite::Database::Statement events(db, "INSERT INTO task_events SELECT ?1, sequence, body "
                                             "FROM task_events WHERE task_id='batch-0'");
      events.bind(1, id).step();
    }
    transaction.commit();
  }
  const auto start = std::chrono::steady_clock::now();
  {
    tasks::Store restored(directory.path, tasks::Identity{"task", "historical-data"});
    const auto list = restored.list();
    EXPECT_EQ(list.tasks_size(), count);
    for (const auto& task : list.tasks()) {
      EXPECT_EQ(task.state(), task::v1::SUCCEEDED);
      EXPECT_EQ(task.total(), static_cast<unsigned>(protocol::max_dataset_bars));
      EXPECT_EQ(task.definition_case(), task::v1::Task::DEFINITION_NOT_SET);
    }
    EXPECT_EQ(restored.get("batch-23").factor().SerializeAsString(), spec.SerializeAsString());
    EXPECT_EQ(tasks::factor_result(restored, "batch-23").SerializeAsString(),
              expected.SerializeAsString());
    RecordProperty("completed_payload_bytes", std::to_string(input_bytes * count));
    RecordProperty("single_payload_bytes", std::to_string(input_bytes));
    RecordProperty("completed_task_count", std::to_string(count));
    RecordProperty("restore_and_verify_ms",
                   std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - start)
                                      .count()));
  }
  // Bounded stopped-service reference inspection still fails closed, but its
  // total-I/O budget must not prevent a valid writer from reopening its store.
  EXPECT_THROW(tasks::Store::inspect_history_usage(directory.path,
                                                   tasks::Identity{"task", "historical-data"},
                                                   std::string(64, 'a')),
               std::invalid_argument);
}

TEST(TaskStore, DurableUpdatesDoNotChangeSubmissionIdentityOrReorderRetries) {
  TaskDirectory directory;
  auto clock = std::make_shared<TaskClock>();
  task::v1::Task retried;
  {
    tasks::Store store(directory.path, tasks::Identity{"task", "historical-data"}, clock);
    const auto original = tasks::submit(store, "first", input());
    tasks::submit(store, "second", input());
    clock->value += 1000000000;
    const auto token = store.commit(store.claim("first")).token();
    EXPECT_EQ(store.get("first").updated_at_ms(), clock->value / 1000000);
    clock->value += 1000000000;
    store.commit(store.progress("first", token, 1));
    const auto progress = store.get("first");
    clock->value += 1000000000;
    store.commit(store.progress("first", token, 1)); // Repeated progress is not a new event.
    EXPECT_EQ(store.get("first").updated_at_ms(), progress.updated_at_ms());
    store.commit(store.fail("first", token, "test worker failure"));
    clock->value += 1000000000;
    retried = store.commit(store.retry("first")).task();
    EXPECT_EQ(retried.updated_at_ms(), clock->value / 1000000);
    EXPECT_EQ(retried.SerializeAsString(), store.describe("first").SerializeAsString());
    EXPECT_EQ(retried.submitted_at_ms(), original.submitted_at_ms());
    EXPECT_EQ(retried.submission_sequence(), original.submission_sequence());
    EXPECT_EQ(store.list().tasks(0).id(), "first");
    auto json = protocol::decode_task(retried);
    EXPECT_EQ(json.at("submitted_at_ms"), original.submitted_at_ms());
    EXPECT_EQ(json.at("updated_at_ms"), retried.updated_at_ms());
    auto missing = retried;
    missing.clear_submitted_at_ms();
    EXPECT_THROW(protocol::decode_task(missing), std::invalid_argument);
  }
  tasks::Store restored(directory.path, tasks::Identity{"task", "historical-data"}, clock);
  EXPECT_EQ(restored.describe("first").SerializeAsString(), retried.SerializeAsString());
}
TEST(TaskStore, RejectsMissingOrDuplicateChronologyWithoutRewritingEvidence) {
  for (const auto& mode : {"old", "missing", "duplicate", "gap", "negative", "overflow"}) {
    SCOPED_TRACE(mode);
    TaskDirectory directory;
    {
      tasks::Store store(directory.path, tasks::Identity{"task", "historical-data"});
      tasks::submit(store, "first", input());
      tasks::submit(store, "second", input());
    }
    const auto manifest = [&] {
      sqlite::Database database(directory.path / "tasks.sqlite");
      sqlite::Database::Statement read(database, "SELECT manifest FROM tasks WHERE id='second'");
      EXPECT_TRUE(read.step());
      return Json::parse(read.text(0));
    };
    auto record = manifest();
    const std::string kind = mode;
    if (kind == "old")
      record["version"] = 4;
    else if (kind == "missing")
      record.erase("submitted_at_ms");
    else if (kind == "duplicate")
      record["submission_sequence"] = 1;
    else if (kind == "gap")
      record["submission_sequence"] = 3;
    else if (kind == "negative")
      record["submitted_at_ms"] = -1;
    else
      record["submitted_at_ms"] = std::numeric_limits<std::uint64_t>::max();
    {
      sqlite::Database database(directory.path / "tasks.sqlite");
      sqlite::Database::Statement write(database, "UPDATE tasks SET manifest=? WHERE id='second'");
      write.bind(1, record.dump()).step();
    }
    EXPECT_THROW(tasks::Store rejected(directory.path, tasks::Identity{"task", "historical-data"}),
                 std::exception);
    EXPECT_EQ(manifest(), record);
  }
}

TEST_F(TaskProcess, SilentExternalPeerDoesNotBlockQueriesAndCancellation) {
  submit("cancel-me");
  auto silent = ipc::Channel::connect(endpoint, 2s);
  task_wire::TaskRequest cancel;
  cancel.mutable_cancel()->set_id("cancel-me");
  EXPECT_EQ(call(cancel).task().state(), task_wire::CANCELLED);
}
TEST_F(TaskProcess, SilentWorkerPeerDoesNotBlockWorkerControl) {
  submit("claim-me");
  auto silent = ipc::Channel::connect(endpoint + ".worker", 2s);
  task_wire::TaskRequest claim;
  claim.mutable_claim()->set_id("claim-me");
  claim.mutable_claim()->set_kind(task_wire::BACKTEST);
  EXPECT_EQ(call(claim, endpoint + ".worker").attempt().task().state(), task_wire::RUNNING);
}
TEST_F(TaskProcess, ExternalSaturationPreservesWorkerControlAndLeaseExpiry) {
  submit("expired");
  task_wire::TaskRequest claim;
  claim.mutable_claim()->set_id("expired");
  claim.mutable_claim()->set_kind(task_wire::BACKTEST);
  const auto token = call(claim, endpoint + ".worker").attempt().token();
  std::vector<ipc::Channel> silent;
  for (unsigned i = 0; i < 20; ++i)
    silent.push_back(ipc::Channel::connect(endpoint, 2s));
  task_wire::TaskRequest get;
  get.mutable_get()->set_id("expired");
  const auto deadline = std::chrono::steady_clock::now() + 4s;
  while (call(get, endpoint + ".worker").task().state() == task_wire::RUNNING &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(30ms);
  EXPECT_EQ(call(get, endpoint + ".worker").task().state(), task_wire::INTERRUPTED);
  task_wire::TaskRequest progress;
  progress.mutable_progress()->set_id("expired");
  progress.mutable_progress()->set_token(token);
  progress.mutable_progress()->set_completed(1);
  EXPECT_THROW(call(progress, endpoint + ".worker"), std::runtime_error);
}

TEST(TaskStore, CompletedResultCarriesPersistedExperimentAndRejectsMismatchedEvidence) {
  TaskDirectory directory;
  {
    tasks::Store store(directory.path, tasks::Identity{"task", "historical-data"});
    tasks::submit(store, "evidence", input());
    store.commit(tasks::finish(store, "evidence", store.commit(store.claim("evidence")).token(),
                               backtest::run(input())));
  }
  tasks::Store restored(directory.path, tasks::Identity{"task", "historical-data"});
  task::v1::TaskResponse response;
  *response.mutable_backtest() = tasks::result(restored, "evidence");
  *response.mutable_result_task() = restored.get("evidence");
  const auto value = protocol::decode_task_result(response, "evidence");
  EXPECT_EQ(value.at("experiment").at("strategies").at(0),
            protocol::decode_strategy(testing_support::moving_average(1, 3)));
  EXPECT_EQ(value.at("experiment").at("paper").at("deposit"), "10000");
  EXPECT_EQ(value.at("experiment")
                .at("paper")
                .at("contracts")
                .at(0)
                .at("cost_schedule")
                .at(0)
                .at("values")
                .at("close_today_fee"),
            "3");
  EXPECT_EQ(value.at("experiment").at("paper").at("risk").at("max_order_quantity"), "100");
  EXPECT_FALSE(
      value.at("experiment").at("paper").at("contracts").at(0).at("dataset").contains("bars"));
  EXPECT_EQ(value.at("experiment").at("data").at(0).at("count"), 7);
  EXPECT_EQ(value.at("task").at("result_digest"), restored.get("evidence").result_digest());
  EXPECT_THROW(protocol::decode_task_result(response, "other"), std::invalid_argument);
  auto bad = response;
  bad.clear_result_task();
  EXPECT_THROW(protocol::decode_task_result(bad, "evidence"), std::invalid_argument);
  bad = response;
  bad.mutable_result_task()->set_state(task::v1::RUNNING);
  EXPECT_THROW(protocol::decode_task_result(bad, "evidence"), std::invalid_argument);
  bad = response;
  bad.mutable_backtest()->set_dataset_revision(std::string(64, '0'));
  EXPECT_THROW(protocol::decode_task_result(bad, "evidence"), std::invalid_argument);
  bad = response;
  bad.mutable_result_task()->clear_definition();
  EXPECT_THROW(protocol::decode_task_result(bad, "evidence"), std::invalid_argument);
  bad = response;
  bad.mutable_result_task()->set_kind(task::v1::FACTOR);
  EXPECT_THROW(protocol::decode_task_result(bad, "evidence"), std::invalid_argument);
}

TEST(Backtest, ExplicitTradingDaysDoNotInferFromWallClock) {
  auto spec = input();
  auto* data = spec.mutable_paper()->mutable_contracts(0)->mutable_dataset();
  data->mutable_bars(0)->set_timestamp_ns(1790254800000000000LL);
  data->set_revision(protocol::bar_dataset_revision(*data));
  spec.set_dataset_revision(data->revision());
  EXPECT_NO_THROW(backtest::run(spec));
  data->mutable_bars(0)->set_trading_day("2026-09-24");
  EXPECT_THROW(backtest::run(spec), std::invalid_argument);
}

namespace {
backtest::v1::BacktestInput multiday_input() {
  auto spec = input();
  spec.mutable_paper()->mutable_contracts(0)->mutable_dataset()->clear_bars();
  const std::int64_t first = 1790298000000000000LL;
  const std::int64_t next = first + 3LL * 86400 * 1000000000;
  int index = 0;
  for (const auto price : {100, 101, 102, 101, 104, 103, 102, 103}) {
    auto* tick = spec.mutable_paper()->mutable_contracts(0)->mutable_dataset()->add_bars();
    tick->set_trading_day(index < 4 ? "2026-09-25" : "2026-09-28");
    tick->set_timestamp_ns((index < 4 ? first : next) + (index % 4) * 1000000000LL);
    for (auto* p :
         {tick->mutable_open(), tick->mutable_high(), tick->mutable_low(), tick->mutable_close()})
      p->set_units(d(std::to_string(price).c_str()).raw());
    tick->mutable_volume()->set_units(d("10").raw());
    ++index;
  }
  spec.mutable_paper()
      ->mutable_contracts(0)
      ->mutable_dataset()
      ->mutable_days(0)
      ->mutable_settlement_price()
      ->set_units(d("105").raw());
  auto* day = spec.mutable_paper()->mutable_contracts(0)->mutable_dataset()->add_days();
  *day = spec.paper().contracts(0).dataset().days(0);
  day->set_trading_day("2026-09-28");
  day->mutable_settlement_price()->set_units(d("110").raw());
  spec.mutable_paper()->mutable_contracts(0)->mutable_dataset()->set_revision(
      protocol::bar_dataset_revision(spec.paper().contracts(0).dataset()));
  spec.set_dataset_revision(spec.paper().contracts(0).dataset().revision());
  return spec;
}
} // namespace
TEST(Backtest, MultidaySettlementCarriesSmaAndChargesYesterdayClose) {
  const auto spec = multiday_input();
  const auto result = backtest::run(spec);
  ASSERT_EQ(result.settlements_size(), 2);
  EXPECT_EQ(result.settlements(0).contracts(0).price().units(), d("105").raw());
  EXPECT_EQ(result.settlements(0).equity().units(), d("10038").raw());
  EXPECT_EQ(result.settlements(0).contracts(0).position_quantity().units(), d("1").raw());
  EXPECT_EQ(result.settlements(1).contracts(0).position_quantity().units(), 0);
  // The first day's closing signal now closes at 104 on the next day,
  // followed by an intraday round trip at 103 (2 + 4 + 2 + 3 fees).
  EXPECT_EQ(result.account().balance().units(), d("10019").raw());
  EXPECT_EQ(result.account().fees().units(), d("11").raw());
  EXPECT_EQ(result.account().realized().units(), d("30").raw());
  EXPECT_EQ(result.max_drawdown().units(), d("26").raw());
  ASSERT_EQ(result.account().orders_size(), 4);
  EXPECT_EQ(result.account().orders(1).offset(), protocol::v1::CLOSE_YESTERDAY);
  ASSERT_EQ(result.equity_size(), 10);
  EXPECT_EQ(result.equity(4).event(), backtest::v1::DAILY_SETTLEMENT);
  EXPECT_EQ(result.equity(4).timestamp_ns(),
            spec.paper().contracts(0).dataset().bars(3).timestamp_ns());
  EXPECT_EQ(result.equity(5).event(), backtest::v1::TRADE_MARK);
  EXPECT_EQ(result.equity(9).equity().units(), result.account().equity().units());
  EXPECT_EQ(result.SerializeAsString(), backtest::run(spec).SerializeAsString());
}
TEST(Backtest, FinalSettlementRevaluesOpenPositionAndParticipatesInDrawdown) {
  auto spec = multiday_input();
  spec.mutable_paper()->mutable_contracts(0)->mutable_dataset()->mutable_days()->DeleteSubrange(1,
                                                                                                1);
  spec.mutable_paper()->mutable_contracts(0)->mutable_dataset()->mutable_bars()->DeleteSubrange(4,
                                                                                                4);
  spec.mutable_paper()
      ->mutable_contracts(0)
      ->mutable_dataset()
      ->mutable_days(0)
      ->mutable_settlement_price()
      ->set_units(d("90").raw());
  spec.mutable_paper()->mutable_contracts(0)->mutable_dataset()->set_revision(
      protocol::bar_dataset_revision(spec.paper().contracts(0).dataset()));
  spec.set_dataset_revision(spec.paper().contracts(0).dataset().revision());
  const auto result = backtest::run(spec);
  EXPECT_EQ(result.account().equity().units(), d("9888").raw());
  EXPECT_EQ(result.account().realized().units(), d("-110").raw());
  EXPECT_EQ(result.account().unrealized().units(), 0);
  EXPECT_EQ(result.max_drawdown().units(), d("112").raw());
  ASSERT_EQ(result.account().positions_size(), 1);
  EXPECT_EQ(result.account().positions(0).basis().units(), d("90").raw());
  EXPECT_EQ(result.equity(3).equity().units(), d("9998").raw());
  EXPECT_EQ(result.equity(4).event(), backtest::v1::DAILY_SETTLEMENT);
}
TEST(Backtest, MultidayRequiresExplicitValidCompleteEvidence) {
  const auto original = multiday_input();
  auto spec = original;
  spec.mutable_paper()
      ->mutable_contracts(0)
      ->mutable_dataset()
      ->mutable_days(1)
      ->clear_settlement_price();
  EXPECT_THROW(backtest::run(spec), std::invalid_argument);
  spec = original;
  spec.mutable_paper()
      ->mutable_contracts(0)
      ->mutable_dataset()
      ->mutable_days(1)
      ->mutable_settlement_price()
      ->set_units(d("-1").raw());
  EXPECT_THROW(backtest::run(spec), std::invalid_argument);

  spec = original;
  spec.mutable_paper()
      ->mutable_contracts(0)
      ->mutable_dataset()
      ->mutable_days(1)
      ->mutable_settlement_price()
      ->set_units(d("110.5").raw());
  EXPECT_THROW(backtest::run(spec), std::invalid_argument);
  spec = original;
  spec.mutable_paper()->mutable_contracts(0)->mutable_dataset()->mutable_days(1)->set_trading_day(
      spec.paper().contracts(0).dataset().days(0).trading_day());
  EXPECT_THROW(backtest::run(spec), std::invalid_argument);

  spec = original;
  spec.mutable_paper()->mutable_contracts(0)->mutable_dataset()->mutable_bars()->DeleteSubrange(4,
                                                                                                4);
  spec.mutable_paper()->mutable_contracts(0)->mutable_dataset()->set_revision(
      protocol::bar_dataset_revision(spec.paper().contracts(0).dataset()));
  spec.set_dataset_revision(spec.paper().contracts(0).dataset().revision());
  EXPECT_THROW(backtest::run(spec), std::invalid_argument);
  spec = original;
  spec.set_version(3);
  EXPECT_THROW(backtest::run(spec), std::invalid_argument);
}
TEST(TaskStore, MultidayEvidenceRestoresAndForgedSettlementCannotCommit) {
  TaskDirectory root;
  const auto spec = multiday_input();
  const auto expected = backtest::run(spec);
  auto altered = spec;
  altered.mutable_paper()
      ->mutable_contracts(0)
      ->mutable_dataset()
      ->mutable_days(0)
      ->mutable_settlement_price()
      ->set_units(d("107").raw());
  altered.mutable_paper()->mutable_contracts(0)->mutable_dataset()->set_revision(
      protocol::bar_dataset_revision(altered.paper().contracts(0).dataset()));
  altered.set_dataset_revision(altered.paper().contracts(0).dataset().revision());
  const auto wrong = backtest::run(altered);
  EXPECT_EQ(wrong.account().equity().units(), expected.account().equity().units());
  {
    tasks::Store store(root.path, tasks::Identity{"task", "historical-data"});
    tasks::submit(store, "multiday", spec);
    const auto token = store.commit(store.claim("multiday")).token();
    EXPECT_THROW(store.commit(tasks::finish(store, "multiday", token, wrong)),
                 std::invalid_argument);
    auto forged = expected;
    forged.mutable_settlements(0)->mutable_contracts(0)->mutable_price()->set_units(d("106").raw());
    EXPECT_THROW(store.commit(tasks::finish(store, "multiday", token, forged)),
                 std::invalid_argument);
    forged = expected;
    forged.mutable_equity(0)->set_timestamp_ns(expected.equity(0).timestamp_ns() + 1);
    EXPECT_THROW(store.commit(tasks::finish(store, "multiday", token, forged)),
                 std::invalid_argument);
    store.commit(tasks::finish(store, "multiday", token, expected));
  }
  tasks::Store restored(root.path, tasks::Identity{"task", "historical-data"});
  EXPECT_EQ(tasks::result(restored, "multiday").SerializeAsString(), expected.SerializeAsString());
  EXPECT_EQ(restored.get("multiday").input().SerializeAsString(), spec.SerializeAsString());
}

TEST_F(TaskProcess, UpgradeFreezesAdmissionsWaitsForCompletionAndPreservesQueuedWork) {
  submit("active");
  submit("queued");
  task_wire::TaskRequest claim;
  claim.mutable_claim()->set_id("active");
  claim.mutable_claim()->set_kind(task_wire::BACKTEST);
  const auto attempt = call(claim, endpoint + ".worker").attempt();
  task_wire::TaskRequest freeze;
  freeze.mutable_quiesce();
  EXPECT_THROW(call(freeze), std::runtime_error);
  EXPECT_TRUE(call(freeze, endpoint + ".health").has_health());
  task_wire::TaskRequest blocked;
  blocked.mutable_submit()->set_id("blocked");
  EXPECT_THROW(call(blocked), std::runtime_error);
  task_wire::TaskRequest stop;
  stop.mutable_quiesce()->set_stop(true);
  EXPECT_THROW(call(stop, endpoint + ".health"), std::runtime_error);
  ASSERT_FALSE(service->exited());
  task_wire::TaskRequest finish;
  finish.mutable_finish()->set_id("active");
  finish.mutable_finish()->set_token(attempt.token());
  *finish.mutable_finish()->mutable_result() = backtest::run(process_input());
  call(finish, endpoint + ".worker");
  EXPECT_TRUE(call(stop, endpoint + ".health").has_health());
  ASSERT_TRUE(service->wait(15s));
  EXPECT_EQ(service->exit_code(), 0);
  start();
  task_wire::TaskRequest get;
  get.mutable_get()->set_id("active");
  EXPECT_EQ(call(get).task().state(), task_wire::SUCCEEDED);
  get.mutable_get()->set_id("queued");
  EXPECT_EQ(call(get).task().state(), task_wire::QUEUED);
}

TEST(TaskStore, PinnedRiskArtifactIsRetainedButResultReadsDoNotLoadAlgorithm) {
  TaskDirectory directory;
  std::string artifact;
  const auto expected = backtest::run(input());
  const auto file = risk_providers::Module::filename(directory.path / "done");
  {
    tasks::Store store(directory.path, tasks::Identity{"task", "historical-data"});
    artifact = tasks::submit(store, "done", input()).risk_artifact();
    EXPECT_EQ(artifact, sha256_file(file));
    store.commit(tasks::finish(store, "done", store.commit(store.claim("done")).token(), expected));
    EXPECT_EQ(tasks::result(store, "done").SerializeAsString(), expected.SerializeAsString());
    const auto saved = directory.path / "original";
    std::filesystem::rename(file, saved);
    EXPECT_EQ(tasks::result(store, "done").SerializeAsString(), expected.SerializeAsString());
    EXPECT_THROW(risk_providers::Module::pinned(directory.path / "done", artifact), std::exception);
    std::ofstream(file) << "corrupt fixture";
    EXPECT_EQ(tasks::result(store, "done").SerializeAsString(), expected.SerializeAsString());
    EXPECT_THROW(risk_providers::Module::pinned(directory.path / "done", artifact), std::exception);
    EXPECT_EQ(std::filesystem::file_size(file), 15U);
    std::filesystem::remove(file);
    std::filesystem::rename(saved, file);
  }
  tasks::Store recovered(directory.path, tasks::Identity{"task", "historical-data"});
  EXPECT_EQ(recovered.get("done").risk_artifact(), artifact);
  EXPECT_EQ(tasks::result(recovered, "done").SerializeAsString(), expected.SerializeAsString());
}

#include "bar_dataset_source.hpp"
#include "data/history_fixture.hpp"
TEST(NamedDatasets, SettlementDaysWithoutMinuteBarsAreListedAsUncovered) {
  TaskDirectory directory;
  const auto selection =
      test::seed_history(directory.path, {100, 101, 102, 103}, "gap", {"2026-09-23", "2026-09-25"},
                         {"2026-09-23", "2026-09-24", "2026-09-25"});
  data::Store store(directory.path, "historical-data", "task");
  Json request = selection;
  request.erase("price_increment");
  request.erase("multiplier");
  request["contract"] = {{"venue", "SHFE"},           {"symbol", "rb2610"},
                         {"product", "rb"},           {"delivery_month", "2026-10"},
                         {"currency", "CNY"},         {"price_increment", "1"},
                         {"quantity_increment", "1"}, {"multiplier", "10"}};
  const auto sources = store.sources(protocol::encode_bar_dataset_request(request));
  const auto dataset = data::resolve_bar_dataset(sources);
  ASSERT_EQ(dataset.bars_size(), 4);
  EXPECT_EQ(dataset.days_size(), 2);
  ASSERT_EQ(dataset.uncovered_days_size(), 1);
  EXPECT_EQ(dataset.uncovered_days(0), "2026-09-24");
  EXPECT_EQ(dataset.revision(), protocol::bar_dataset_revision(dataset));
}
#include "history_coverage.hpp"
TEST(NamedDatasets, CoverageCountsTradingDaysPerContractAcrossTheArchive) {
  TaskDirectory directory;
  test::seed_history(directory.path, {100, 101, 102, 103}, "coverage", {"2026-09-23", "2026-09-25"},
                     {"2026-09-23", "2026-09-24", "2026-09-25"});
  history_files::Archive archive(directory.path / "history");
  data::v1::HistoryFilter filter;
  filter.set_venue("SHFE");
  filter.set_product("rb");
  const auto coverage = data::history_coverage(archive, filter);
  ASSERT_EQ(coverage.items_size(), 1);
  const auto& row = coverage.items(0);
  EXPECT_EQ(row.contract_id(), "SHFE/rb/2026-10");
  EXPECT_EQ(row.minute_days(), 2U);
  EXPECT_EQ(row.minute_first(), "2026-09-23");
  EXPECT_EQ(row.daily_days(), 3U);
  EXPECT_EQ(row.uncovered(), 1U);
  ASSERT_EQ(row.uncovered_days_size(), 1);
  EXPECT_EQ(row.uncovered_days(0), "2026-09-24");
  filter.set_product("cu");
  EXPECT_EQ(data::history_coverage(archive, filter).items_size(), 0);
}
TEST(Backtest, PortfolioOfIdenticalContractsDoublesEveryLedgerEffect) {
  const auto single = multiday_input();
  auto pair = single;
  auto* second = pair.mutable_paper()->add_contracts();
  *second = single.paper().contracts(0);
  auto* data = second->mutable_dataset();
  *data->mutable_contract() = test::contract("SHFE", "hc2610", "hc", "2026-10");
  data->set_revision(protocol::bar_dataset_revision(*data));
  pair.set_dataset_revision(protocol::dataset_revision(pair.paper()));
  EXPECT_NE(pair.dataset_revision(), single.dataset_revision());
  const auto one = backtest::run(single);
  const auto two = backtest::run(pair);
  EXPECT_EQ(two.SerializeAsString(), backtest::run(pair).SerializeAsString());
  const auto doubled = [](const protocol::v1::Decimal& value) { return value.units() * 2; };
  EXPECT_EQ(two.account().fees().units(), doubled(one.account().fees()));
  EXPECT_EQ(two.account().realized().units(), doubled(one.account().realized()));
  EXPECT_EQ(two.account().fills_size(), one.account().fills_size() * 2);
  ASSERT_EQ(two.settlements_size(), one.settlements_size());
  for (int day = 0; day < two.settlements_size(); ++day) {
    const auto& settled = two.settlements(day);
    ASSERT_EQ(settled.contracts_size(), 2);
    EXPECT_EQ(settled.contracts(0).symbol(), "rb2610");
    EXPECT_EQ(settled.contracts(1).symbol(), "hc2610");
    EXPECT_EQ(settled.contracts(1).position_quantity().units(),
              one.settlements(day).contracts(0).position_quantity().units());
    // Equity moves twice as far from the shared deposit.
    EXPECT_EQ(settled.equity().units() - d("10000").raw(),
              (one.settlements(day).equity().units() - d("10000").raw()) * 2);
  }
  auto shifted = pair;
  shifted.mutable_paper()->mutable_contracts(1)->mutable_dataset()->mutable_days()->DeleteSubrange(
      1, 1);
  shifted.mutable_paper()->mutable_contracts(1)->mutable_dataset()->mutable_bars()->DeleteSubrange(
      4, 4);
  auto* shifted_data = shifted.mutable_paper()->mutable_contracts(1)->mutable_dataset();
  shifted_data->set_revision(protocol::bar_dataset_revision(*shifted_data));
  shifted.set_dataset_revision(protocol::dataset_revision(shifted.paper()));
  EXPECT_THROW(backtest::run(shifted), std::invalid_argument);
}

TEST(Backtest, FullBarBudgetRunsThroughFinalSettlement) {
  auto spec = input();
  std::vector<MarketBar> bars;
  for (std::size_t i = 0; i < protocol::max_dataset_bars; ++i)
    bars.push_back(test::flat("2026-09-25", 1790298000000000000LL + i * 1000000LL, "100", "10"));
  *spec.mutable_paper()->mutable_contracts(0)->mutable_dataset() =
      test::dataset(bars, {{"2026-09-25", d("100")}});
  spec.set_dataset_revision(protocol::dataset_revision(spec.paper()));
  const auto result = backtest::run(spec);
  EXPECT_EQ(result.equity_size(), protocol::max_dataset_bars + 1);
  ASSERT_EQ(result.settlements_size(), 1);
  EXPECT_EQ(result.settlements(0).balance().units(), d("10000").raw());
  EXPECT_EQ(result.account().cursor(), protocol::max_dataset_bars);
}

TEST(TaskStore, DatedCostsArePinnedSwitchAtSettlementAndRejectForgedResults) {
  TaskDirectory directory;
  auto spec = multiday_input();
  auto* schedule = spec.mutable_paper()->mutable_contracts(0)->mutable_cost_schedule();
  auto next = schedule->versions(0);
  next.set_effective_from("2026-09-28");
  next.set_source("new published schedule");
  next.mutable_values()->mutable_close_yesterday_fee()->set_units(d("9").raw());
  next.mutable_values()->mutable_margin_per_lot()->set_units(d("700").raw());
  *schedule->add_versions() = next;
  const auto expected = backtest::run(spec);
  EXPECT_EQ(expected.settlements(0).fees().units(), d("2").raw());
  EXPECT_EQ(expected.account().fees().units(), d("16").raw());
  EXPECT_EQ(expected.account().balance().units(), d("10014").raw());
  EXPECT_EQ(expected.account().contracts(0).costs().margin_per_lot().units(), d("700").raw());
  EXPECT_EQ(expected.account().contracts(0).cost_schedule().SerializeAsString(),
            schedule->SerializeAsString());
  {
    tasks::Store store(directory.path, tasks::Identity{"task", "historical-data"});
    tasks::submit(store, "dated", spec);
    const auto token = store.commit(store.claim("dated")).token();
    auto forged = expected;
    forged.mutable_account()
        ->mutable_contracts(0)
        ->mutable_cost_schedule()
        ->mutable_versions(1)
        ->set_source("unrecorded replacement");
    EXPECT_THROW(store.commit(tasks::finish(store, "dated", token, forged)), std::invalid_argument);
    forged = expected;
    forged.mutable_account()
        ->mutable_contracts(0)
        ->mutable_costs()
        ->mutable_margin_per_lot()
        ->set_units(d("100").raw());
    EXPECT_THROW(store.commit(tasks::finish(store, "dated", token, forged)), std::invalid_argument);
    store.commit(tasks::finish(store, "dated", token, expected));
  }
  tasks::Store restored(directory.path, tasks::Identity{"task", "historical-data"});
  EXPECT_EQ(tasks::result(restored, "dated").SerializeAsString(), expected.SerializeAsString());
  EXPECT_EQ(restored.get("dated").input().SerializeAsString(), spec.SerializeAsString());
}

TEST(NamedDatasets, CoverageDoesNotMergeIntervalsOrRevisions) {
  TaskDirectory directory;
  const auto sparse =
      test::seed_history(directory.path, {100, 101, 102, 103}, "sparse",
                         {"2026-09-23", "2026-09-25"}, {"2026-09-23", "2026-09-24", "2026-09-25"});
  const auto filled = test::seed_history(directory.path, {100, 101, 102, 103, 104, 105}, "filled",
                                         {"2026-09-23", "2026-09-24", "2026-09-25"}, {}, "rb", 5);
  history_files::Archive archive(directory.path / "history");
  const auto result = data::history_coverage(archive, {});
  // Identical settlement bars from two acquisitions retain distinct version evidence.
  ASSERT_EQ(result.items_size(), 4);
  const std::set<std::string> daily_versions{
      sparse.at("settlement_dataset_ids").at(0).get<std::string>(),
      filled.at("settlement_dataset_ids").at(0).get<std::string>()};
  for (const auto& row : result.items()) {
    EXPECT_TRUE(daily_versions.contains(row.daily_dataset_id()));
    if (row.minute_dataset_id() == sparse.at("source_dataset_ids").at(0).get<std::string>()) {
      EXPECT_EQ(row.interval_minutes(), 1);
      EXPECT_EQ(row.uncovered(), 1);
      EXPECT_EQ(row.uncovered_days(0), "2026-09-24");
    } else {
      EXPECT_EQ(row.minute_dataset_id(), filled.at("source_dataset_ids").at(0).get<std::string>());
      EXPECT_EQ(row.interval_minutes(), 5);
      EXPECT_EQ(row.uncovered(), 0);
    }
  }
}

TEST(NamedDatasets, CoverageRejectsChangedManifestEvenWhenChunksAreValid) {
  TaskDirectory directory;
  const auto selected = test::seed_history(directory.path, {100, 101, 102}, "fixed-coverage");
  history_files::Archive archive(directory.path / "history");
  const auto record = archive.get(selected.at("source_dataset_ids").at(0).get<std::string>());
  const auto manifest_path =
      std::filesystem::path(record.minute_result().directory()) / "minutes.json";
  std::ifstream stream(manifest_path);
  const std::string original((std::istreambuf_iterator<char>(stream)), {});
  stream.close();
  replace_file_durably(manifest_path, original + "\n");
  EXPECT_THROW(data::history_coverage(archive, {}), std::invalid_argument);
}

TEST(NamedDatasets, RequestedWindowExcludesUnrelatedMissingSettlement) {
  TaskDirectory directory;
  const auto selection = test::seed_history(directory.path, {100, 101, 102, 103}, "window",
                                            {"2026-09-23", "2026-09-25"}, {"2026-09-25"});
  data::Store store(directory.path, "historical-data", "task");
  data::v1::BarDatasetRequest request;
  request.add_source_dataset_ids(selection.at("source_dataset_ids").at(0).get<std::string>());
  request.add_settlement_dataset_ids(
      selection.at("settlement_dataset_ids").at(0).get<std::string>());
  *request.mutable_contract() = test::contract();
  EXPECT_THROW(data::resolve_bar_dataset(store.sources(request)), std::invalid_argument);
  request.set_begin_day("2026-09-25");
  request.set_end_day("2026-09-25");
  const auto dataset = data::resolve_bar_dataset(store.sources(request));
  ASSERT_EQ(dataset.days_size(), 1);
  EXPECT_EQ(dataset.days(0).trading_day(), "2026-09-25");
  EXPECT_EQ(dataset.bars_size(), 2);
}

TEST(NamedDatasets, RequestedWindowReportsMissingBoundaryDays) {
  TaskDirectory directory;
  const auto selection =
      test::seed_history(directory.path, {100, 101, 102, 103}, "boundary", {"2026-09-24"},
                         {"2026-09-23", "2026-09-24", "2026-09-25"});
  data::Store store(directory.path, "historical-data", "task");
  data::v1::BarDatasetRequest request;
  request.add_source_dataset_ids(selection.at("source_dataset_ids").at(0).get<std::string>());
  request.add_settlement_dataset_ids(
      selection.at("settlement_dataset_ids").at(0).get<std::string>());
  request.set_begin_day("2026-09-23");
  request.set_end_day("2026-09-25");
  *request.mutable_contract() = test::contract();
  const auto dataset = data::resolve_bar_dataset(store.sources(request));
  ASSERT_EQ(dataset.uncovered_days_size(), 2);
  EXPECT_EQ(dataset.uncovered_days(0), "2026-09-23");
  EXPECT_EQ(dataset.uncovered_days(1), "2026-09-25");
  task::v1::Task calculation;
  *factor_bars(*calculation.mutable_factor()) = dataset;
  calculation.mutable_factor()->set_version(6);
  calculation.mutable_factor()->set_dataset_revision(dataset.revision());
  calculation.mutable_factor()->add_lookbacks(1);
  calculation.mutable_factor()->set_horizon(1);
  calculation.mutable_factor()->set_full_sample(true);
  const auto execution =
      protocol::task_execution(calculation, sha256_bytes(calculation.factor().SerializeAsString()));
  EXPECT_EQ(execution.factor().series(0).bars().begin_day(), "2026-09-23");
  EXPECT_EQ(execution.factor().series(0).bars().end_day(), "2026-09-25");
  EXPECT_EQ(data::resolve_bar_dataset(store.sources(execution.factor().series(0).bars()))
                .SerializeAsString(),
            dataset.SerializeAsString());
}

namespace {
data::v1::BarDatasetRequest composed_request(const std::vector<Json>& selections) {
  Json request = {{"source_dataset_ids", Json::array()},
                  {"settlement_dataset_ids", Json::array()},
                  {"begin_day", ""},
                  {"end_day", ""},
                  {"contract", protocol::decode_contract(test::contract())}};
  for (const auto* role : {"source_dataset_ids", "settlement_dataset_ids"}) {
    std::set<std::string> ids;
    for (const auto& selected : selections)
      for (const auto& id : selected.at(role))
        ids.insert(id.get<std::string>());
    request[role] = ids;
  }
  return protocol::encode_bar_dataset_request(request);
}
} // namespace
TEST(NamedDatasets, ComposesDownloadsDeduplicatesAndRestoresFixedVersions) {
  TaskDirectory directory;
  const auto first =
      test::seed_history(directory.path, {100, 101}, "compose-first", {"2026-09-23"});
  const auto second =
      test::seed_history(directory.path, {102, 103}, "compose-second", {"2026-09-24"});
  const auto whole = test::seed_history(directory.path, {100, 101, 102, 103}, "compose-whole",
                                        {"2026-09-23", "2026-09-24"});
  auto store = std::make_unique<data::Store>(directory.path, "historical-data", "task");
  const auto request = composed_request({second, first});
  const auto result = data::resolve_bar_dataset(store->sources(request));
  EXPECT_EQ(result.bars_size(), 4);
  EXPECT_EQ(result.days_size(), 2);
  EXPECT_EQ(result.source_dataset_ids_size(), 2);
  EXPECT_EQ(result.uncovered_days_size(), 0);
  const auto overlapping =
      data::resolve_bar_dataset(store->sources(composed_request({first, whole, second})));
  EXPECT_EQ(overlapping.bars_size(), 4);
  EXPECT_EQ(overlapping.revision(), result.revision());
  EXPECT_EQ(data::resolve_bar_dataset(store->sources(composed_request({whole}))).revision(),
            result.revision());
  auto json = protocol::decode_bar_dataset_request(request);
  std::reverse(json["source_dataset_ids"].begin(), json["source_dataset_ids"].end());
  EXPECT_EQ(protocol::encode_bar_dataset_request(json).SerializeAsString(),
            request.SerializeAsString());
  history_files::Archive archive(directory.path / "history");
  data::v1::NamedDataset saved;
  saved.set_version(1);
  saved.set_name("composed");
  *saved.add_selections() = request;
  saved.add_content_revisions(result.revision());
  saved.set_id(protocol::named_dataset_revision(saved));
  archive.save_named_dataset(saved);
  store.reset();
  data::Store restarted(directory.path, "historical-data", "task");
  const auto restored = archive.named_dataset(saved.id());
  EXPECT_EQ(
      data::resolve_bar_dataset(restarted.sources(restored.selections(0))).SerializeAsString(),
      result.SerializeAsString());
  auto daily = request;
  *daily.mutable_source_dataset_ids() = daily.settlement_dataset_ids();
  const auto day_result = data::resolve_bar_dataset(restarted.sources(daily));
  EXPECT_EQ(day_result.interval_minutes(), 0);
  EXPECT_EQ(day_result.bars_size(), 2);
}
TEST(NamedDatasets, CompositionRejectsConflictsAndClipsBeforeComparing) {
  TaskDirectory directory;
  const auto one = test::seed_history(directory.path, {100, 101, 102, 103}, "conflict-one",
                                      {"2026-09-23", "2026-09-24"});
  const auto two = test::seed_history(directory.path, {999, 101}, "conflict-two", {"2026-09-23"});
  const auto settlement = test::seed_history(directory.path, {100, 101}, "settlement-conflict",
                                             {"2026-09-23"}, {}, "rb", 1, 120);
  data::Store store(directory.path, "historical-data", "task");
  auto request = composed_request({one, two});
  EXPECT_THROW(data::resolve_bar_dataset(store.sources(request)), std::invalid_argument);
  request.set_begin_day("2026-09-24");
  EXPECT_EQ(data::resolve_bar_dataset(store.sources(request)).bars_size(), 2);
  EXPECT_THROW(data::resolve_bar_dataset(store.sources(composed_request({one, settlement}))),
               std::invalid_argument);
}
TEST(NamedDatasets, CompositionRejectsMixedContractsIntervalsAndSemantics) {
  TaskDirectory directory;
  const auto one = test::seed_history(directory.path, {100, 101}, "mix-one");
  const auto contract =
      test::seed_history(directory.path, {100, 101}, "mix-contract", {"2026-09-25"}, {}, "cu");
  const auto interval =
      test::seed_history(directory.path, {100, 101}, "mix-interval", {"2026-09-25"}, {}, "rb", 5);
  const auto semantics = test::seed_history(directory.path, {100, 101}, "mix-semantics",
                                            {"2026-09-25"}, {}, "rb", 1, 110, "test.confirmed.v2");
  data::Store store(directory.path, "historical-data", "task");
  for (const auto& other : {contract, interval, semantics}) {
    auto request = composed_request({one, other});
    EXPECT_THROW(data::resolve_bar_dataset(store.sources(request)), std::invalid_argument);
  }
  // Verify bar semantics independently of settlement semantics.
  auto request = composed_request({one, semantics});
  *request.mutable_settlement_dataset_ids() = composed_request({one}).settlement_dataset_ids();
  EXPECT_THROW(data::resolve_bar_dataset(store.sources(request)), std::invalid_argument);
}
TEST(NamedDatasets, CompositionReportsGapsAndRejectsInvalidVersionLists) {
  TaskDirectory directory;
  const auto one = test::seed_history(directory.path, {100, 101}, "gap-one", {"2026-09-23"},
                                      {"2026-09-23", "2026-09-24", "2026-09-25"});
  const auto two = test::seed_history(directory.path, {102, 103}, "gap-two", {"2026-09-25"});
  data::Store store(directory.path, "historical-data", "task");
  auto request = composed_request({one, two});
  const auto result = data::resolve_bar_dataset(store.sources(request));
  ASSERT_EQ(result.uncovered_days_size(), 1);
  EXPECT_EQ(result.uncovered_days(0), "2026-09-24");
  auto json = protocol::decode_bar_dataset_request(request);
  json["source_dataset_ids"].push_back(json["source_dataset_ids"][0]);
  EXPECT_THROW(protocol::encode_bar_dataset_request(json), std::invalid_argument);
  json["source_dataset_ids"] = Json::array();
  EXPECT_THROW(protocol::encode_bar_dataset_request(json), std::invalid_argument);
  json["source_dataset_ids"] = std::vector<std::string>(33, std::string(64, 'a'));
  EXPECT_THROW(protocol::encode_bar_dataset_request(json), std::invalid_argument);
  // Retired singular wire fields are rejected, not interpreted as a different version list.
  request.GetReflection()->MutableUnknownFields(&request)->AddLengthDelimited(6,
                                                                              std::string(64, 'a'));
  EXPECT_THROW(protocol::decode_bar_dataset_request(request), std::invalid_argument);
}

TEST(HistoryUpdate, IncrementalMinuteAndDailyPlansStartAfterFixedBoundaries) {
  TaskDirectory directory;
  const auto seeded = test::seed_history(directory.path, {100, 101}, "update-base");
  history_files::Archive archive(directory.path / "history");
  auto query =
      protocol::encode_history_update_query({{"dataset_id", seeded.at("source_dataset_ids").at(0)},
                                             {"calendar_dataset_id", ""},
                                             {"mode", "extend"},
                                             {"end_day", "2026-09-26"},
                                             {"requests_per_minute", 60}});
  const auto now = parse_shanghai_time("2026-09-28 01:00:00");
  const auto before = archive.get(query.dataset_id()).SerializeAsString();
  const auto plan = data::history_update_plan(archive, query, now);
  EXPECT_EQ(plan.minutes().begin_ns(), parse_shanghai_time("2026-09-25 09:01:01"));
  EXPECT_EQ(plan.minutes().end_ns(), parse_shanghai_time("2026-09-26 23:59:59"));
  EXPECT_EQ(plan.minutes().source_instrument(), "RB2610.SHF");
  EXPECT_EQ(data::history_update_plan(archive, query, now).id(), plan.id());
  EXPECT_EQ(protocol::decode_history_update_plan(plan).at("query"),
            protocol::decode_history_update_query(query));
  EXPECT_EQ(archive.get(query.dataset_id()).SerializeAsString(), before);
  query.set_dataset_id(seeded.at("settlement_dataset_ids").at(0).get<std::string>());
  const auto day = data::history_update_plan(archive, query, now);
  EXPECT_EQ(day.daily().begin_day(), "2026-09-26");
  EXPECT_EQ(day.daily().end_day(), "2026-09-26");
  auto changed = day;
  changed.mutable_daily()->set_end_day("2026-09-27");
  EXPECT_THROW(protocol::decode_history_update_plan(changed), std::invalid_argument);
  query.set_end_day("2026-09-28");
  EXPECT_THROW(data::history_update_plan(archive, query, now), std::invalid_argument);
  query.set_end_day("2026-09-25");
  EXPECT_THROW(data::history_update_plan(archive, query, now), std::invalid_argument);
}
TEST(HistoryUpdate, RepairPinsCalendarAndPreservesNightSessionWindow) {
  TaskDirectory directory;
  const auto seeded =
      test::seed_history(directory.path, {100, 101, 102, 103}, "update-gaps",
                         {"2026-09-23", "2026-09-25"}, {"2026-09-23", "2026-09-24", "2026-09-25"});
  const auto other =
      test::seed_history(directory.path, {100, 101}, "update-other", {"2026-09-25"}, {}, "cu");
  history_files::Archive archive(directory.path / "history");
  auto query = protocol::encode_history_update_query(
      {{"dataset_id", seeded.at("source_dataset_ids").at(0)},
       {"calendar_dataset_id", seeded.at("settlement_dataset_ids").at(0)},
       {"mode", "repair"},
       {"end_day", ""},
       {"requests_per_minute", 60}});
  const auto now = parse_shanghai_time("2026-09-28 01:00:00");
  const auto source = archive.get(query.dataset_id());
  const auto plan = data::history_update_plan(archive, query, now);
  ASSERT_EQ(plan.missing_days_size(), 1);
  EXPECT_EQ(plan.missing_days(0), "2026-09-24");
  EXPECT_EQ(plan.minutes().begin_ns(), source.minutes().begin_ns());
  EXPECT_EQ(plan.minutes().end_ns(), source.minutes().end_ns());
  EXPECT_EQ(plan.minutes().requests_per_minute(), 60);
  query.set_calendar_dataset_id(other.at("settlement_dataset_ids").at(0).get<std::string>());
  EXPECT_THROW(data::history_update_plan(archive, query, now), std::invalid_argument);
  query.set_dataset_id(other.at("source_dataset_ids").at(0).get<std::string>());
  EXPECT_THROW(data::history_update_plan(archive, query, now), std::invalid_argument);
  query.set_dataset_id(other.at("settlement_dataset_ids").at(0).get<std::string>());
  EXPECT_THROW(data::history_update_plan(archive, query, now), std::invalid_argument);
}
TEST(HistoryUpdate, CorruptBaseAndMalformedQueryCannotProducePlans) {
  TaskDirectory directory;
  const auto seeded = test::seed_history(directory.path, {100, 101}, "update-corrupt");
  history_files::Archive archive(directory.path / "history");
  auto query =
      protocol::encode_history_update_query({{"dataset_id", seeded.at("source_dataset_ids").at(0)},
                                             {"calendar_dataset_id", ""},
                                             {"mode", "extend"},
                                             {"end_day", "2026-09-26"},
                                             {"requests_per_minute", 60}});
  const auto now = parse_shanghai_time("2026-09-28 01:00:00");
  const auto path =
      std::filesystem::path(archive.get(query.dataset_id()).minute_result().directory()) /
      "minutes.json";
  std::ifstream file(path);
  Json manifest;
  file >> manifest;
  manifest["rows"] = manifest.at("rows").get<unsigned>() + 1;
  replace_file_durably(path, manifest.dump());
  EXPECT_THROW(data::history_update_plan(archive, query, now), std::invalid_argument);
  auto json = protocol::decode_history_update_query(query);
  json["requests_per_minute"] = 501;
  EXPECT_THROW(protocol::encode_history_update_query(json), std::invalid_argument);
  json["requests_per_minute"] = 1;
  json["mode"] = "automatic";
  EXPECT_THROW(protocol::encode_history_update_query(json), std::invalid_argument);
  json["mode"] = "repair";
  EXPECT_THROW(protocol::encode_history_update_query(json), std::invalid_argument);
}

TEST_F(TaskProcess, AuthorizedDownloadPersistsExactReviewedRange) {
  service.reset();
  const auto seeded = test::seed_history(warehouse.path, {100, 101}, "update-rpc");
  start_data();
  start();
  protocol::DataClient data(endpoint + ".data", "historical-data");
  data::v1::DataRequest preview;
  *preview.mutable_update_plan() =
      protocol::encode_history_update_query({{"dataset_id", seeded.at("source_dataset_ids").at(0)},
                                             {"calendar_dataset_id", ""},
                                             {"mode", "extend"},
                                             {"end_day", "2026-09-26"},
                                             {"requests_per_minute", 60}});
  const auto plan = data.call(preview).update_plan();
  task_wire::TaskRequest submit;
  submit.mutable_submit()->set_id("incremental");
  data::v1::DataRequest capture;
  auto* authorization = capture.mutable_authorize_download();
  authorization->set_task_instance("task");
  authorization->set_task_id("incremental");
  authorization->set_credential("fixture");
  *authorization->mutable_minutes() = plan.minutes();
  const auto captured = data.call(capture).download_authorization();
  submit.mutable_submit()->set_download_authorization(captured.id());
  submit.mutable_submit()->set_id("wrong-task");
  EXPECT_THROW(call(submit), std::runtime_error);
  submit.mutable_submit()->set_id("incremental");
  task_wire::TaskRequest get;
  get.mutable_get()->set_id("incremental");
  EXPECT_THROW(call(get), std::runtime_error);
  const auto created = call(submit).task();
  EXPECT_EQ(created.state(), task_wire::QUEUED);
  EXPECT_EQ(created.definition_case(), task_wire::Task::DEFINITION_NOT_SET);
  EXPECT_EQ(call(get).task().minutes().SerializeAsString(), plan.minutes().SerializeAsString());
  EXPECT_EQ(call(submit).task().SerializeAsString(), created.SerializeAsString());
  service.reset();
  start();
  EXPECT_EQ(call(get).task().minutes().SerializeAsString(), plan.minutes().SerializeAsString());
  EXPECT_EQ(data.call(preview).update_plan().id(), plan.id());
}

TEST_F(TaskProcess, HistoryUsageKeepsTaskReferencesAfterCancellationAndRestart) {
  service.reset();
  std::vector<int> prices(40);
  for (std::size_t i = 0; i < prices.size(); ++i)
    prices[i] = 100 + static_cast<int>(i) + static_cast<int>(i % 3);
  const auto seeded = test::seed_history(warehouse.path, prices, "usage");
  const auto request = composed_request({seeded});
  {
    tasks::Store store(directory.path, tasks::Identity{"task", "historical-data"});
    data::Store data_store(warehouse.path, "historical-data", "task");
    const auto data = data::resolve_bar_dataset(data_store.sources(request));
    auto backtest = input();
    *backtest.mutable_paper()->mutable_contracts(0)->mutable_dataset() = data;
    backtest.set_dataset_revision(protocol::dataset_revision(backtest.paper()));
    tasks::submit(store, "usage-backtest", backtest);
    store.commit(store.cancel("usage-backtest")).task();
    factor::v1::FactorInput factor;
    factor.set_version(6);
    *factor_bars(factor) = data;
    factor.set_dataset_revision(data.revision());
    factor.add_lookbacks(1);
    factor.set_horizon(1);
    factor.set_full_sample(true);
    tasks::submit(store, "usage-factor", factor);
    store.commit(store.cancel("usage-factor")).task();
  }
  start();
  task_wire::TaskRequest query;
  query.mutable_history_usage()->set_id(seeded.at("source_dataset_ids").at(0).get<std::string>());
  const auto first = call(query).history_usage();
  ASSERT_EQ(first.references_size(), 2);
  std::set<int> kinds;
  for (const auto& row : first.references()) {
    kinds.insert(row.kind());
    ASSERT_EQ(row.roles_size(), 1);
    EXPECT_EQ(row.roles(0), data::v1::HISTORY_MARKET);
  }
  EXPECT_EQ(kinds, (std::set<int>{data::v1::HISTORY_BACKTEST, data::v1::HISTORY_FACTOR}));
  EXPECT_EQ(protocol::decode_history_usage(first).at("references").size(), 2);
  service.reset();
  start();
  EXPECT_EQ(call(query).history_usage().SerializeAsString(), first.SerializeAsString());
  query.mutable_history_usage()->set_id(
      seeded.at("settlement_dataset_ids").at(0).get<std::string>());
  const auto settlement = call(query).history_usage();
  ASSERT_EQ(settlement.references_size(), 2);
  for (const auto& row : settlement.references())
    EXPECT_EQ(row.roles(0), data::v1::HISTORY_SETTLEMENT);
  query.mutable_history_usage()->set_id(std::string(64, '0'));
  EXPECT_EQ(call(query).history_usage().references_size(), 0);
  query.mutable_history_usage()->set_id("../escape");
  EXPECT_THROW(call(query), std::runtime_error);
  EXPECT_FALSE(std::filesystem::exists(directory.path / "history"));
  data_service.reset();
  std::filesystem::remove_all(warehouse.path); // Only this test's isolated warehouse.
  query.mutable_history_usage()->set_id(first.dataset_id());
  EXPECT_EQ(call(query).history_usage().SerializeAsString(), first.SerializeAsString());
}
TEST_F(TaskProcess, HistoryUsageFindsImportedTaskWithoutLocalArchiveCopy) {
  service.reset();
  const auto spec = input();
  {
    tasks::Store store(directory.path, tasks::Identity{"task", "historical-data"});
    tasks::submit(store, "external-origin", spec);
    store.commit(store.cancel("external-origin")).task();
  }
  EXPECT_FALSE(std::filesystem::exists(directory.path / "history"));
  start();
  task_wire::TaskRequest query;
  query.mutable_history_usage()->set_id(spec.paper().contracts(0).dataset().source_dataset_ids(0));
  const auto result = call(query).history_usage();
  ASSERT_EQ(result.references_size(), 1);
  EXPECT_EQ(result.references(0).id(), "external-origin");
  EXPECT_EQ(result.references(0).kind(), data::v1::HISTORY_BACKTEST);
  EXPECT_EQ(result.references(0).roles(0), data::v1::HISTORY_MARKET);
  service.reset();
  start();
  EXPECT_EQ(call(query).history_usage().SerializeAsString(), result.SerializeAsString());
}
TEST(HistoryUsage, RejectsUnknownKindsDuplicateReferencesAndInvalidRoles) {
  data::v1::HistoryUsage usage;
  usage.set_dataset_id(std::string(64, 'a'));
  EXPECT_TRUE(protocol::decode_history_usage(usage).at("references").empty());
  auto* row = usage.add_references();
  row->set_id("consumer");
  row->set_kind(data::v1::HISTORY_BACKTEST);
  row->add_roles(data::v1::HISTORY_MARKET);
  row->add_roles(data::v1::HISTORY_SETTLEMENT);
  EXPECT_EQ(protocol::decode_history_usage(usage).at("references").at(0).at("roles").size(), 2);
  const auto valid = usage;
  *usage.add_references() = usage.references(0);
  EXPECT_THROW(protocol::decode_history_usage(usage), std::invalid_argument);
  usage = valid;
  usage.mutable_references(0)->set_kind(data::v1::HISTORY_REFERENCE_UNSPECIFIED);
  EXPECT_THROW(protocol::decode_history_usage(usage), std::invalid_argument);
  usage = valid;
  usage.mutable_references(0)->add_roles(data::v1::HISTORY_OUTPUT);
  EXPECT_THROW(protocol::decode_history_usage(usage), std::invalid_argument);
}

TEST(HistoryUsage, StoppedLedgerInspectionDoesNotRecoverTasksOrChangeFiles) {
  TaskDirectory directory, warehouse;
  const auto seeded = test::seed_history(warehouse.path, {100, 101, 102}, "stopped-usage");
  const auto request = composed_request({seeded});
  {
    tasks::Store store(directory.path, tasks::Identity{"task", "historical-data"});
    data::Store data_store(warehouse.path, "historical-data", "task");
    const auto data = data::resolve_bar_dataset(data_store.sources(request));
    auto task = input();
    *task.mutable_paper()->mutable_contracts(0)->mutable_dataset() = data;
    task.set_dataset_revision(protocol::dataset_revision(task.paper()));
    tasks::submit(store, "running", task);
    (void)store.commit(store.claim("running")).token();
    EXPECT_THROW(tasks::Store::inspect_history_usage(directory.path,
                                                     tasks::Identity{"task", "historical-data"},
                                                     seeded.at("source_dataset_ids")[0]),
                 std::exception);
  }
  const auto fingerprints = [&] {
    std::map<std::string, std::string> files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory.path))
      if (entry.is_regular_file())
        files[entry.path().lexically_relative(directory.path).string()] = sha256_file(entry.path());
    return files;
  };
  const auto before = fingerprints();
  for (const auto& key : {"source_dataset_ids", "settlement_dataset_ids"}) {
    const auto usage = tasks::Store::inspect_history_usage(
        directory.path, tasks::Identity{"task", "historical-data"}, seeded.at(key)[0]);
    EXPECT_EQ(usage.references_size(), 1); // Only the still-running task owns this reference.
  }
  auto after = fingerprints();
  // SQLite READONLY may create its own coordination files; never ignore a pre-existing WAL.
  if (!before.contains("tasks.sqlite-wal")) {
    if (after.contains("tasks.sqlite-wal"))
      EXPECT_EQ(after.at("tasks.sqlite-wal"), sha256_bytes(""));
    after.erase("tasks.sqlite-wal");
  }
  if (!before.contains("tasks.sqlite-shm"))
    after.erase("tasks.sqlite-shm");
  EXPECT_EQ(after, before);
  {
    sqlite::Database db(directory.path / "tasks.sqlite", sqlite::Database::Access::read_only);
    sqlite::Database::Statement state(db, "SELECT state FROM tasks WHERE id='running'");
    ASSERT_TRUE(state.step());
    EXPECT_EQ(state.integer(0), task::v1::RUNNING);
  }
  const auto lock = directory.path / "manager.lock";
  std::filesystem::remove(lock);
  EXPECT_THROW(tasks::Store::inspect_history_usage(directory.path,
                                                   tasks::Identity{"task", "historical-data"},
                                                   seeded.at("source_dataset_ids")[0]),
               std::exception);
  EXPECT_FALSE(std::filesystem::exists(lock));
  EXPECT_THROW(tasks::Store::inspect_history_usage(directory.path / "missing",
                                                   tasks::Identity{"task", "historical-data"},
                                                   std::string(64, 'a')),
               std::exception);
  EXPECT_FALSE(std::filesystem::exists(directory.path / "missing"));
}

TEST(HistoryUsage, StoppedInspectionRejectsCorruptStateWithoutRepair) {
  TaskDirectory directory;
  {
    tasks::Store store(directory.path, tasks::Identity{"task", "historical-data"});
    tasks::submit(store, "bad", input());
  }
  {
    sqlite::Database db(directory.path / "tasks.sqlite");
    db.execute("UPDATE tasks SET manifest='{}'");
  }
  const auto before = sha256_file(directory.path / "tasks.sqlite");
  EXPECT_THROW(tasks::Store::inspect_history_usage(directory.path,
                                                   tasks::Identity{"task", "historical-data"},
                                                   std::string(64, 'a')),
               std::exception);
  EXPECT_EQ(sha256_file(directory.path / "tasks.sqlite"), before);
  EXPECT_FALSE(std::filesystem::exists(directory.path / "history"));
}

TEST(TaskStore, MixedHistoricalWarehouseIsRejectedWithoutChangingFiles) {
  TaskDirectory directory;
  std::filesystem::create_directory(directory.path / "history");
  const auto evidence = directory.path / "history" / "preserve.bin";
  replace_file_durably(evidence, "owned historical data");
  EXPECT_THROW(tasks::Store rejected(directory.path, tasks::Identity{"task", "historical-data"}),
               std::invalid_argument);
  EXPECT_EQ(sha256_file(evidence), sha256_bytes("owned historical data"));
  EXPECT_FALSE(std::filesystem::exists(directory.path / "tasks.sqlite"));
  EXPECT_FALSE(std::filesystem::exists(directory.path / "manager.lock"));
}

TEST(TaskStore, ForeignStoreIsRejectedBeforeCreatingTaskSchema) {
  TaskDirectory directory;
  {
    sqlite::Database database(directory.path / "tasks.sqlite");
    database.execute("CREATE TABLE foreign_data(value TEXT)");
    database.execute("INSERT INTO foreign_data VALUES('preserve')");
  }
  const auto before = sha256_file(directory.path / "tasks.sqlite");
  EXPECT_THROW(tasks::Store rejected(directory.path, tasks::Identity{"task", "historical-data"}),
               std::exception);
  EXPECT_EQ(sha256_file(directory.path / "tasks.sqlite"), before);
  EXPECT_FALSE(std::filesystem::exists(directory.path / "manager.lock"));
  EXPECT_FALSE(std::filesystem::exists(directory.path / "history"));
  sqlite::Database inspect(directory.path / "tasks.sqlite", sqlite::Database::Access::read_only);
  sqlite::Database::Statement tables(inspect,
                                     "SELECT COUNT(*) FROM sqlite_schema WHERE type='table'");
  ASSERT_TRUE(tables.step());
  EXPECT_EQ(tables.integer(0), 1);
}

TEST_F(TaskProcess, WorkerAttemptJoinsServiceDiagnosticsWithoutItsFenceToken) {
  service.reset();
  const char* value = std::getenv("ASTERION_LOG_DIRECTORY");
  const std::optional<std::string> saved = value ? std::optional<std::string>(value) : std::nullopt;
  struct Restore {
    const std::optional<std::string>& saved;
    ~Restore() {
      if (saved)
        ::setenv("ASTERION_LOG_DIRECTORY", saved->c_str(), 1);
      else
        ::unsetenv("ASTERION_LOG_DIRECTORY");
    }
  } restore{saved};
  const auto logs = directory.path / "logs";
  ASSERT_EQ(::setenv("ASTERION_LOG_DIRECTORY", logs.c_str(), 1), 0);
  submit("traced-task");
  {
    ChildProcess worker(ASTERION_BACKTEST_PATH,
                        {"--endpoint", endpoint, "--session", "task", "--task", "traced-task"});
    ASSERT_TRUE(worker.wait(asterion::testing_support::bound(15s)));
    ASSERT_EQ(worker.exit_code(), 0);
  }
  service.reset();
  std::vector<Json> records;
  for (const auto& entry : std::filesystem::directory_iterator(logs)) {
    std::ifstream stream(entry.path());
    std::string line;
    while (std::getline(stream, line)) {
      const auto record = Json::parse(line);
      const auto& fields = record.at("fields");
      EXPECT_FALSE(fields.contains("token"));
      EXPECT_FALSE(fields.contains("provider_token"));
      EXPECT_FALSE(fields.contains("input"));
      records.push_back(record);
    }
  }
  const auto claimed = std::find_if(records.begin(), records.end(), [](const Json& record) {
    return record.at("event") == "task.claimed";
  });
  ASSERT_NE(claimed, records.end());
  EXPECT_EQ(claimed->at("fields").at("task_id"), "traced-task");
  EXPECT_EQ(claimed->at("fields").at("attempt"), 1);
  const auto trace = claimed->at("fields").at("trace_id");
  unsigned linked = 0;
  for (const auto& record : records) {
    const auto& fields = record.at("fields");
    if (record.at("logger") != "task-service" || record.at("event") != "rpc.completed" ||
        (fields.at("operation") != "claim" && fields.at("operation") != "finish"))
      continue;
    EXPECT_EQ(fields.at("task_id"), "traced-task");
    EXPECT_EQ(fields.at("attempt"), 1);
    EXPECT_EQ(fields.at("success"), true);
    EXPECT_TRUE(std::any_of(records.begin(), records.end(), [&](const Json& link) {
      return link.at("event") == "rpc.started" && link.at("fields").at("trace_id") == trace &&
             link.at("fields").at("correlation_id") == fields.at("correlation_id");
    }));
    ++linked;
  }
  EXPECT_EQ(linked, 2U);
}

TEST(TaskStore, RetainedExperimentsContinueInTheSameWarehouseBeyondOneThousandTasks) {
  TaskDirectory directory, warehouse;
  std::vector<int> prices(40);
  for (std::size_t i = 0; i < prices.size(); ++i)
    prices[i] = 100 + static_cast<int>(i) + static_cast<int>(i % 3);
  const auto selection = test::seed_history(warehouse.path, prices, "long-lived");
  data::Store data_store(warehouse.path, "historical-data", "task");
  const auto source = data::resolve_bar_dataset(data_store.sources(composed_request({selection})));
  factor::v1::FactorInput spec;
  spec.set_version(6);
  *factor_bars(spec) = source;
  spec.set_dataset_revision(source.revision());
  spec.add_lookbacks(1);
  spec.set_horizon(1);
  spec.set_full_sample(true);
  const auto expected = factor::run(spec);
  const auto dataset_id = selection.at("source_dataset_ids").at(0).get<std::string>();
  std::string original_hash;
  {
    tasks::Store store(directory.path, tasks::Identity{"task", "historical-data"});
    for (unsigned i = 0; i < 1001; ++i) {
      const auto id = "experiment-" + std::to_string(i);
      const auto submitted = tasks::submit(store, id, spec);
      ASSERT_EQ(submitted.submission_sequence(), i + 1);
      store.commit(tasks::finish(store, id, store.commit(store.claim(id)).token(), expected));
    }
    const auto page = store.list();
    EXPECT_EQ(page.capacity().retained_tasks(), 1001);
    EXPECT_EQ(page.capacity().active_used(), 0);
    EXPECT_EQ(page.tasks_size(), 200);
    EXPECT_EQ(page.tasks(199).submission_sequence(), 1001);
    EXPECT_NO_THROW(protocol::decode_task(page.tasks(199)));
    original_hash = sha256_file(directory.path / "experiment-0/results/1.pb");
    auto usage = store.prepare_history_usage(dataset_id);
    EXPECT_EQ(tasks::submit(store, "experiment-0", spec).state(), task::v1::SUCCEEDED);
    tasks::submit(store, "retry-original", spec);
    store.commit(store.fail("retry-original", store.commit(store.claim("retry-original")).token(),
                            "fixture failure"));
    unsigned pages = 0;
    while (store.next_history_page(usage)) {
      ++pages;
      auto reading = std::async(std::launch::async, [&] { usage.load_page(); });
      EXPECT_EQ(store.describe("retry-original").state(), task::v1::FAILED);
      reading.get();
    }
    EXPECT_EQ(pages, 6U);
    EXPECT_EQ(usage.take().references_size(), 1001);
  }
  {
    tasks::Store restored(directory.path, tasks::Identity{"task", "historical-data"});
    EXPECT_EQ(tasks::factor_result(restored, "experiment-0").SerializeAsString(),
              expected.SerializeAsString());
    EXPECT_EQ(tasks::factor_result(restored, "experiment-1000").SerializeAsString(),
              expected.SerializeAsString());
    EXPECT_EQ(sha256_file(directory.path / "experiment-0/results/1.pb"), original_hash);
    EXPECT_EQ(restored.history_usage(dataset_id).references_size(), 1002);
    const auto retried = restored.commit(restored.retry("retry-original")).task();
    EXPECT_EQ(retried.submission_sequence(), 1002);
    EXPECT_EQ(retried.attempt(), 1);
    EXPECT_EQ(restored.list().capacity().active_used(), 1);
    restored.commit(tasks::finish(restored, "retry-original",
                                  restored.commit(restored.claim("retry-original")).token(),
                                  expected));
    EXPECT_EQ(restored.describe("retry-original").attempt(), 2);
    EXPECT_EQ(restored.list().capacity().active_used(), 0);
    EXPECT_EQ(data::resolve_bar_dataset(data_store.sources(composed_request({selection})))
                  .SerializeAsString(),
              source.SerializeAsString());
  }
  // Membership is part of the recovery index, never authority to silently skip history.
  {
    sqlite::Database database(directory.path / "tasks.sqlite");
    database.execute("DELETE FROM task_event_members WHERE task_id='experiment-0'");
  }
  const auto evidence = sha256_file(directory.path / "tasks.sqlite");
  EXPECT_THROW((tasks::Store(directory.path, tasks::Identity{"task", "historical-data"})),
               std::invalid_argument);
  EXPECT_EQ(sha256_file(directory.path / "tasks.sqlite"), evidence);
}

TEST(TaskStore, ActiveCapacityAppliesToNewAdmissionsAndExplicitRetries) {
  TaskDirectory directory;
  factor::v1::FactorInput spec;
  spec.set_version(6);
  *factor_bars(spec) = input().paper().contracts(0).dataset();
  auto* data = factor_bars(spec);
  const auto first = data->bars(0);
  data->clear_bars();
  for (std::int64_t i = 0; i < 40; ++i) {
    auto* bar = data->add_bars();
    *bar = first;
    bar->set_timestamp_ns(first.timestamp_ns() + i * 1000000000);
  }
  data->set_revision(protocol::bar_dataset_revision(*data));
  spec.set_dataset_revision(spec.series(0).bars().revision());
  spec.add_lookbacks(1);
  spec.set_horizon(1);
  spec.set_full_sample(true);
  tasks::Store store(directory.path, tasks::Identity{"task", "historical-data"});
  for (unsigned i = 0; i < 1000; ++i)
    tasks::submit(store, "queued-" + std::to_string(i), spec);
  EXPECT_EQ(store.list().capacity().active_used(), 1000);
  EXPECT_THROW(tasks::submit(store, "next", spec), std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(directory.path / "next"));
  EXPECT_EQ(store.commit(store.cancel("queued-0")).task().state(), task::v1::CANCELLED);
  EXPECT_EQ(store.list().capacity().active_used(), 999);
  auto pending = store.submission("next", spec);
  store.admit_submission(pending);
  EXPECT_EQ(store.list().capacity().active_reserved(), 1U);
  EXPECT_FALSE(std::filesystem::exists(directory.path / "next"));
  EXPECT_THROW(store.commit(store.retry("queued-0")), std::invalid_argument);
  pending.prepare_files();
  EXPECT_EQ(store.commit(store.register_submission(pending)).task().submission_sequence(), 1001);
  EXPECT_EQ(store.list().capacity().active_reserved(), 0U);
  EXPECT_THROW(store.commit(store.retry("queued-0")).task(), std::invalid_argument);
  EXPECT_EQ(store.describe("queued-0").state(), task::v1::CANCELLED);
  store.commit(store.cancel("next")).task();
  EXPECT_EQ(store.commit(store.retry("queued-0")).task().submission_sequence(), 1);
  EXPECT_EQ(store.list().capacity().retained_tasks(), 1001);
  EXPECT_EQ(store.list().capacity().active_used(), 1000);
}
TEST(TaskStore, AComparisonCountsEveryReplayAndKeepsItsScores) {
  TaskDirectory directory;
  const auto spec = comparison();
  const auto result = backtest::run(spec);
  {
    tasks::Store store(directory.path, tasks::Identity{"task", "historical-data"});
    const auto task = tasks::submit(store, "compared", spec);
    EXPECT_EQ(task.total(), 80U);
    const auto token = store.commit(store.claim("compared")).token();
    // A result without the scores of what it compared is not this task's.
    auto bare = result;
    bare.clear_candidates();
    bare.clear_selected();
    EXPECT_THROW(store.commit(tasks::finish(store, "compared", token, bare)),
                 std::invalid_argument);
    // Nor is one that names another strategy than the best of them.
    auto misnamed = result;
    misnamed.set_selected(0);
    EXPECT_THROW(store.commit(tasks::finish(store, "compared", token, misnamed)),
                 std::invalid_argument);
    store.commit(tasks::finish(store, "compared", token, result));
  }
  tasks::Store restored(directory.path, tasks::Identity{"task", "historical-data"});
  EXPECT_EQ(restored.get("compared").state(), task::v1::SUCCEEDED);
}
