#include "strategy_store.hpp"
#include "research_submission.hpp"
#include "data_submission.hpp"
#include <asterion/research.pb.h>
#include "support/task_wait.hpp"
#include <asterion/protocol/data_client.hpp>
#include "tasks/paper_input.hpp"
#include "performance.hpp"
#include "portfolio_budget.hpp"
#include "account_reconciliation.hpp"
#include "support/local_listener.hpp"
#include "support/data_discovery.hpp"
#include <asterion/protocol/factor.hpp>
#include "history_update.hpp"
#include "sqlite_database.hpp"
#include "task_events.hpp"
#include <asterion/kernel/durable_file.hpp>
#include "engine.hpp"
#include "factor_engine.hpp"
#include "risk_module.hpp"
#include "data/bar_fixture.hpp"
#include "data/shfe_margin_fixture.hpp"
#include "historical_margins.hpp"
#include "data/history_fixture.hpp"
#include "bar_dataset_source.hpp"
#include "task_store.hpp"
#include "research_host.hpp"
#include "research_backup.hpp"
#include "research_execution.hpp"
#include "program_files.hpp"
#include "research_data.hpp"
#include "research_configuration.hpp"
#include "tasks/task_store_support.hpp"
#include "support/timing.hpp"
#include "verification_slots.hpp"

#include <asterion/protocol/task_execution.hpp>
#include <asterion/protocol/research_variable.hpp>
#include <asterion/protocol/research_strategy.hpp>
#include <asterion/protocol/research_logic.hpp>
#include <asterion/protocol/resource.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/child.hpp>
#include <fstream>
#include <cstdlib>
#include <future>
#include <gtest/gtest.h>
#include <map>
using namespace asterion;

namespace {

Decimal d(const char* text)
{
    return Decimal::parse(text);
}

// Compares the input's strategies as a request for them would: before a
// holdout from `from`, or by rolling when training days are given.
void compare(
    backtest::proto::BacktestInput& input,
    const std::string& from,
    unsigned training = 0,
    unsigned validation = 0)
{
    backtest::proto::BacktestRequest request;
    *request.mutable_strategies() = input.strategies();
    // This comparison fixture explicitly chooses its numerical scoring assumptions.
    request.mutable_performance_assumptions()->set_annual_periods(252);
    request.mutable_performance_assumptions()->set_risk_free(
        backtest::proto::PerformanceAssumptions::EXPLICIT_ZERO);
    request.set_holdout_from(from);
    if (training) {
        request.mutable_walk_forward()->set_training_days(training);
        request.mutable_walk_forward()->set_validation_days(validation);
    }
    protocol::set_backtest_strategies(input, request);
}

backtest::proto::BacktestInput input(
    const std::vector<int>& prices = {100, 101, 102, 101, 100, 101, 103},
    const char* settlement = "103")
{
    std::vector<MarketBar> bars;
    std::int64_t time = 1790298000000000000LL;
    for (const auto price : prices) {
        bars.push_back(test::flat("2026-09-25", time, std::to_string(price).c_str(), "10"));
        time += 1000000000;
    }
    return testing_support::backtest_input(test::dataset(bars, {{"2026-09-25", d(settlement)}}));
}

// Thirty trading days of one bar each, rising two a day with a dip every
// third. A bar opens at the close before it and trades one below, so a limit
// at that close fills. Two moving averages of 1 and 2 are compared on the 25
// days before 2026-10-20: one may only be short, the other only long.
backtest::proto::BacktestInput comparison()
{
    using namespace std::chrono;
    std::vector<MarketBar> bars;
    std::vector<DaySettlement> settlements;
    int previous = 100;
    for (int i = 0; i < 30; ++i) {
        const int close = 100 + 2 * i - (i % 3 == 2 ? 3 : 0);
        const auto day =
            format_trading_date(year_month_day{sys_days{year{2026} / September / 25} + days{i}});
        const auto text = [](int value) {
            return std::to_string(value);
        };
        bars.push_back(
            test::bar(
                day,
                1790298000000000000LL + i * 86400LL * 1000000000,
                text(previous).c_str(),
                text(std::max(previous, close)).c_str(),
                text(std::min(previous, close) - 1).c_str(),
                text(close).c_str(),
                "10"));
        settlements.push_back({day, d(text(close).c_str())});
        previous = close;
    }
    auto result = testing_support::backtest_input(test::dataset(bars, settlements));
    *result.mutable_strategies(0) = testing_support::moving_average(1, 2, "1", "short");
    *result.add_strategies() = testing_support::moving_average(1, 2, "1", "long");
    compare(result, "2026-10-20");
    return result;
}

// Sixty such days: the first thirty rise as above, the last thirty fall two a
// day with a bump every third. The same two averages are compared by rolling,
// twenty days of training and ten of validation: four rounds, the
// last of which trains on falling days alone.
backtest::proto::BacktestInput rolling_comparison()
{
    using namespace std::chrono;
    std::vector<MarketBar> bars;
    std::vector<DaySettlement> settlements;
    int previous = 100;
    for (int i = 0; i < 60; ++i) {
        const int close =
            i < 30 ? 100 + 2 * i - (i % 3 == 2 ? 3 : 0) : 158 - 2 * (i - 29) + (i % 3 == 2 ? 3 : 0);
        const auto day =
            format_trading_date(year_month_day{sys_days{year{2026} / September / 25} + days{i}});
        const auto text = [](int value) {
            return std::to_string(value);
        };
        bars.push_back(
            test::bar(
                day,
                1790298000000000000LL + i * 86400LL * 1000000000,
                text(previous).c_str(),
                text(std::max(previous, close)).c_str(),
                text(std::min(previous, close) - 1).c_str(),
                text(close).c_str(),
                "10"));
        settlements.push_back({day, d(text(close).c_str())});
        previous = close;
    }
    auto result = testing_support::backtest_input(test::dataset(bars, settlements));
    *result.mutable_strategies(0) = testing_support::moving_average(1, 2, "1", "short");
    *result.add_strategies() = testing_support::moving_average(1, 2, "1", "long");
    compare(result, "", 20, 10);
    return result;
}

// The bars of a factor input's one series, which is added when there is none yet.
data::proto::BarDataset* factor_bars(factor::proto::FactorInput& input)
{
    if (input.series().empty())
        input.add_series();
    return input.mutable_series(0)->mutable_bars();
}

factor::proto::FactorInput maximum_factor_input()
{
    factor::proto::FactorInput spec;
    spec.set_factor(protocol::proto::PRICE_MOMENTUM);
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

TEST(Backtest, HashUsesExactSnapshotAndExcludesExperimentCosts)
{
    EXPECT_EQ(
        sha256_bytes("abc"),
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

TEST(Backtest, DeterministicNextTickExecutionFeesAndDrawdown)
{
    const auto spec = input();
    const auto result = backtest::run(spec);
    EXPECT_EQ(result.SerializeAsString(), backtest::run(spec).SerializeAsString());
    ASSERT_EQ(result.account().fills_size(), 2);
    EXPECT_EQ(result.account().fills(0).price().units(), d("101").raw());
    EXPECT_EQ(result.account().fills(1).price().units(), d("101").raw());
    EXPECT_EQ(result.account().fees().units(), d("5").raw());
    EXPECT_EQ(result.account().fills(0).fee().units(), d("2").raw());
    EXPECT_EQ(result.account().fills(1).fee().units(), d("3").raw());
    auto missing_fee = result;
    missing_fee.mutable_account()->mutable_fills(0)->clear_fee();
    EXPECT_THROW(protocol::decode_backtest_result(missing_fee), std::invalid_argument);
    auto inconsistent_fee = result;
    inconsistent_fee.mutable_account()->mutable_fills(0)->mutable_fee()->set_units(d("1").raw());
    EXPECT_THROW(protocol::decode_backtest_result(inconsistent_fee), std::invalid_argument);
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

TEST(Backtest, DailySignalsTradeNextDayWithNewCostsAndYesterdayPositions)
{
    auto spec = input();
    std::vector<MarketBar> bars;
    const std::vector<std::string>
        days{"2026-09-21", "2026-09-22", "2026-09-23", "2026-09-24", "2026-09-25"};
    const std::vector<const char*> prices{"100", "102", "104", "100", "101"};
    for (std::size_t i = 0; i < days.size(); ++i)
        bars.push_back(
            test::flat(
                days[i],
                1'790'000'000'000'000'000LL + static_cast<std::int64_t>(i) * 86'400'000'000'000LL,
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
    const auto original_bytes = result.SerializeAsString();
    for (const auto& settled : result.settlements()) {
        ASSERT_TRUE(settled.has_occupancy());
        ASSERT_TRUE(settled.occupancy().has_margin());
        ASSERT_TRUE(settled.occupancy().has_available());
        Decimal margin;
        for (const auto& row : settled.contracts()) {
            ASSERT_TRUE(row.has_gross_quantity());
            ASSERT_TRUE(row.has_margin());
            margin = margin + Decimal::from_raw(row.margin().units());
        }
        EXPECT_EQ(margin.raw(), settled.occupancy().margin().units());
    }
    auto old_result = result;
    for (auto& settled : *old_result.mutable_settlements()) {
        settled.clear_occupancy();
        for (auto& row : *settled.mutable_contracts()) {
            row.clear_gross_quantity();
            row.clear_margin();
        }
    }
    const auto old_bytes = old_result.SerializeAsString();
    const auto old_decoded = protocol::decode_backtest_result(old_result);
    EXPECT_FALSE(old_decoded.at("settlements").at(0).contains("occupancy"));
    const auto old_path = settlement_exposure(spec, old_result),
               new_path = settlement_exposure(spec, result);
    ASSERT_EQ(old_path.size(), days.size());
    for (std::size_t i = 0; i < days.size(); ++i) {
        EXPECT_FALSE(old_path[i].gross_notional);
        EXPECT_FALSE(old_path[i].margin);
        EXPECT_FALSE(old_path[i].available);
        EXPECT_EQ(old_path[i].net_notional, new_path[i].net_notional);
        EXPECT_TRUE(new_path[i].gross_notional);
    }
    EXPECT_EQ(old_result.SerializeAsString(), old_bytes);
    EXPECT_EQ(result.SerializeAsString(), original_bytes);
    auto partial = result;
    partial.mutable_settlements(3)->mutable_contracts(0)->clear_margin();
    EXPECT_THROW(protocol::decode_backtest_result(partial), std::invalid_argument);
    auto inconsistent = result;
    inconsistent.mutable_settlements(3)->mutable_occupancy()->mutable_margin()->set_units(1);
    EXPECT_THROW(protocol::decode_backtest_result(inconsistent), std::invalid_argument);
    ASSERT_EQ(result.account().fills_size(), 2);
    ASSERT_EQ(result.fill_days_size(), 2);
    EXPECT_EQ(result.fill_days(0).fill_id(), result.account().fills(0).id());
    EXPECT_EQ(result.fill_days(0).trading_day(), days[3]);
    EXPECT_EQ(result.fill_days(1).trading_day(), days[4]);
    auto without_dates = result;
    without_dates.clear_fill_days();
    const auto old_view = protocol::decode_backtest_result(without_dates);
    EXPECT_FALSE(old_view.contains("fill_days"));
    EXPECT_EQ(old_view.at("account"), protocol::decode_backtest_result(result).at("account"));
    auto partial_dates = result;
    partial_dates.mutable_fill_days()->RemoveLast();
    EXPECT_THROW(protocol::decode_backtest_result(partial_dates), std::invalid_argument);
    EXPECT_EQ(result.account().fills(0).price().units(), d("100").raw());
    EXPECT_EQ(result.account().fills(1).price().units(), d("101").raw());
    EXPECT_EQ(result.account().orders(1).offset(), protocol::proto::CLOSE_YESTERDAY);
    EXPECT_EQ(result.settlements(2).contracts(0).position_quantity().units(), 0);
    EXPECT_EQ(result.settlements(3).contracts(0).position_quantity().units(), d("1").raw());
    EXPECT_EQ(result.account().fees().units(), d("16").raw());
    EXPECT_EQ(result.account().fills(0).fee().units(), d("7").raw());
    EXPECT_EQ(result.account().fills(1).fee().units(), d("9").raw());
    EXPECT_EQ(result.account().equity().units(), d("9994").raw());
    EXPECT_EQ(result.account().positions_size(), 0);
    EXPECT_EQ(result.account().frozen().units(), 0);
    // Submission must use the new day's margin, before it can match.
    cost->mutable_values()->mutable_margin_per_lot()->set_units(d("20000").raw());
    const auto refused = backtest::run(spec);
    EXPECT_EQ(refused.account().fills_size(), 0);
    EXPECT_EQ(refused.account().orders_size(), 0);
    EXPECT_EQ(refused.account().positions_size(), 0);
    EXPECT_EQ(refused.account().fees().units(), 0);
    EXPECT_EQ(refused.account().frozen().units(), 0);
    EXPECT_EQ(refused.account().margin().units(), 0);
    EXPECT_EQ(refused.account().balance().units(), spec.paper().deposit().units());
    EXPECT_EQ(refused.account().equity().units(), spec.paper().deposit().units());
    EXPECT_EQ(refused.account().available().units(), spec.paper().deposit().units());
    ASSERT_GT(refused.target_rejections_size(), 0);
    EXPECT_EQ(refused.target_rejections(0).source_index(), 3U);
    for (const auto& rejected : refused.target_rejections()) {
        EXPECT_EQ(rejected.reason(), backtest::proto::BacktestTargetRejection::INSUFFICIENT_FUNDS);
        EXPECT_EQ(rejected.target().units(), d("1").raw());
        EXPECT_FALSE(rejected.intent_id().empty());
    }
    protocol::validate_backtest_result(spec, refused);
    // Invalid terms remain input errors, distinct from a valid but unaffordable order.
    cost->mutable_values()->mutable_margin_per_lot()->set_units(d("-1").raw());
    EXPECT_THROW(backtest::run(spec), std::invalid_argument);
}

TEST(Backtest, RejectsUnsupportedDaysAndStopsCooperatively)
{
    auto spec = input();
    spec.mutable_paper()->mutable_contracts(0)->mutable_dataset()->mutable_bars(6)->set_trading_day(
        "2026-09-26");
    spec.mutable_paper()
        ->mutable_contracts(0)
        ->mutable_dataset()
        ->mutable_bars(6)
        ->set_timestamp_ns(
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
    EXPECT_THROW(
        backtest::run(
            spec,
            stop.get_token(),
            [&](auto n, auto) {
                done = n;
                stop.request_stop();
            }),
        std::runtime_error);
    EXPECT_EQ(done, 1U);
}

TEST(Backtest, ShortTargetsSellToOpenAndSettleAsNegativePositions)
{
    // The close falls below its average at the third bar; the fourth lets the sell fill.
    auto spec = input({103, 102, 101, 101, 99}, "99");
    spec.mutable_strategies(0)->set_sides(protocol::proto::LONG_AND_SHORT);
    const auto result = backtest::run(spec);
    ASSERT_EQ(result.account().fills_size(), 1);
    EXPECT_EQ(result.account().fills(0).price().units(), d("101").raw());
    // At its target the run sends nothing more.
    ASSERT_EQ(result.account().orders_size(), 1);
    EXPECT_EQ(result.account().orders(0).side(), protocol::proto::SELL);
    EXPECT_EQ(result.account().orders(0).offset(), protocol::proto::OPEN);
    ASSERT_EQ(result.account().positions_size(), 1);
    EXPECT_EQ(result.account().positions(0).side(), protocol::proto::SELL);
    ASSERT_EQ(result.settlements_size(), 1);
    EXPECT_EQ(result.settlements(0).contracts(0).position_quantity().units(), d("-1").raw());
    // One lot sold at 101 and settled at 99, ten per point, less the opening fee.
    EXPECT_EQ(result.account().equity().units(), d("10018").raw());
    // The same bars with long positions only: nothing to hold.
    spec.mutable_strategies(0)->set_sides(protocol::proto::LONG_ONLY);
    const auto flat = backtest::run(spec);
    EXPECT_EQ(flat.account().fills_size(), 0);
    EXPECT_EQ(flat.account().equity().units(), d("10000").raw());
    // No sides is not a strategy.
    spec.mutable_strategies(0)->clear_sides();
    EXPECT_THROW(backtest::run(spec), std::invalid_argument);
}

TEST(Backtest, AReversalBuysTheShortBackBeforeItBuysToOpen)
{
    // Short from the fourth bar; the close rises above its average at the seventh.
    auto spec = input({103, 102, 101, 101, 99, 98, 100, 100, 100}, "100");
    spec.mutable_strategies(0)->set_sides(protocol::proto::LONG_AND_SHORT);
    const auto result = backtest::run(spec);
    std::vector<std::pair<protocol::proto::Side, protocol::proto::Offset>> filled;
    for (const auto& order : result.account().orders())
        if (order.state() == protocol::proto::FILLED)
            filled.emplace_back(order.side(), order.offset());
    using Filled = std::vector<std::pair<protocol::proto::Side, protocol::proto::Offset>>;
    EXPECT_EQ(
        filled,
        (Filled{
            {protocol::proto::SELL, protocol::proto::OPEN},
            {protocol::proto::BUY, protocol::proto::CLOSE_TODAY},
            {protocol::proto::BUY, protocol::proto::OPEN}
        }));
    ASSERT_EQ(result.account().positions_size(), 1);
    EXPECT_EQ(result.account().positions(0).side(), protocol::proto::BUY);
    EXPECT_EQ(result.settlements(0).contracts(0).position_quantity().units(), d("1").raw());
    // Sold at 101 and bought back at 100, less an open, a close and another open.
    EXPECT_EQ(result.account().equity().units(), d("10003").raw());
}

TEST(Backtest, ARuleOtherThanAveragesRunsThroughTheSameEngine)
{
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
    EXPECT_EQ(
        protocol::decode_backtest(spec, protocol::DatasetView::metadata)
            .at("strategies")
            .at(0)
            .at("rule"),
        Json({{"kind", "momentum"}, {"lookback", 2}}));
    // With one price increment of slippage the same two orders fill a point
    // worse each: bought at 102 and sold at 100, twenty less at ten a point.
    auto slipped = spec;
    slipped.mutable_paper()->mutable_contracts(0)->set_slippage_ticks(1);
    const auto worse = backtest::run(slipped);
    ASSERT_EQ(worse.account().fills_size(), 2);
    EXPECT_EQ(worse.account().fills(0).price().units(), d("102").raw());
    EXPECT_EQ(worse.account().fills(1).price().units(), d("100").raw());
    EXPECT_EQ(worse.account().fees().units(), result.account().fees().units());
    EXPECT_EQ(worse.account().equity().units(), d("9975").raw());
    EXPECT_EQ(
        protocol::decode_backtest(slipped, protocol::DatasetView::metadata)
            .at("paper")
            .at("contracts")
            .at(0)
            .at("slippage_ticks"),
        1);
    // What a worker is told to give up is what the input fixed.
    task::proto::Task task;
    *task.mutable_input() = slipped;
    EXPECT_EQ(
        protocol::task_execution(task, std::string(64, '0'))
            .backtest()
            .contracts(0)
            .slippage_ticks(),
        1U);
    slipped.mutable_paper()->mutable_contracts(0)->set_slippage_ticks(101);
    EXPECT_THROW(backtest::validate(slipped), std::invalid_argument);
    // Seven bars cannot give a rule its first target at the eighth.
    spec.mutable_strategies(0)->mutable_momentum()->set_lookback(7);
    EXPECT_THROW(backtest::run(spec), std::invalid_argument);
}

TEST(Backtest, DailyPortfolioRecordsEachOriginalFillDayWithoutChangingBalances)
{
    const std::vector<std::string>
        days{"2026-09-21", "2026-09-22", "2026-09-23", "2026-09-24", "2026-09-25"};
    std::vector<MarketBar> bars;
    const std::vector<const char*> prices{"100", "102", "104", "100", "101"};
    for (std::size_t i = 0; i < days.size(); ++i)
        bars.push_back(
            test::flat(
                days[i],
                1'790'000'000'000'000'000LL + static_cast<std::int64_t>(i) * 86'400'000'000'000LL,
                prices[i]));
    auto single = input();
    *single.mutable_paper()->mutable_contracts(0)->mutable_dataset() =
        test::dataset(bars, {}, test::contract(), 0);
    single.set_dataset_revision(protocol::dataset_revision(single.paper()));
    const auto one = backtest::run(single);
    ASSERT_GT(one.account().fills_size(), 0);
    ASSERT_EQ(one.fill_days_size(), one.account().fills_size());
    auto pair = single;
    auto* second = pair.mutable_paper()->add_contracts();
    *second = single.paper().contracts(0);
    auto* data = second->mutable_dataset();
    *data->mutable_contract() = test::contract("SHFE", "hc2610", "hc", "2026-10");
    data->set_revision(protocol::bar_dataset_revision(*data));
    pair.set_dataset_revision(protocol::dataset_revision(pair.paper()));
    const auto two = backtest::run(pair);
    EXPECT_EQ(two.SerializeAsString(), backtest::run(pair).SerializeAsString());
    ASSERT_EQ(two.account().fills_size(), one.account().fills_size() * 2);
    ASSERT_EQ(two.fill_days_size(), two.account().fills_size());
    std::map<std::string, int> cursors;
    for (int i = 0; i < two.account().fills_size(); ++i) {
        const auto& fill = two.account().fills(i);
        ASSERT_EQ(fill.venue(), "SHFE");
        ASSERT_TRUE(
            fill.symbol() == single.paper().contracts(0).dataset().contract().symbol()
            || fill.symbol() == "hc2610");
        const auto original = cursors[fill.symbol()]++;
        ASSERT_LT(original, one.account().fills_size());
        EXPECT_EQ(two.fill_days(i).fill_id(), fill.id());
        EXPECT_EQ(two.fill_days(i).trading_day(), one.fill_days(original).trading_day());
        EXPECT_EQ(fill.price().units(), one.account().fills(original).price().units());
        EXPECT_EQ(fill.quantity().units(), one.account().fills(original).quantity().units());
        EXPECT_EQ(fill.fee().units(), one.account().fills(original).fee().units());
    }
    EXPECT_EQ(two.account().fees().units(), one.account().fees().units() * 2);
    EXPECT_EQ(two.account().realized().units(), one.account().realized().units() * 2);
    EXPECT_EQ(
        two.account().equity().units() - pair.paper().deposit().units(),
        (one.account().equity().units() - single.paper().deposit().units()) * 2);
    const auto json = protocol::decode_backtest_result(two);
    EXPECT_EQ(json.at("fill_days").size(), static_cast<std::size_t>(two.account().fills_size()));
    auto partial = two;
    partial.mutable_fill_days()->RemoveLast();
    EXPECT_THROW(protocol::decode_backtest_result(partial), std::invalid_argument);
}

TEST(Backtest, ARankingRuleTradesTheStrongestAndWeakestOfSeveralContracts)
{
    // Four contracts on the same six bars of one day, each settled at its last
    // price. A lookback of 1 ranks them at the second bar and again at the
    // fourth: first rb leads and zn trails, then the two change places. A bar
    // trades 100 lots, of which an order may take ten.
    const auto contract =
        [](const char* symbol, const char* product, std::initializer_list<const char*> prices) {
            std::vector<MarketBar> bars;
            std::int64_t time = 1790298000000000000LL;
            for (const auto* price : prices) {
                bars.push_back(test::flat("2026-09-25", time, price, "100"));
                time += 1000000000;
            }
            return test::dataset(
                bars,
                {{"2026-09-25", bars.back().close}},
                test::contract("SHFE", symbol, product, "2026-10"));
        };
    auto spec = testing_support::backtest_input(
        contract("rb2610", "rb", {"100", "110", "110", "99", "99", "99"}));
    for (const auto& dataset :
        {contract("hc2610", "hc", {"100", "102", "102", "102", "102", "102"}),
            contract("al2610", "al", {"100", "100", "100", "100", "100", "100"}),
            contract("zn2610", "zn", {"100", "90", "90", "99", "99", "99"})}) {
        auto* added = spec.mutable_paper()->add_contracts();
        *added = spec.paper().contracts(0);
        *added->mutable_dataset() = dataset;
    }
    spec.set_dataset_revision(protocol::dataset_revision(spec.paper()));
    // A lot is worth ten times its price; each held contract is sized to 2500.
    const Json rule{
        {"kind", "cross_momentum"},
        {"reverse", false},
        {"lookback", 1},
        {"rebalance", 2},
        {"count", 1},
        {"notional", "2500"},
        {"volatility", 0}
    };
    *spec.mutable_strategies(0) = protocol::encode_strategy({{"sides", "both"}, {"rule", rule}});
    const auto result = backtest::run(spec);
    // Decided at the second bar and filled at the third: 2500 is nearest two
    // lots of rb at 110 and three of zn at 90. Decided again at the fourth,
    // when both are at 99 and three lots each: closed at the fifth bar and
    // opened the other way at the sixth. hc and al never trade.
    ASSERT_EQ(result.account().fills_size(), 6);
    const auto fill_of = [](const backtest::proto::BacktestResult& run, int index) {
        const auto& value = run.account().fills(index);
        const auto order =
            std::ranges::find(run.account().orders(), value.order_id(), [](const auto& item) {
                return item.id();
            });
        return value.symbol() + (order->side() == protocol::proto::BUY ? " buy " : " sell ")
            + Decimal::from_raw(value.quantity().units()).str() + " at "
            + Decimal::from_raw(value.price().units()).str();
    };
    const auto fill = [&](int index) {
        return fill_of(result, index);
    };
    EXPECT_EQ(fill(0), "rb2610 buy 2 at 110");
    EXPECT_EQ(fill(1), "zn2610 sell 3 at 90");
    EXPECT_EQ(fill(2), "rb2610 sell 2 at 99");
    EXPECT_EQ(fill(3), "zn2610 buy 3 at 99");
    EXPECT_EQ(fill(4), "rb2610 sell 3 at 99");
    EXPECT_EQ(fill(5), "zn2610 buy 3 at 99");
    // Lost 11 a unit on two lots of rb and 9 on three of zn at a multiplier of
    // 10; eleven lots opened at 2 and five closed the same day at 3.
    EXPECT_EQ(result.account().realized().units(), d("-490").raw());
    EXPECT_EQ(result.account().fees().units(), d("37").raw());
    EXPECT_EQ(result.account().equity().units(), d("9473").raw());
    ASSERT_EQ(result.settlements(0).contracts_size(), 4);
    EXPECT_EQ(result.settlements(0).contracts(0).position_quantity().units(), d("-3").raw());
    EXPECT_EQ(result.settlements(0).contracts(1).position_quantity().units(), 0);
    EXPECT_EQ(result.settlements(0).contracts(3).position_quantity().units(), d("3").raw());
    EXPECT_EQ(
        protocol::decode_backtest(spec, protocol::DatasetView::metadata).at("strategies").at(0),
        (Json{{"sides", "both"}, {"rule", rule}}));
    // Reversed, the same ranking holds the weakest long and the strongest
    // short: rb is sold and zn bought.
    auto reversed = spec;
    reversed.mutable_strategies(0)->mutable_cross()->set_reversed(true);
    const auto mirrored = backtest::run(reversed);
    ASSERT_EQ(mirrored.account().fills_size(), 6);
    EXPECT_EQ(fill_of(mirrored, 0), "rb2610 sell 2 at 110");
    EXPECT_EQ(fill_of(mirrored, 1), "zn2610 buy 3 at 90");
    // What the first lost the second gains, less the same fees.
    EXPECT_EQ(mirrored.account().realized().units(), d("490").raw());
    EXPECT_EQ(mirrored.account().equity().units(), d("10453").raw());
    // A contract whose one lot is worth more than twice the notional is ranked
    // and not held: 400 against lots of about 1000.
    auto small = spec;
    small.mutable_strategies(0)->mutable_cross()->mutable_notional()->set_units(d("400").raw());
    EXPECT_EQ(backtest::run(small).account().fills_size(), 0);
    // One contract a side of four leaves two flat; two a side needs them all,
    // and three a side has no one to hold.
    spec.mutable_strategies(0)->mutable_cross()->set_count(2);
    EXPECT_NO_THROW(backtest::validate(spec));
    spec.mutable_strategies(0)->mutable_cross()->set_count(3);
    EXPECT_THROW(backtest::validate(spec), std::invalid_argument);
}

TEST(Backtest, ARankingRuleSizedByVolatilityHoldsLessOfWhatMovesMore)
{
    // rb moves by a tenth a bar where hc moves by a hundredth, at a hundredth of
    // its price. A bar trades 1000 lots, of which an order may take a hundred.
    const auto contract =
        [](const char* symbol, const char* product, std::initializer_list<const char*> prices) {
            std::vector<MarketBar> bars;
            std::int64_t time = 1790298000000000000LL;
            for (const auto* price : prices) {
                bars.push_back(test::flat("2026-09-25", time, price, "1000"));
                time += 1000000000;
            }
            return test::dataset(
                bars,
                {{"2026-09-25", bars.back().close}},
                test::contract("SHFE", symbol, product, "2026-10"));
        };
    auto spec =
        testing_support::backtest_input(contract("rb2610", "rb", {"100", "110", "99", "99", "99"}));
    auto* added = spec.mutable_paper()->add_contracts();
    *added = spec.paper().contracts(0);
    *added->mutable_dataset() =
        contract("hc2610", "hc", {"10000", "10100", "9999", "9999", "9999"});
    spec.set_dataset_revision(protocol::dataset_revision(spec.paper()));
    // Ranked once, at the third bar: the window of two returns needs it.
    const Json rule{
        {"kind", "cross_momentum"},
        {"reverse", false},
        {"lookback", 1},
        {"rebalance", 10},
        {"count", 1},
        {"notional", "110000"},
        {"volatility", 2}
    };
    *spec.mutable_strategies(0) = protocol::encode_strategy({{"sides", "both"}, {"rule", rule}});
    const auto result = backtest::run(spec);
    // hc fell less and is held long, rb short. Of the 220000 the two hold
    // together rb takes one eleventh, 20000 against lots of 990, and hc the
    // rest, 200000 against lots of 99990.
    ASSERT_EQ(result.account().fills_size(), 2);
    const auto fill = [&](int index) {
        const auto& value = result.account().fills(index);
        return value.symbol() + " " + Decimal::from_raw(value.quantity().units()).str() + " at "
            + Decimal::from_raw(value.price().units()).str();
    };
    EXPECT_EQ(fill(0), "rb2610 20 at 99");
    EXPECT_EQ(fill(1), "hc2610 2 at 9999");
    // Nothing moved after the fills: only the 22 lots' opening fees are gone.
    EXPECT_EQ(result.account().equity().units(), d("9956").raw());
    // Split alike, the same notional asks for a hundred lots of rb and one of
    // hc at the second bar: more than the account may hold, and refused.
    spec.mutable_strategies(0)->mutable_cross()->set_volatility(0);
    const auto constrained = backtest::run(spec);
    ASSERT_EQ(constrained.account().fills_size(), 1);
    EXPECT_EQ(constrained.account().fills(0).symbol(), "hc2610");
    EXPECT_EQ(constrained.account().fills(0).quantity().units(), d("1").raw());
    EXPECT_EQ(constrained.account().fills(0).price().units(), d("9999").raw());
    ASSERT_EQ(constrained.account().positions_size(), 1);
    EXPECT_EQ(constrained.account().positions(0).symbol(), "hc2610");
    EXPECT_EQ(constrained.account().fees().units(), d("2").raw());
    EXPECT_EQ(constrained.account().balance().units(), d("9998").raw());
    EXPECT_EQ(constrained.account().equity().units(), d("9998").raw());
    EXPECT_EQ(constrained.account().margin().units(), d("100").raw());
    EXPECT_EQ(constrained.account().frozen().units(), 0);
    EXPECT_EQ(constrained.account().available().units(), d("9898").raw());
    ASSERT_GT(constrained.target_rejections_size(), 0);
    for (const auto& rejected : constrained.target_rejections()) {
        EXPECT_EQ(rejected.reason(), backtest::proto::BacktestTargetRejection::INSUFFICIENT_FUNDS);
        EXPECT_EQ(rejected.target().units(), d("100").raw());
        EXPECT_FALSE(rejected.intent_id().empty());
    }
    protocol::validate_backtest_result(spec, constrained);
    spec.mutable_paper()->mutable_risk()->set_max_working_orders(0);
    EXPECT_THROW(backtest::run(spec), std::invalid_argument);
}

TEST(Backtest, SeveralStrategiesAreComparedOnTheDaysBeforeTheHoldoutAlone)
{
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
        days.push_back(
            {std::chrono::sys_days(parse_trading_date(single.settlements(i).trading_day())),
                Decimal::from_raw(single.settlements(i).equity().units())});
    std::vector<Decimal> marks;
    for (int i = 0; i < 50; ++i) // a mark and a settlement for each day
        marks.push_back(Decimal::from_raw(single.equity(i).equity().units()));
    PerformanceContext context;
    context.begin_ns = single.equity(0).timestamp_ns();
    context.end_ns = single.settlements(24).timestamp_ns();
    context.annual_periods = 252;
    context.explicitly_zero_risk_free = true;
    const auto record = performance(d("10000"), days, marks, context);
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
        EXPECT_EQ(
            other.candidates(i).SerializeAsString(),
            result.candidates(i).SerializeAsString());
    EXPECT_NE(other.account().equity().units(), result.account().equity().units());
}

TEST(Backtest, ARollingComparisonFollowsEachRoundsBestStrategyOverTheDaysAfterIt)
{
    const auto spec = rolling_comparison();
    ASSERT_TRUE(spec.has_walk_forward());
    std::size_t reported = 0, units = 0;
    const auto result = backtest::run(spec, {}, [&](auto done, auto total) {
        reported = done;
        units = total;
    });
    // Each strategy alone over every bar, and the account once more.
    EXPECT_EQ(units, 3U * 60);
    EXPECT_EQ(reported, units);
    EXPECT_EQ(protocol::backtest_work_units(spec), units);
    ASSERT_EQ(result.folds_size(), 4);
    EXPECT_EQ(result.candidates_size(), 0);
    // Each strategy as it trades alone, to compare the rounds with.
    std::vector<backtest::proto::BacktestResult> alone;
    for (const auto& strategy : spec.strategies()) {
        auto single = spec;
        single.clear_walk_forward();
        single.clear_strategies();
        *single.add_strategies() = strategy;
        alone.push_back(backtest::run(single));
    }
    const auto position = [](const backtest::proto::BacktestResult& run, int day) {
        return run.settlements(day).contracts(0).position_quantity().units();
    };
    // Nothing is held before the first round has anything to follow.
    for (int day = 0; day < 20; ++day) {
        EXPECT_EQ(position(result, day), 0) << day;
        EXPECT_EQ(result.settlements(day).equity().units(), d("10000").raw()) << day;
    }
    for (int round = 0; round < 4; ++round) {
        const auto& fold = result.folds(round);
        const int first = 20 + 10 * round;
        EXPECT_EQ(fold.first_day(), result.settlements(first).trading_day());
        EXPECT_EQ(fold.training_first_day(), result.settlements(first - 20).trading_day());
        EXPECT_EQ(fold.training_last_day(), result.settlements(first - 1).trading_day());
        EXPECT_EQ(fold.last_day(), result.settlements(first + 9).trading_day());
        EXPECT_EQ(fold.training_days(), 20U);
        EXPECT_EQ(fold.validation_days(), 10U);
        ASSERT_EQ(fold.candidates_size(), 2);
        // What a round records of a strategy is what its twenty training days
        // alone show of that strategy trading alone.
        std::optional<unsigned> best;
        for (int k = 0; k < 2; ++k) {
            std::vector<EquityDay> days;
            for (int day = first - 20; day < first; ++day)
                days.push_back(
                    {std::chrono::sys_days(
                         parse_trading_date(alone[k].settlements(day).trading_day())),
                        Decimal::from_raw(alone[k].settlements(day).equity().units())});
            std::vector<Decimal> marks; // a mark and a settlement for each day
            for (int i = 2 * (first - 20); i < 2 * first; ++i)
                marks.push_back(Decimal::from_raw(alone[k].equity(i).equity().units()));
            const auto base = first == 20
                ? d("10000")
                : Decimal::from_raw(alone[k].settlements(first - 21).equity().units());
            PerformanceContext context;
            context.begin_ns = first == 20 ? alone[k].equity(0).timestamp_ns()
                                           : alone[k].settlements(first - 21).timestamp_ns();
            context.end_ns = alone[k].settlements(first - 1).timestamp_ns();
            context.opening_is_full_period = first > 20;
            context.annual_periods = 252;
            context.explicitly_zero_risk_free = true;
            const auto record = performance(base, days, marks, context);
            EXPECT_DOUBLE_EQ(fold.candidates(k).total_return(), record.total_return) << round << k;
            ASSERT_EQ(fold.candidates(k).has_sharpe(), record.sharpe.has_value()) << round << k;
            if (record.sharpe) {
                EXPECT_DOUBLE_EQ(fold.candidates(k).sharpe(), *record.sharpe) << round << k;
                if (!best || *record.sharpe > fold.candidates(static_cast<int>(*best)).sharpe())
                    best = static_cast<unsigned>(k);
            }
        }
        ASSERT_TRUE(best);
        ASSERT_TRUE(fold.has_selected());
        EXPECT_EQ(fold.selected(), *best);
        // Through its validation days the account holds what that strategy holds:
        // the target it had the evening before fills on the first of them.
        for (int day = first; day < first + 10; ++day)
            EXPECT_EQ(position(result, day), position(alone[fold.selected()], day)) << day;
    }
    // The rising days favour the long average and the falling ones the short.
    EXPECT_EQ(result.folds(0).selected(), 1U);
    EXPECT_EQ(result.folds(3).selected(), 0U);

    // A round reads nothing from its validation days on: other prices from the
    // last round's first day leave every round as it was.
    auto changed = spec;
    auto* dataset = changed.mutable_paper()->mutable_contracts(0)->mutable_dataset();
    for (int i = 50; i < 60; ++i) {
        auto* bar = dataset->mutable_bars(i);
        const auto price = d(std::to_string(300 + i).c_str()).raw();
        for (auto* field :
            {bar->mutable_open(), bar->mutable_high(), bar->mutable_low(), bar->mutable_close()})
            field->set_units(price);
        dataset->mutable_days(i)->mutable_settlement_price()->set_units(price);
    }
    dataset->set_revision(protocol::bar_dataset_revision(*dataset));
    changed.set_dataset_revision(protocol::dataset_revision(changed.paper()));
    const auto other = backtest::run(changed);
    for (int round = 0; round < 4; ++round)
        EXPECT_EQ(other.folds(round).SerializeAsString(), result.folds(round).SerializeAsString());
    EXPECT_NE(other.account().equity().units(), result.account().equity().units());
}

TEST(Backtest, ARollingComparisonNeedsTwentyTrainingDaysAndDaysAfterThem)
{
    const auto spec = rolling_comparison();
    EXPECT_NO_THROW(backtest::validate(spec));
    const auto refused = [&](unsigned training, unsigned validation) {
        auto invalid = spec;
        invalid.mutable_walk_forward()->set_training_days(training);
        invalid.mutable_walk_forward()->set_validation_days(validation);
        EXPECT_THROW(backtest::validate(invalid), std::invalid_argument)
            << training << " " << validation;
    };
    refused(19, 10); // too few days for a yearly ratio
    refused(20, 0);
    refused(60, 10); // no day left to trade
    // The last round takes what remains: 59 training days leave one.
    auto last = spec;
    last.mutable_walk_forward()->set_training_days(59);
    EXPECT_NO_THROW(backtest::validate(last));
    EXPECT_EQ(protocol::backtest_folds(last).size(), 1U);
    auto uneven = spec;
    uneven.mutable_walk_forward()->set_validation_days(7);
    const auto rounds = protocol::backtest_folds(uneven);
    ASSERT_EQ(rounds.size(), 6U); // 20-26, 27-33, 34-40, 41-47, 48-54, 55-59
    EXPECT_EQ(rounds.back().first, 55U);
    EXPECT_EQ(rounds.back().end, 60U);
    const auto partial = backtest::run(uneven);
    EXPECT_EQ(partial.folds(5).validation_days(), 5U);
    EXPECT_EQ(partial.folds(5).training_days(), 20U);
    EXPECT_EQ(partial.folds(5).last_day(), partial.settlements(59).trading_day());
    EXPECT_EQ(partial.folds(5).training_first_day(), partial.settlements(35).trading_day());
    EXPECT_EQ(partial.folds(5).training_last_day(), partial.settlements(54).trading_day());
    // One way of comparing at a time, and only several strategies are compared.
    auto both = spec;
    both.set_holdout_day("2026-10-20");
    EXPECT_THROW(backtest::validate(both), std::invalid_argument);
    auto single = spec;
    single.mutable_strategies()->RemoveLast();
    EXPECT_THROW(backtest::validate(single), std::invalid_argument);
    auto requested = spec;
    requested.clear_walk_forward();
    EXPECT_THROW(compare(requested, "2026-10-20", 20, 10), std::invalid_argument);
    // What a worker is told, and what a client reads back.
    task::proto::Task task;
    *task.mutable_input() = spec;
    EXPECT_EQ(
        protocol::task_execution(task, std::string(64, '0'))
            .backtest()
            .walk_forward()
            .SerializeAsString(),
        spec.walk_forward().SerializeAsString());
    EXPECT_EQ(
        protocol::decode_backtest(spec, protocol::DatasetView::metadata).at("walk_forward"),
        (Json{{"training_days", 20}, {"validation_days", 10}}));
}

TEST(Backtest, AComparisonNeedsDifferentStrategiesAndTwentyDaysBeforeItsHoldout)
{
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
    compare(dated, "2026-10-19");
    EXPECT_EQ(dated.holdout_day(), "2026-10-19");
    EXPECT_THROW(compare(dated, "2026-12-01"), std::invalid_argument);
    EXPECT_THROW(compare(dated, "soon"), std::invalid_argument);
    dated.mutable_strategies()->RemoveLast();
    EXPECT_THROW(compare(dated, "2026-10-19"), std::invalid_argument);
    // Rules that take their first side only after the development days leave
    // the account untouched there: nothing varies, so there is nothing to
    // choose by.
    auto idle = spec;
    for (int i = 0; i < 2; ++i)
        *idle.mutable_strategies(i) = protocol::encode_strategy(
            {{"quantity", "1"},
                {"sides", "both"},
                {"rule", {{"kind", "momentum"}, {"lookback", 28 + i}}}});
    EXPECT_THROW(backtest::run(idle), std::invalid_argument);
}

TEST(Backtest, InputDecodingRejectsCorruptOrIncompleteInputs)
{
    const auto spec = input();
    const auto json = protocol::decode_backtest(spec);
    auto metadata = json;
    for (auto& contract : metadata.at("paper").at("contracts")) {
        contract.at("dataset").erase("bars");
        contract.at("dataset").erase("days");
    }
    EXPECT_EQ(protocol::decode_backtest(spec, protocol::DatasetView::metadata), metadata);
    auto corrupt = spec;
    corrupt.mutable_paper()
        ->mutable_contracts(0)
        ->mutable_dataset()
        ->mutable_bars(0)
        ->clear_close();
    EXPECT_THROW(
        protocol::decode_backtest(corrupt, protocol::DatasetView::metadata),
        std::invalid_argument);
    auto missing = spec;
    missing.mutable_strategies(0)->clear_quantity();
    EXPECT_THROW(protocol::decode_backtest(missing), std::invalid_argument);
}

namespace {

struct TaskDirectory
{
    std::filesystem::path path = std::filesystem::temp_directory_path()
        / ("asterion-backtest-factor-" + unique_process_id());

    TaskDirectory()
    {
        std::filesystem::create_directory(path);
    }

    ~TaskDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

} // namespace

TEST(TaskMetadata, DescriptionAndDerivationSurviveRestartWithoutChangingCalculationEvidence)
{
    TaskDirectory directory;
    const auto spec = input();
    const auto expected = backtest::run(spec);
    std::string input_hash, result_hash, digest;
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        tasks::submit(store, "original", spec);
        const auto token = store.commit(store.claim("original")).token();
        store.commit(tasks::finish(store, "original", token, expected));
        EXPECT_FALSE(store.describe("original").has_metadata());
        input_hash = sha256_file(directory.path / "original/input.pb");
        result_hash = sha256_file(directory.path / "original/results/1.pb");
        digest = store.describe("original").result_digest();
        auto submission = task_submission(store, "derived", spec);
        task::proto::TaskDerivation source;
        source.set_source_id("original");
        source.set_source_result_digest(digest);
        source.set_data_choice("fixed");
        submission.set_derivation(source);
        store.admit_submission(submission);
        submission.prepare_files();
        store.commit(store.register_submission(submission));
        task::proto::TaskMetadataEdit edit;
        edit.set_id("original");
        edit.set_name("baseline");
        edit.set_note("first line\nsecond line");
        const auto before = store.describe("original");
        const auto changed = store.commit(store.edit_metadata(edit)).task();
        EXPECT_EQ(changed.metadata().revision(), 1U);
        EXPECT_EQ(changed.state(), before.state());
        EXPECT_EQ(changed.attempt(), before.attempt());
        EXPECT_EQ(changed.updated_at_ms(), before.updated_at_ms());
        EXPECT_EQ(changed.result_digest(), digest);
        EXPECT_THROW(store.edit_metadata(edit), Error);
        const auto derived_token = store.commit(store.claim("derived")).token();
        edit.set_id("derived");
        edit.set_name("candidate");
        edit.set_note("fixed evidence");
        store.commit(store.edit_metadata(edit));
        EXPECT_EQ(store.describe("derived").metadata().derivation().source_id(), "original");
        EXPECT_EQ(store.describe("derived").metadata().derivation().source_instance(), "task");
        edit.set_expected_revision(1);
        edit.set_note("must not be committed");
        auto change = store.edit_metadata(edit);
        sqlite::fail_next_commits_for_testing(1);
        EXPECT_THROW(change.persist(), std::runtime_error);
        sqlite::fail_next_commits_for_testing(0);
        EXPECT_EQ(store.describe("derived").metadata().note(), "fixed evidence");
        store.commit(store.progress("derived", derived_token, 1));
        store.commit(tasks::finish(store, "derived", derived_token, expected));
    }
    {
        tasks::Store restored(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        const auto original = restored.describe("original"), derived = restored.describe("derived");
        EXPECT_EQ(original.metadata().name(), "baseline");
        EXPECT_EQ(original.metadata().note(), "first line\nsecond line");
        EXPECT_EQ(derived.metadata().name(), "candidate");
        EXPECT_EQ(derived.metadata().revision(), 1U);
        EXPECT_EQ(derived.metadata().derivation().source_result_digest(), digest);
        EXPECT_EQ(derived.metadata().derivation().data_choice(), "fixed");
        EXPECT_EQ(
            tasks::result(restored, "original").SerializeAsString(),
            expected.SerializeAsString());
        EXPECT_EQ(sha256_file(directory.path / "original/input.pb"), input_hash);
        EXPECT_EQ(sha256_file(directory.path / "original/results/1.pb"), result_hash);
    }
}

TEST(TaskEventSegments, FailedIndexCommitKeepsActiveEvidenceAndConfirmsOnlyTheOriginalSegment)
{
    TaskDirectory directory;
    const auto root = std::filesystem::canonical(directory.path);
    const auto segments = root / "event-segments";
    {
        sqlite::Database database(root / "events.sqlite");
        database.execute("CREATE TABLE task_events(task_id TEXT, sequence INTEGER, body TEXT)");
        tasks::TaskEvents log(database, segments, false);
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
    tasks::TaskEvents restored(database, segments, true);
    unsigned count = 0;
    restored.read([&](const auto&, auto sequence, const auto&) {
        EXPECT_EQ(sequence, ++count);
    });
    EXPECT_EQ(count, 1001);
    const auto database_hash = sha256_file(root / "events.sqlite");
    replace_file_durably(segments / "1.pb", "corrupted fixture");
    EXPECT_THROW(restored.read([](const auto&, auto, const auto&) {}), std::invalid_argument);
    EXPECT_EQ(sha256_file(root / "events.sqlite"), database_hash);
}

TEST(TaskStore, SegmentedEventsKeepAttemptFencingAndOwnedResultsAcrossRecovery)
{
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
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        tasks::submit(store, "long-run", spec);
        input_hash = sha256_file(directory.path / "long-run/input.pb");
        token = store.commit(store.claim("long-run")).token();
        for (unsigned completed = 1; completed <= 1001; ++completed)
            store.commit(store.progress("long-run", token, completed));
        store.commit(tasks::finish(store, "long-run", token, expected));
        EXPECT_EQ(store.describe("long-run").state(), task::proto::SUCCEEDED);
        result_hash = sha256_file(directory.path / "long-run/results/1.pb");
        segment_hash = sha256_file(directory.path / "event-segments/1.pb");
    }
    {
        tasks::Store restored(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        EXPECT_EQ(restored.describe("long-run").submission_sequence(), 1);
        EXPECT_EQ(restored.describe("long-run").attempt(), 1);
        EXPECT_EQ(
            tasks::result(restored, "long-run").SerializeAsString(),
            expected.SerializeAsString());
        EXPECT_THROW(
            restored.commit(restored.progress("long-run", token, 1002)),
            std::invalid_argument);
        EXPECT_EQ(sha256_file(directory.path / "long-run/input.pb"), input_hash);
        EXPECT_EQ(sha256_file(directory.path / "long-run/results/1.pb"), result_hash);
        EXPECT_EQ(sha256_file(directory.path / "event-segments/1.pb"), segment_hash);
    }
    {
        sqlite::Database database(directory.path / "tasks.sqlite");
        database.execute(
            "DELETE FROM task_events WHERE task_id='long-run' AND sequence=" "(SELECT MAX(sequence) FROM task_events WHERE task_id='long-run')");
    }
    const auto database_hash = sha256_file(directory.path / "tasks.sqlite");
    EXPECT_THROW(
        (tasks::Store(asterion::research::task_domain(), directory.path, tasks::Identity{"task"})),
        std::invalid_argument);
    EXPECT_EQ(sha256_file(directory.path / "tasks.sqlite"), database_hash);
}

TEST(TaskStore, HistoryPageCursorDoesNotHideQueuedWorkOrShiftAfterNewSubmissions)
{
    TaskDirectory directory;
    tasks::Store store(asterion::research::task_domain(), directory.path, tasks::Identity{"task"});
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
    std::set<std::string> allowance;

    EXPECT_THROW(store.list(0), std::invalid_argument);
    EXPECT_THROW(store.list(201), std::invalid_argument);
}

TEST(TaskStore, DuplicateSubmissionAndStaleAttemptsAreFenced)
{
    TaskDirectory directory;
    tasks::Store store(asterion::research::task_domain(), directory.path, tasks::Identity{"task"});
    auto spec = input();
    const auto first = tasks::submit(store, "job1", spec);
    EXPECT_EQ(tasks::submit(store, "job1", spec).SerializeAsString(), first.SerializeAsString());
    spec.mutable_strategies(0)->mutable_quantity()->set_units(d("2").raw());
    EXPECT_THROW(tasks::submit(store, "job1", spec), std::invalid_argument);
    const auto old = store.commit(store.claim("job1")).token();
    store.commit(store.progress("job1", old, 2));
    EXPECT_THROW(store.commit(store.progress("job1", old, 1)), std::invalid_argument);
    EXPECT_EQ(store.commit(store.cancel("job1")).task().state(), task::proto::CANCEL_REQUESTED);
    store.commit(tasks::finish(store, "job1", old, backtest::run(input())));
    EXPECT_EQ(store.get("job1").state(), task::proto::CANCELLED);
    EXPECT_THROW(tasks::result(store, "job1"), std::invalid_argument);
    store.commit(store.retry("job1")).task();
    const auto next = store.commit(store.claim("job1")).token();
    EXPECT_NE(old, next);
    EXPECT_THROW(
        store.commit(tasks::finish(store, "job1", old, backtest::run(input()))),
        std::invalid_argument);
    store.commit(tasks::finish(store, "job1", next, backtest::run(input())));
    EXPECT_EQ(store.get("job1").attempt(), 2U);
    EXPECT_EQ(store.get("job1").state(), task::proto::SUCCEEDED);
    EXPECT_EQ(
        tasks::result(store, "job1").SerializeAsString(),
        backtest::run(input()).SerializeAsString());
    EXPECT_THROW(store.commit(store.retry("job1")).task(), std::invalid_argument);
}

TEST(TaskStore, PreparationFailureRequiresExplicitRetryAndCannotOverwriteCompletedResults)
{
    TaskDirectory directory;
    tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
    tasks::submit(store, "queued", input());
    store.commit(store.fail_start("queued", 1, "input preparation failed", "operation_failed"));
    EXPECT_EQ(store.get("queued").state(), task::proto::FAILED);
    EXPECT_EQ(store.get("queued").attempt(), 1);
    store.commit(store.retry("queued"));
    store.commit(store.fail_start("queued", 1, "late exit", "operation_failed"));
    EXPECT_EQ(store.get("queued").state(), task::proto::QUEUED);
    auto token = store.commit(store.claim("queued")).token();
    store.commit(store.fail_start("queued", 1, "old exit", "operation_failed"));
    EXPECT_EQ(store.get("queued").state(), task::proto::RUNNING);
    store.commit(tasks::finish(store, "queued", token, backtest::run(input())));
    store.commit(store.fail_start("queued", 3, "late preparation failure", "operation_failed"));
    EXPECT_EQ(store.get("queued").state(), task::proto::SUCCEEDED);
}

TEST(TaskStore, RestartRetainsQueueAndResultsButInterruptsUnconfirmedWork)
{
    TaskDirectory directory;
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        tasks::submit(store, "queued", input());
        tasks::submit(store, "working", input());
        tasks::submit(store, "done", input());
        const auto token = store.commit(store.claim("working")).token();
        store.commit(store.progress("working", token, 2));
        store.commit(
            tasks::finish(
                store,
                "done",
                store.commit(store.claim("done")).token(),
                backtest::run(input())));
        EXPECT_THROW(
            tasks::Store other(
                asterion::research::task_domain(),
                directory.path,
                tasks::Identity{"task"}),
            std::runtime_error);
    }
    {
        tasks::Store restored(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        EXPECT_EQ(restored.get("queued").state(), task::proto::QUEUED);
        EXPECT_EQ(restored.get("working").state(), task::proto::INTERRUPTED);
        EXPECT_EQ(restored.get("working").completed(), 2U);
        EXPECT_EQ(restored.get("done").state(), task::proto::SUCCEEDED);
        EXPECT_EQ(
            tasks::result(restored, "done").SerializeAsString(),
            backtest::run(input()).SerializeAsString());
        restored.commit(restored.retry("working")).task();
        EXPECT_EQ(restored.get("working").state(), task::proto::QUEUED);
    }
    tasks::Store again(asterion::research::task_domain(), directory.path, tasks::Identity{"task"});
    EXPECT_EQ(again.get("working").state(), task::proto::QUEUED);
}

TEST(TaskStore, InstanceBindingRejectsWrongOwnerBeforeRecovery)
{
    TaskDirectory directory;
    const tasks::Identity identity{"task-one"};
    {
        tasks::Store store(asterion::research::task_domain(), directory.path, identity);
        tasks::submit(store, "working", input());
        (void)store.commit(store.claim("working")).token();
    }
    const auto database = directory.path / "tasks.sqlite";
    const auto seal = directory.path / "instance.json";
    const auto database_hash = sha256_file(database);
    const auto seal_hash = sha256_file(seal);
    for (const tasks::Identity wrong : {tasks::Identity{"task-two"}, tasks::Identity{"data-one"}}) {
        EXPECT_THROW(
            tasks::Store rejected(asterion::research::task_domain(), directory.path, wrong),
            std::invalid_argument);
        EXPECT_THROW(
            tasks::Store::inspect_history_usage(
                asterion::research::task_domain(),
                directory.path,
                wrong,
                std::string(64, 'a')),
            std::invalid_argument);
        EXPECT_EQ(sha256_file(database), database_hash);
        EXPECT_EQ(sha256_file(seal), seal_hash);
    }
    tasks::Store restored(asterion::research::task_domain(), directory.path, identity);
    EXPECT_EQ(restored.get("working").state(), task::proto::INTERRUPTED);
    EXPECT_EQ(sha256_file(seal), seal_hash);
}

TEST(TaskStore, MissingOrUnsupportedIdentityIsNeverFilledIntoExistingStore)
{
    for (const bool missing : {true, false}) {
        SCOPED_TRACE(missing);
        TaskDirectory directory;
        const tasks::Identity identity{"task-one"};
        {
            tasks::Store store(asterion::research::task_domain(), directory.path, identity);
            tasks::submit(store, "working", input());
            (void)store.commit(store.claim("working")).token();
        }
        const auto seal = directory.path / "instance.json";
        if (missing)
            std::filesystem::remove(seal);
        else
            replace_file_durably(seal, Json{{"task_instance", "different-task"}}.dump());
        const auto database_hash = sha256_file(directory.path / "tasks.sqlite");
        const auto seal_hash = missing ? std::string{} : sha256_file(seal);
        EXPECT_THROW(
            tasks::Store rejected(asterion::research::task_domain(), directory.path, identity),
            std::invalid_argument);
        EXPECT_THROW(
            tasks::Store::inspect_history_usage(
                asterion::research::task_domain(),
                directory.path,
                identity,
                std::string(64, 'a')),
            std::invalid_argument);
        EXPECT_EQ(sha256_file(directory.path / "tasks.sqlite"), database_hash);
        EXPECT_EQ(std::filesystem::exists(seal), !missing);
        if (!missing)
            EXPECT_EQ(sha256_file(seal), seal_hash);
    }
}

TEST(TaskStore, DirectorySyncFailureCannotCommitTaskSubmissionOrSuccessfulResult)
{
    {
        TaskDirectory directory;
        const tasks::Identity identity{"task-one"};
        fail_next_directory_syncs_for_testing(1);
        EXPECT_THROW(
            tasks::Store store(asterion::research::task_domain(), directory.path, identity),
            std::runtime_error);
        fail_next_directory_syncs_for_testing(0);
        EXPECT_FALSE(std::filesystem::exists(directory.path / "tasks.sqlite"));
        tasks::Store restored(asterion::research::task_domain(), directory.path, identity);
        EXPECT_TRUE(restored.list().tasks().empty());
    }
    {
        TaskDirectory directory;
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
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
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        tasks::submit(store, "job", input());
        const auto token = store.commit(store.claim("job")).token();
        const auto result = backtest::run(input());
        fail_next_directory_syncs_for_testing(1);
        EXPECT_THROW(store.commit(tasks::finish(store, "job", token, result)), std::runtime_error);
        fail_next_directory_syncs_for_testing(0);
        EXPECT_EQ(store.get("job").state(), task::proto::RUNNING);
        EXPECT_THROW(tasks::result(store, "job"), std::invalid_argument);
    }
    tasks::Store restored(
        asterion::research::task_domain(),
        directory.path,
        tasks::Identity{"task"});
    EXPECT_EQ(restored.get("job").state(), task::proto::INTERRUPTED);
    EXPECT_TRUE(std::filesystem::is_regular_file(directory.path / "job/results/1.pb"));
    restored.commit(restored.retry("job")).task();
    restored.commit(
        tasks::finish(
            restored,
            "job",
            restored.commit(restored.claim("job")).token(),
            backtest::run(input())));
    EXPECT_EQ(restored.get("job").state(), task::proto::SUCCEEDED);
}

TEST(TaskStore, SubmissionPreparationLeavesConfirmedStateAvailable)
{
    TaskDirectory directory;
    tasks::Store store(asterion::research::task_domain(), directory.path, tasks::Identity{"task"});
    tasks::submit(store, "existing", input());
    auto pending = task_submission(store, "new", input());
    store.admit_submission(pending);
    EXPECT_TRUE(store.has_submission());
    EXPECT_FALSE(std::filesystem::exists(directory.path / "new"));
    EXPECT_EQ(store.list().capacity().active_reserved(), 1U);
    EXPECT_EQ(store.list().capacity().active_used(), 1U);
    EXPECT_EQ(store.list().capacity().retained_tasks(), 1U);
    EXPECT_EQ(store.list().capacity().uncommitted(), 0U);
    auto prepared = std::async(std::launch::async, [&] {
        pending.prepare_files();
    });
    store.commit(store.cancel("existing"));
    prepared.get();
    EXPECT_TRUE(store.active_tasks().empty());
    auto registered = store.register_submission(pending);
    auto persisted = std::async(std::launch::async, [&] {
        registered.persist();
    });
    persisted.get();
    EXPECT_TRUE(store.active_tasks().empty());
    EXPECT_TRUE(store.has_submission());
    store.confirm(registered);
    EXPECT_FALSE(store.has_submission());
    EXPECT_EQ(store.describe("new").submission_sequence(), 2U);
    EXPECT_EQ(store.list().capacity().active_reserved(), 0U);
    EXPECT_EQ(store.list().capacity().active_used(), 1U);

    // Duplicate input verification cannot restore an obsolete state snapshot.
    auto duplicate = task_submission(store, "new", input());
    store.admit_submission(duplicate);
    auto verified = std::async(std::launch::async, [&] {
        duplicate.prepare_files();
    });
    store.commit(store.cancel("new"));
    verified.get();
    EXPECT_EQ(
        store.commit(store.register_submission(duplicate)).task().state(),
        task::proto::CANCELLED);
    EXPECT_TRUE(store.active_tasks().empty());
}

TEST(TaskStore, JournalWaitKeepsActiveStateAvailableAndRequiresOwnerConfirmation)
{
    using namespace std::chrono_literals;
    TaskDirectory directory;
    tasks::Store store(asterion::research::task_domain(), directory.path, tasks::Identity{"task"});
    tasks::submit(store, "job", input());
    tasks::submit(store, "other", input());
    auto claimed = store.claim("job");
    sqlite::hold_commits_for_testing(true);
    auto persisted = std::async(std::launch::async, [&] {
        claimed.persist();
    });

    struct Release
    {
        ~Release()
        {
            sqlite::hold_commits_for_testing(false);
        }
    } release;

    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!sqlite::commit_waiting_for_testing() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    ASSERT_TRUE(sqlite::commit_waiting_for_testing());
    EXPECT_EQ(store.describe("job").state(), task::proto::QUEUED);
    EXPECT_EQ(store.active_tasks().size(), 2U);
    // Another task can be decided while the writer is held at its durable barrier.
    auto cancelled = store.cancel("other");
    EXPECT_EQ(cancelled.task().state(), task::proto::CANCELLED);
    EXPECT_EQ(store.describe("other").state(), task::proto::QUEUED);
    sqlite::hold_commits_for_testing(false);
    persisted.get();
    EXPECT_EQ(store.describe("job").state(), task::proto::QUEUED);
    store.confirm(claimed);
    EXPECT_EQ(store.describe("job").state(), task::proto::RUNNING);
    store.commit(std::move(cancelled));
    ASSERT_EQ(store.active_tasks().size(), 1U);
    EXPECT_EQ(store.active_tasks().front().id(), "job");
}

TEST(TaskStore, FailedStateCommitKeepsTheLastConfirmedActiveState)
{
    TaskDirectory directory;
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
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
    tasks::Store restored(
        asterion::research::task_domain(),
        directory.path,
        tasks::Identity{"task"});
    EXPECT_EQ(restored.describe("job").state(), task::proto::INTERRUPTED);
    EXPECT_EQ(restored.describe("job").completed(), 1U);
    EXPECT_TRUE(restored.active_tasks().empty());
    restored.commit(restored.retry("job")).task();
    EXPECT_EQ(restored.active_tasks().size(), 1U);
}

TEST(TaskStore, PreparedPayloadCannotBypassCancellationOrANewerAttempt)
{
    for (const bool retry : {false, true}) {
        SCOPED_TRACE(retry);
        TaskDirectory directory;
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        tasks::submit(store, "job", input());
        task::proto::TaskFinish request;
        request.set_id("job");
        request.set_token(store.commit(store.claim("job")).token());
        *request.mutable_result() = backtest::run(input());
        auto pending = store.prepare_finish(request);
        EXPECT_THROW(store.commit(store.finish(pending)), std::invalid_argument);
        // File preparation owns only copies. The state owner can process progress,
        // cancellation and a new attempt while the old attempt writes its payload.
        auto preparation = std::async(std::launch::async, [&] {
            pending.prepare_payload();
        });
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
            EXPECT_EQ(store.get("job").state(), task::proto::RUNNING);
            EXPECT_EQ(store.get("job").attempt(), 2U);
        } else {
            store.commit(store.finish(std::move(pending)));
            EXPECT_EQ(store.get("job").state(), task::proto::CANCELLED);
        }
        EXPECT_TRUE(std::filesystem::exists(directory.path / "job" / "results" / "1.pb"));
        EXPECT_THROW(tasks::result(store, "job"), std::invalid_argument);
    }
}

TEST(TaskStore, PreparedPayloadIsOwnedAndExpiredAttemptCannotCommit)
{
    TaskDirectory directory;
    tasks::Store store(asterion::research::task_domain(), directory.path, tasks::Identity{"task"});
    tasks::submit(store, "job", input());
    task::proto::TaskFinish request;
    request.set_id("job");
    request.set_token(store.commit(store.claim("job")).token());
    *request.mutable_result() = backtest::run(input());
    auto pending = store.prepare_finish(request);
    request.mutable_result()->set_dataset_revision("changed.after.prepare");
    EXPECT_NO_THROW(pending.prepare_payload());
    store.commit(store.interrupt("job", request.token(), "test lease expired"));
    EXPECT_THROW(store.commit(store.finish(std::move(pending))), std::invalid_argument);
    EXPECT_EQ(store.get("job").state(), task::proto::INTERRUPTED);
    EXPECT_TRUE(std::filesystem::exists(directory.path / "job" / "results" / "1.pb"));
    EXPECT_THROW(tasks::result(store, "job"), std::invalid_argument);
}

TEST(TaskStore, InputReadPreservesConcurrentCancellationAndRejectsForeignOrChangedFiles)
{
    TaskDirectory directory;
    tasks::Store store(asterion::research::task_domain(), directory.path, tasks::Identity{"task"});
    tasks::submit(store, "job", input());
    auto read = store.prepare_input("job");
    EXPECT_THROW(store.confirm_input(read), std::invalid_argument);
    EXPECT_THROW((void)store.claim(read), std::invalid_argument);
    auto loading = std::async(std::launch::async, [&] {
        read.load();
    });
    store.commit(store.cancel("job")).task();
    loading.get();
    TaskDirectory foreign_directory;
    tasks::Store foreign(
        asterion::research::task_domain(),
        foreign_directory.path,
        tasks::Identity{"task"});
    tasks::submit(foreign, "job", input());
    EXPECT_THROW(foreign.confirm_input(read), std::invalid_argument);
    EXPECT_THROW((void)foreign.claim(read), std::invalid_argument);
    EXPECT_THROW((void)store.claim(read), std::invalid_argument);
    const auto value = store.confirm_input(std::move(read));
    EXPECT_EQ(value.state(), task::proto::CANCELLED);
    EXPECT_EQ(value.input().SerializeAsString(), input().SerializeAsString());
    EXPECT_THROW(store.commit(store.claim("job")).token(), std::invalid_argument);
    auto changed = store.prepare_input("job");
    std::ofstream(directory.path / "job/input.pb", std::ios::binary | std::ios::trunc) << "bad";
    EXPECT_THROW(changed.load(), std::invalid_argument);
    EXPECT_THROW(store.confirm_input(std::move(changed)), std::invalid_argument);
}

TEST(TaskStore, ResultReadOwnsItsSnapshotAndChecksFilesBeforeCacheConfirmation)
{
    TaskDirectory directory;
    const auto expected = backtest::run(input());
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        tasks::submit(store, "done", input());
        store.commit(
            tasks::finish(store, "done", store.commit(store.claim("done")).token(), expected));
    }
    tasks::Store store(asterion::research::task_domain(), directory.path, tasks::Identity{"task"});
    tasks::submit(store, "other", input());
    const auto token = store.commit(store.claim("other")).token();
    auto read = store.prepare_result("done");
    EXPECT_THROW(store.confirm_result(read), std::invalid_argument);
    auto verifying = std::async(std::launch::async, [&] {
        read.verify();
    });
    store.commit(store.progress("other", token, 1));
    store.commit(store.cancel("other")).task();
    verifying.get();
    TaskDirectory other_directory;
    tasks::Store other(
        asterion::research::task_domain(),
        other_directory.path,
        tasks::Identity{"task"});
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

TEST(TaskStore, VerificationAdmissionReservesBothCompletionsAndReleasesAfterFailure)
{
    tasks::VerificationSlots slots;
    {
        auto read = slots.acquire(false, "read");
        auto first = slots.acquire(true, "first");
        auto second = slots.acquire(true, "second");
        EXPECT_FALSE(slots.completion_available());
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
    EXPECT_TRUE(slots.completion_available());
    EXPECT_NO_THROW(slots.acquire(false, "next-read"));
    auto first = slots.acquire(true, "first");
    EXPECT_THROW(slots.acquire(true, "first"), Error);
    EXPECT_NO_THROW(slots.acquire(true, "second"));
}

TEST(TaskStore, InvalidInputAndQueuedCancellationDoNotRunAnything)
{
    TaskDirectory directory;
    tasks::Store store(asterion::research::task_domain(), directory.path, tasks::Identity{"task"});
    auto spec = input();
    spec.set_dataset_revision("invalid");
    EXPECT_THROW(tasks::submit(store, "invalid", spec), std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(directory.path / "invalid"));
    EXPECT_THROW(tasks::submit(store, "../outside", input()), std::exception);
    tasks::submit(store, "queued", input());
    EXPECT_EQ(store.commit(store.cancel("queued")).task().state(), task::proto::CANCELLED);
    EXPECT_THROW(store.commit(store.claim("queued")).token(), std::invalid_argument);
    EXPECT_EQ(store.get("queued").attempt(), 0U);
}

#include <asterion/kernel/ipc/local_channel.hpp>
#include <thread>
#include <array>

namespace {

using namespace std::chrono_literals;
namespace task_wire = task::proto;

struct TaskProcess : testing::Test
{
    TaskDirectory directory, warehouse;
    const std::string fixture_program_artifact = sha256_file(ASTERION_RESEARCH_PATH);
    std::unique_ptr<ChildProcess> service, data_service;
    std::unique_ptr<testing_support::DataDiscovery> discovery;
    std::filesystem::path sockets;
    std::string endpoint;

    void SetUp() override
    {
        sockets = std::filesystem::path("/tmp") / ("ast-r-" + unique_process_id().substr(0, 12));
        std::filesystem::create_directory(sockets);
        std::filesystem::permissions(sockets, std::filesystem::perms::owner_all);
        endpoint = (sockets / "task.sock").string();
        replace_file_durably(sockets / "research.json", R"({"task_concurrency":2})");
        discovery = std::make_unique<testing_support::DataDiscovery>(
            endpoint + ".discovery",
            "historical-data",
            endpoint + ".data");
        start();
    }

    void TearDown() override
    {
        service.reset();
        data_service.reset();
        discovery.reset();
        if (!sockets.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(sockets, ec);
        }
    }

    task_wire::TaskResponse call(
        research::proto::ResearchRequest request,
        const std::string& target = "",
        std::chrono::milliseconds timeout = 2s)
    {
        if (request.has_submit() && !request.submit().has_data_owner()
            && !request.submit().has_statistics() && !request.submit().has_logic_trial())
            request.mutable_submit()->mutable_data_owner()->set_service_id("historical-data");
        request.set_version(1);
        request.set_service_id("task");
        request.set_correlation_id(unique_process_id());
        auto channel = ipc::Channel::connect(target.empty() ? endpoint : target, 2s);
        channel.send(request.SerializeAsString(), 2s);
        task_wire::TaskResponse response;
        if (!response.ParseFromString(channel.receive(timeout)))
            throw std::runtime_error("bad task response");
        protocol::validate_message(response);
        if (response.version() != 1 || response.service_id() != request.service_id()
            || response.correlation_id() != request.correlation_id())
            throw std::runtime_error("wrong task response identity");
        if (response.has_error())
            throw std::runtime_error(response.error().message());
        return response;
    }

    void start_data()
    {
        data_service = std::make_unique<ChildProcess>(
            ASTERION_DATA_SERVICE_PATH,
            std::vector<std::string>{
                "--directory",
                warehouse.path.string(),
                "--session",
                "historical-data",
                "--node-id",
                "fixture-node",
                "--endpoint",
                endpoint + ".data",
                "--internal-endpoint",
                endpoint + ".data-workers",
                "--file-workers",
                "1"
            });
        protocol::DataClient client(endpoint + ".data", "historical-data");
        const auto until = std::chrono::steady_clock::now() + 10s;
        for (;;) {
            if (std::chrono::steady_clock::now() >= until)
                throw std::runtime_error("data fixture did not initialize");
            try {
                data::proto::DataRequest request;
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

    void launch(bool publish_process = true, const std::filesystem::path& storage_root = {})
    {
        std::filesystem::remove(endpoint); // This test exclusively owns the private directory.
        std::filesystem::remove(endpoint + ".health");
        const auto directory_utf8 =
            (storage_root.empty() ? directory.path : storage_root).u8string();
        service = std::make_unique<ChildProcess>(
            ASTERION_RESEARCH_PATH,
            std::vector<std::string>{
                "--config",
                (sockets / "research.json").string(),
                "--directory",
                std::string(directory_utf8.begin(), directory_utf8.end()),
                "--endpoint",
                endpoint,
                "--health-endpoint",
                endpoint + ".health",
                "--session",
                "task",
                "--node-id",
                "fixture-node",
                "--discovery-endpoint",
                endpoint + ".discovery",
                "--file-workers",
                "1"
            });
        if (publish_process)
            discovery->research("task", service->id(), fixture_program_artifact);
    }

    void start(std::chrono::milliseconds timeout = 10s)
    {
        launch();
        wait_for_initialization(timeout);
    }

    void wait_for_initialization(std::chrono::milliseconds timeout = 10s)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            try {
                research::proto::ResearchRequest req;
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

    std::optional<backtest::proto::BacktestInput> fixed_input;

    data::proto::BarDataset published_dataset(const std::vector<int>& prices, const std::string& id)
    {
        data_service.reset();
        std::vector<std::string> days;
        const auto first = std::chrono::sys_days(std::chrono::year(2024) / 1 / 1);
        for (std::size_t day = 0; day < (prices.size() + 239) / 240; ++day)
            days.push_back(
                format_trading_date(std::chrono::year_month_day(first + std::chrono::days(day))));
        const auto selection = test::seed_history(warehouse.path, prices, id, days);
        data::proto::BarDatasetRequest request;
        for (const auto& version : selection.at("source_dataset_ids"))
            request.add_source_dataset_ids(version.get<std::string>());
        *request.mutable_daily() = protocol::encode_daily_table(selection.at("daily"));
        request.set_contract("SHFE/rb/2026-10");
        data::proto::BarDataset dataset;
        {
            data::Store store(warehouse.path, "historical-data");
            dataset = data::resolve_bar_dataset(store.tables(), store.sources(request));
        }
        start_data();
        return dataset;
    }

    const backtest::proto::BacktestInput& process_input()
    {
        if (!fixed_input) {
            fixed_input = input();
            *fixed_input->mutable_paper()->mutable_contracts(0)->mutable_dataset() =
                published_dataset({100, 101, 102, 101, 100, 101, 103}, "worker-input");
            fixed_input->set_dataset_revision(protocol::dataset_revision(fixed_input->paper()));
        }
        return *fixed_input;
    }

    std::string preserved_program;

    void preserve_program()
    {
        if (preserved_program.empty()) {
            resource::proto::Owner owner;
            owner.set_node_id("fixture-node");
            owner.set_service_id("task");
            owner.set_domain("research");
            preserved_program = programs::Files(directory.path / "programs", owner)
                                    .preserve(ASTERION_RESEARCH_PATH, fixture_program_artifact)
                                    .digest;
        }
    }

    template <class Input>
    task::proto::Task persist(tasks::Store& store, const std::string& id, Input input)
    {
        preserve_program();
        auto submission = task_submission(store, id, std::move(input));
        asterion::tasks::set_program_artifact(submission, preserved_program);
        return tasks::submit(store, std::move(submission));
    }

    void submit(const std::string& id)
    {
        service.reset();
        {
            tasks::Store store(
                asterion::research::task_domain(),
                directory.path,
                tasks::Identity{"task"});
            persist(store, id, process_input());
        }
        start();
    }
};

} // namespace

TEST(ResearchExecution, CapturedProgramCannotChangeBetweenTheTaskProjectionAndLoadedCalculation)
{
    task::proto::TaskAttempt attempt;
    attempt.mutable_task()->set_kind(task::proto::BACKTEST);
    *attempt.mutable_task()->mutable_input() = input();
    attempt.mutable_task()->set_program_artifact(std::string(64, 'a'));
    *attempt.mutable_execution() = protocol::task_execution(
        attempt.task(),
        sha256_bytes(attempt.task().input().SerializeAsString()));
    const auto expected = attempt.execution();
    attempt.mutable_execution()->set_program_artifact(std::string(64, 'b'));
    EXPECT_THROW(tasks::resolve_task_input(attempt, {}), std::invalid_argument);
    *attempt.mutable_execution() = expected;
    TaskDirectory isolated;
    EXPECT_THROW(tasks::execute(attempt, isolated.path, {}, {}, std::string(64, 'b')), Error);
}

TEST_F(TaskProcess, ServiceRunsWithoutSubmittingClientAndResultSurvivesRestart)
{
    submit("run1"); // Every request closes its connection; no UI client remains.
    research::proto::ResearchRequest fixed;
    fixed.mutable_get()->set_id("run1");
    const auto artifact = sha256_file(ASTERION_RESEARCH_PATH);
    EXPECT_EQ(call(fixed).task().program_artifact(), artifact);
    EXPECT_EQ(sha256_file(directory.path / "programs" / artifact), artifact);
    EXPECT_EQ(
        testing_support::wait_task(
            [&](auto request) {
                return call(request);
            },
            "run1")
            .state(),
        task::proto::SUCCEEDED);
    research::proto::ResearchRequest result;
    result.mutable_result()->set_id("run1");
    EXPECT_EQ(
        call(result).backtest().SerializeAsString(),
        backtest::run(process_input()).SerializeAsString());
    research::proto::ResearchRequest list;
    list.mutable_list()->set_limit(200);
    auto response = call(list);
    ASSERT_EQ(response.tasks().tasks_size(), 1);
    EXPECT_FALSE(response.tasks().tasks(0).has_input());
    service.reset();
    start();
    EXPECT_EQ(
        call(result).backtest().SerializeAsString(),
        backtest::run(process_input()).SerializeAsString());
    submit("run1"); // Same task ID after restart remains idempotent.
    EXPECT_EQ(call(list).tasks().tasks_size(), 1);
    EXPECT_EQ(call(result).result_task().program_artifact(), artifact);
}

TEST_F(TaskProcess, QueuedCalculationWaitsForCurrentProcessObservationWithoutBlockingControls)
{
    const auto stale_pid = service->id();
    auto fixed = process_input();
    service.reset();
    {
        tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
        persist(store, "waiting-for-current-process", fixed);
    }
    // Retain the stopped observation until this test explicitly publishes the new PID.
    discovery->research("task", stale_pid, fixture_program_artifact);
    launch(false);
    wait_for_initialization();
    research::proto::ResearchRequest get;
    get.mutable_get()->set_id("waiting-for-current-process");
    const auto pending = call(get).task();
    EXPECT_EQ(pending.state(), task::proto::QUEUED);
    EXPECT_EQ(pending.attempt(), 0U);
    discovery->research("task", service->id(), fixture_program_artifact);
    EXPECT_EQ(
        testing_support::wait_task(
            [&](auto request) {
                return call(request);
            },
            "waiting-for-current-process")
            .state(),
        task::proto::SUCCEEDED);
    EXPECT_EQ(call(get).task().program_artifact(), fixture_program_artifact);
}

TEST_F(TaskProcess, ABacktestOfAProductTradesTheMonthsTheDailyTableNames)
{
    // Rebar over six days of the daily table: October is the most held until
    // the third day, January from the fourth; May never trades.
    const char* days[] =
        {"2026-09-21", "2026-09-22", "2026-09-23", "2026-09-24", "2026-09-25", "2026-09-28"};
    const auto traded = [&](int base, std::vector<int> held) {
        std::vector<test::DailyRow> rows;
        for (int i = 0; i < 6; ++i) {
            const int price = base + 2 * i;
            rows.push_back(
                {days[i],
                    price,
                    held[static_cast<std::size_t>(i)],
                    test::DailyTrade{price, price + 5, price - 5, price, 100}});
        }
        return rows;
    };
    data_service.reset();
    {
        data::Store created(warehouse.path, "historical-data");
    }
    (void)test::seed_daily_table(
        warehouse.path,
        "SHFE/rb/2026-10",
        traded(200, {100, 100, 40, 30, 20, 10}));
    (void)test::seed_daily_table(
        warehouse.path,
        "SHFE/rb/2027-01",
        traded(100, {10, 60, 90, 95, 99, 99}));
    std::vector<test::DailyRow> idle;
    for (const auto* day : days)
        idle.push_back({day, 150, 5});
    const auto table = test::seed_daily_table(warehouse.path, "SHFE/rb/2027-05", idle);
    start_data();
    research::proto::ResearchRequest submit;
    submit.mutable_submit()->set_id("product");
    auto* backtest = submit.mutable_submit()->mutable_backtest();
    auto* product = backtest->add_series()->mutable_product();
    *product->mutable_data() = protocol::encode_product_bars_request(
        {{"venue", "SHFE"},
            {"product", "rb"},
            {"begin_day", days[0]},
            {"end_day", days[5]},
            {"daily", table}});
    *product->mutable_cost_schedule() = input().paper().contracts(0).cost_schedule();
    product->set_slippage_ticks(1);
    *backtest->mutable_deposit() = input().paper().deposit();
    *backtest->mutable_risk() = input().paper().risk();
    *backtest->add_strategies() = input().strategies(0);
    EXPECT_EQ(call(submit).task().state(), task_wire::QUEUED);
    EXPECT_EQ(
        testing_support::wait_task(
            [&](auto request) {
                return call(request);
            },
            "product")
            .state(),
        task::proto::SUCCEEDED);
    research::proto::ResearchRequest get;
    get.mutable_get()->set_id("product");
    const auto task = call(get).task();
    EXPECT_EQ(task.state(), task_wire::SUCCEEDED);
    // The months that trade are the contracts, each with the product's costs;
    // the month nobody traded is not among them.
    const auto& paper = task.input().paper();
    ASSERT_EQ(paper.contracts_size(), 2);
    EXPECT_EQ(paper.contracts(0).dataset().contract().symbol(), "rb2610");
    EXPECT_EQ(paper.contracts(1).dataset().contract().symbol(), "rb2701");
    for (const auto& contract : paper.contracts()) {
        EXPECT_EQ(contract.dataset().interval_minutes(), 0U);
        EXPECT_EQ(contract.dataset().source_dataset_ids_size(), 0);
        EXPECT_EQ(contract.slippage_ticks(), 1U);
        EXPECT_EQ(
            contract.cost_schedule().SerializeAsString(),
            product->cost_schedule().SerializeAsString());
    }
    ASSERT_EQ(task.input().series_size(), 1);
    ASSERT_EQ(task.input().series(0).rolls_size(), 2);
    EXPECT_EQ(task.input().series(0).rolls(1).trading_day(), days[3]);
    // A list names the series once, over all its days, and the snapshot its
    // months were read under once.
    EXPECT_EQ(task.instrument(), "SHFE/rb (rb2610-rb2701)");
    EXPECT_EQ(task.trading_day(), std::string(days[1]) + " / " + days[5]);
    EXPECT_EQ(task.source_name(), table.at("snapshot").get<std::string>());
    research::proto::ResearchRequest result;
    result.mutable_result()->set_id("product");
    // Settled every day from the second, on which the series begins.
    const auto original_result = call(result);
    EXPECT_EQ(original_result.backtest().settlements_size(), 5);
    const auto original_bytes = task.input().SerializeAsString();
    const auto reuse = [&](const std::string& id,
                           bool fresh,
                           const std::string& snapshot,
                           const std::string& digest) {
        research::proto::ResearchRequest request;
        request.mutable_submit()->set_id(id);
        auto* ref = request.mutable_submit()->mutable_backtest_reuse();
        ref->set_source_id("product");
        ref->set_source_result_digest(task.result_digest());
        ref->set_source_input_sha256(digest);
        ref->set_choice(
            fresh ? backtest::proto::BacktestReuse::NEW_SNAPSHOT
                  : backtest::proto::BacktestReuse::FIXED);
        ref->set_snapshot(snapshot);
        return call(request);
    };
    const auto digest = sha256_bytes(original_bytes);
    EXPECT_THROW(reuse("stale", false, "", std::string(64, 'f')), std::runtime_error);
    EXPECT_EQ(reuse("fixed", false, "", digest).task().state(), task_wire::QUEUED);
    EXPECT_EQ(
        testing_support::wait_task(
            [&](auto request) {
                return call(request);
            },
            "fixed")
            .state(),
        task::proto::SUCCEEDED);
    result.mutable_result()->set_id("fixed");
    const auto fixed_result = call(result);
    EXPECT_EQ(fixed_result.result_task().input().SerializeAsString(), original_bytes);
    EXPECT_EQ(
        fixed_result.backtest().SerializeAsString(),
        original_result.backtest().SerializeAsString());
    data_service.reset();
    const auto revised = test::seed_daily_table(
        warehouse.path,
        "SHFE/rb/2027-01",
        traded(100, {10, 200, 90, 95, 99, 99}));
    start_data();
    EXPECT_EQ(
        reuse("revised", true, revised.at("snapshot").get<std::string>(), digest).task().state(),
        task_wire::QUEUED);
    EXPECT_EQ(
        testing_support::wait_task(
            [&](auto request) {
                return call(request);
            },
            "revised")
            .state(),
        task::proto::SUCCEEDED);
    result.mutable_result()->set_id("revised");
    const auto revised_result = call(result);
    const auto& revised_input = revised_result.result_task().input();
    EXPECT_NE(revised_input.dataset_revision(), task.input().dataset_revision());
    ASSERT_EQ(revised_input.series(0).rolls_size(), 2);
    EXPECT_EQ(revised_input.series(0).rolls(0).trading_day(), days[1]);
    EXPECT_EQ(revised_input.series(0).rolls(1).trading_day(), days[2]);
    EXPECT_EQ(
        revised_input.strategies(0).SerializeAsString(),
        task.input().strategies(0).SerializeAsString());
    EXPECT_EQ(revised_input.paper().risk().SerializeAsString(), paper.risk().SerializeAsString());
    for (const auto& contract : revised_input.paper().contracts()) {
        EXPECT_EQ(
            contract.cost_schedule().SerializeAsString(),
            product->cost_schedule().SerializeAsString());
        EXPECT_EQ(contract.slippage_ticks(), 1U);
    }
    get.mutable_get()->set_id("product");
    EXPECT_EQ(call(get).task().input().SerializeAsString(), original_bytes);
    result.mutable_result()->set_id("product");
    EXPECT_EQ(
        call(result).backtest().SerializeAsString(),
        original_result.backtest().SerializeAsString());
}

TEST_F(TaskProcess, ExecutionRejectsDataEvidenceDifferentFromTheAcceptedInput)
{
    auto spec = process_input();
    auto* evidence =
        spec.mutable_paper()->mutable_contracts(0)->mutable_dataset()->mutable_history_evidence(0);
    evidence->set_acquired_at_ns(evidence->acquired_at_ns() + 1);
    service.reset();
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        persist(store, "different-evidence", spec);
    }
    start();
    EXPECT_EQ(
        testing_support::wait_task(
            [&](auto request) {
                return call(request);
            },
            "different-evidence")
            .state(),
        task::proto::FAILED);
    research::proto::ResearchRequest get;
    get.mutable_get()->set_id("different-evidence");
    const auto task = call(get).task();
    EXPECT_EQ(task.state(), task_wire::FAILED);
    EXPECT_EQ(task.error(), "calculation input does not match fixed task evidence");
    EXPECT_TRUE(task.result_digest().empty());
}

TEST_F(TaskProcess, InvalidFixedInputsFailTheirAttemptWithoutRequeueing)
{
    const auto spec = process_input();
    service.reset();
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        persist(store, "other-rules", spec);
        persist(store, "other-messages", spec);
    }
    // A digest-matched input with an unknown field is still invalid.
    {
        const auto path = directory.path / "other-messages" / "input.pb";
        std::ifstream stream(path, std::ios::binary);
        std::string raw((std::istreambuf_iterator<char>(stream)), {});
        stream.close();
        raw += "\xb8\x3e\x01"; // field 999, a varint
        replace_file_durably(path, raw);
        sqlite::Database db(directory.path / "tasks.sqlite");
        Json manifest;
        task::proto::StoredTaskRecord record;
        {
            sqlite::Database::Statement read(
                db,
                "SELECT manifest, record FROM tasks WHERE id='other-messages'");
            ASSERT_TRUE(read.step());
            manifest = Json::parse(read.text(0));
            ASSERT_TRUE(record.ParseFromString(read.blob(1)));
        }
        manifest["input_sha256"] = sha256_bytes(raw);
        manifest["input_bytes"] = raw.size();
        record.set_input_digest(sha256_bytes(raw));
        record.set_input_bytes(raw.size());
        sqlite::Database::Statement write(
            db,
            "UPDATE tasks SET manifest=?, record=? WHERE id='other-messages'");
        write.bind(1, manifest.dump()).bind_blob(2, record.SerializeAsString()).step();
    }
    // A consistent index can still disagree with the fixed input's work.
    unsigned admitted = 0;
    {
        sqlite::Database db(directory.path / "tasks.sqlite");
        task::proto::StoredTaskRecord record;
        {
            sqlite::Database::Statement read(db, "SELECT record FROM tasks WHERE id='other-rules'");
            ASSERT_TRUE(read.step());
            ASSERT_TRUE(record.ParseFromString(read.blob(0)));
        }
        admitted = record.task().total() + 1;
        record.mutable_task()->set_total(admitted);
        sqlite::Database::Statement write(db, "UPDATE tasks SET record=? WHERE id='other-rules'");
        write.bind_blob(1, record.SerializeAsString()).step();
    }
    // The store agrees with itself, so the service opens it and lists the task
    // as it was stored.
    start();
    research::proto::ResearchRequest get;
    get.mutable_get()->set_id("other-rules");
    EXPECT_EQ(call(get).task().state(), task_wire::QUEUED);
    EXPECT_EQ(call(get).task().total(), admitted);
    // The invalid attempt fails once and leaves the queue.
    EXPECT_EQ(
        testing_support::wait_task(
            [&](auto request) {
                return call(request);
            },
            "other-rules")
            .state(),
        task::proto::FAILED);
    const auto refused = call(get).task();
    EXPECT_EQ(refused.state(), task_wire::FAILED);
    EXPECT_EQ(refused.attempt(), 1U);
    EXPECT_EQ(refused.total(), admitted);
    EXPECT_EQ(refused.error(), "task input does not match admitted work");
    // Reference inspection fails explicitly rather than returning a partial answer.
    research::proto::ResearchRequest usage;
    usage.mutable_history_usage()->set_id(
        spec.paper().contracts(0).dataset().source_dataset_ids(0));
    EXPECT_THROW(call(usage), std::runtime_error);
    EXPECT_EQ(
        testing_support::wait_task(
            [&](auto request) {
                return call(request);
            },
            "other-messages")
            .state(),
        task::proto::FAILED);
    // Asking for the task with its input is refused for the same reason; the
    // list, which reads the index, shows how its attempt ended.
    get.mutable_get()->set_id("other-messages");
    EXPECT_THROW(call(get), std::runtime_error);
    research::proto::ResearchRequest list;
    list.mutable_list()->set_limit(200);
    const auto tasks = call(list).tasks().tasks();
    const auto listed = std::ranges::find(tasks, "other-messages", &task_wire::Task::id);
    ASSERT_NE(listed, tasks.end());
    EXPECT_EQ(listed->state(), task_wire::FAILED);
    EXPECT_EQ(listed->error(), "unknown Protobuf field");
    service.reset();
    start();
    get.mutable_get()->set_id("other-rules");
    EXPECT_EQ(call(get).task().SerializeAsString(), refused.SerializeAsString());
}

TEST_F(TaskProcess, ListenerServesRepeatedConnectionsAndRetainsOwnership)
{
    for (int i = 0; i < 5; ++i) {
        research::proto::ResearchRequest request;
        request.mutable_heartbeat();
        EXPECT_TRUE(call(request).has_health());
    }
    EXPECT_THROW(testing_support::LocalListener duplicate(endpoint), Error);
}

// Research knows its node from its launch arguments; its own records must not
// depend on the Agent staying reachable.
TEST_F(TaskProcess, MetadataRequestsDoNotNeedAgentDiscovery)
{
    discovery.reset();
    research::proto::ResearchRequest registration;
    auto& operation = *registration.mutable_logic()->mutable_mutation();
    operation.set_id("register-without-agent");
    auto& rule = *operation.mutable_registration();
    rule.set_id("two-means");
    rule.set_name("Synthetic two means");
    rule.set_purpose("Pure causal logic, not market performance");
    rule.set_fast(1);
    rule.set_slow(3);
    rule.set_input(research::proto::LogicDefinition::COMPLETED_CLOSE);
    rule.set_output(research::proto::LogicDefinition::TARGET_SIGN);
    const auto definition = call(registration).logic().definition();
    EXPECT_EQ(definition.owner().node_id(), "fixture-node");
}

TEST_F(
    TaskProcess,
    LogicTrialsUsePriorFictionalExpectationsWithoutDataAndKeepOriginalAfterDeprecation)
{
    EXPECT_FALSE(data_service);
    research::proto::ResearchRequest registration;
    auto& operation = *registration.mutable_logic()->mutable_mutation();
    operation.set_id("register-rule");
    auto& rule = *operation.mutable_registration();
    rule.set_id("two-means");
    rule.set_name("Synthetic two means");
    rule.set_purpose("Pure causal logic, not market performance");
    rule.set_fast(1);
    rule.set_slow(3);
    rule.set_input(research::proto::LogicDefinition::COMPLETED_CLOSE);
    rule.set_output(research::proto::LogicDefinition::TARGET_SIGN);
    const auto definition = call(registration).logic().definition();
    research::proto::ResearchRequest save;
    auto& mutation = *save.mutable_logic()->mutable_mutation();
    mutation.set_id("save-prior-case");
    auto& example = *mutation.mutable_example();
    example.set_id("prior-case");
    *example.mutable_definition() = protocol::logic_object(definition);
    example.set_name("Prior fictional mismatch");
    example.set_use_scope("Four fictional observations only; no financial conclusion");
    example.set_fictional(true);
    for (int price : {10, 10, 13, 7}) {
        const auto index = example.observations_size();
        auto& row = *example.add_observations();
        row.set_closed_at_ns((index + 1) * 60000000000LL);
        row.set_available_at_ns(row.closed_at_ns());
        row.mutable_close()->set_units(Decimal::parse(std::to_string(price)).raw());
        if (index < 2)
            row.set_warmup(true);
        else
            row.set_target_sign(-1);
    }
    const auto saved = call(save).logic().example();
    EXPECT_EQ(call(save).logic().example().SerializeAsString(), saved.SerializeAsString());
    auto changed = save;
    changed.mutable_logic()
        ->mutable_mutation()
        ->mutable_example()
        ->mutable_observations(2)
        ->set_target_sign(1);
    EXPECT_THROW(call(changed), std::runtime_error);
    research::proto::ResearchRequest submit;
    submit.mutable_submit()->set_id("logic-comparison");
    *submit.mutable_submit()->mutable_logic_trial()->mutable_example() =
        protocol::logic_object(saved);
    call(submit);
    const auto finished = testing_support::wait_task(
        [&](auto request) {
            return call(request);
        },
        "logic-comparison");
    ASSERT_EQ(finished.state(), task_wire::SUCCEEDED) << finished.error();
    research::proto::ResearchRequest read;
    read.mutable_result()->set_id("logic-comparison");
    const auto original = call(read);
    ASSERT_TRUE(original.has_logic_trial());
    EXPECT_FALSE(original.logic_trial().all_match());
    EXPECT_EQ(original.logic_trial().matched(), 3U);
    EXPECT_EQ(original.logic_trial().decisions(), 2U);
    EXPECT_EQ(
        original.result_task().logic_trial().example().SerializeAsString(),
        saved.SerializeAsString());
    EXPECT_EQ(original.result_task().program_artifact(), fixture_program_artifact);
    service.reset();
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        auto prepared =
            task_submission(store, "logic-prepared", original.result_task().logic_trial());
        asterion::tasks::set_program_artifact(prepared, fixture_program_artifact);
        store.admit_submission(prepared);
        prepared.prepare_files();
        std::filesystem::remove(directory.path / "logic-prepared/input.pb");
    }
    start();
    research::proto::ResearchRequest recover;
    recover.mutable_recover_submission()->set_id("logic-prepared");
    EXPECT_EQ(call(recover).task().kind(), task_wire::LOGIC_TRIAL);
    const auto recovered = testing_support::wait_task(
        [&](auto request) {
            return call(request);
        },
        "logic-prepared");
    ASSERT_EQ(recovered.state(), task_wire::SUCCEEDED) << recovered.error();
    research::proto::ResearchRequest recovered_read;
    recovered_read.mutable_result()->set_id("logic-prepared");
    EXPECT_EQ(
        call(recovered_read).logic_trial().SerializeAsString(),
        original.logic_trial().SerializeAsString());
    service.reset();
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        auto prepared =
            task_submission(store, "logic-abandoned", original.result_task().logic_trial());
        asterion::tasks::set_program_artifact(prepared, fixture_program_artifact);
        store.admit_submission(prepared);
        prepared.prepare_files();
        std::filesystem::remove(directory.path / "logic-abandoned/input.pb");
    }
    start();
    research::proto::ResearchRequest abandon;
    abandon.mutable_abandon_preparation()->set_id("logic-abandoned");
    const auto decision = call(abandon).release_decision();
    EXPECT_EQ(decision.state(), resource::proto::ReleaseDecision::RELEASED);
    EXPECT_EQ(decision.object().kind(), "logic_trial");
    EXPECT_EQ(call(abandon).release_decision().SerializeAsString(), decision.SerializeAsString());
    recover.mutable_recover_submission()->set_id("logic-abandoned");
    EXPECT_THROW(call(recover), std::runtime_error);
    research::proto::ResearchRequest retire;
    auto& removal = *retire.mutable_logic()->mutable_mutation();
    removal.set_id("deprecate-rule");
    *removal.mutable_retire()->mutable_definition() = protocol::logic_object(definition);
    removal.mutable_retire()->set_reason("No new trials; retain original evidence");
    EXPECT_FALSE(call(retire).logic().definition().retirement_reason().empty());
    EXPECT_EQ(call(submit).task().state(), task_wire::SUCCEEDED);
    auto forbidden = submit;
    forbidden.mutable_submit()->set_id("forbidden-trial");
    EXPECT_THROW(call(forbidden), std::runtime_error);
    service.reset();
    start();
    EXPECT_FALSE(data_service);
    const auto reopened = call(read);
    EXPECT_EQ(
        reopened.result_task().SerializeAsString(),
        original.result_task().SerializeAsString());
    EXPECT_EQ(
        reopened.logic_trial().SerializeAsString(),
        original.logic_trial().SerializeAsString());
    EXPECT_EQ(call(submit).task().state(), task_wire::SUCCEEDED);
    EXPECT_EQ(call(save).logic().example().SerializeAsString(), saved.SerializeAsString());
    research::proto::ResearchRequest retirement;
    retirement.mutable_retirement()->set_id("logic-comparison");
    EXPECT_EQ(call(retirement).retirement().state(), task_wire::TaskRetirement::AVAILABLE);
    retirement.mutable_retirement()->set_execute(true);
    const auto retired = call(retirement).retirement();
    EXPECT_EQ(retired.state(), task_wire::TaskRetirement::RETIRED);
    EXPECT_TRUE(retired.source_release_confirmed());
    EXPECT_EQ(
        call(read).logic_trial().SerializeAsString(),
        original.logic_trial().SerializeAsString());
}

TEST_F(
    TaskProcess,
    MatchingLogicPackageRequiresExplicitBacktestConditionsAndKeepsActualUseAfterRestart)
{
    research::proto::ResearchRequest registration;
    auto& operation = *registration.mutable_logic()->mutable_mutation();
    operation.set_id("register-logic");
    auto& rule = *operation.mutable_registration();
    rule.set_id("means");
    rule.set_name("Fictional logic");
    rule.set_purpose("No market performance claim");
    rule.set_fast(1);
    rule.set_slow(3);
    rule.set_input(research::proto::LogicDefinition::COMPLETED_CLOSE);
    rule.set_output(research::proto::LogicDefinition::TARGET_SIGN);
    const auto definition = call(registration).logic().definition();
    research::proto::ResearchRequest save;
    auto& mutation = *save.mutable_logic()->mutable_mutation();
    mutation.set_id("prior-expectations");
    auto& example = *mutation.mutable_example();
    example.set_id("prior-case");
    example.set_name("Fictional rising and falling");
    example.set_use_scope("Pure causal rule check only");
    example.set_fictional(true);
    *example.mutable_definition() = protocol::logic_object(definition);
    for (int price : {10, 10, 13, 7}) {
        const auto index = example.observations_size();
        auto& row = *example.add_observations();
        row.set_closed_at_ns((index + 1) * 60000000000LL);
        row.set_available_at_ns(row.closed_at_ns());
        row.mutable_close()->set_units(Decimal::parse(std::to_string(price)).raw());
        if (index < 2)
            row.set_warmup(true);
        else
            row.set_target_sign(index == 2 ? 1 : -1);
    }
    const auto saved = call(save).logic().example();
    research::proto::ResearchRequest trial;
    trial.mutable_submit()->set_id("matching-logic");
    *trial.mutable_submit()->mutable_logic_trial()->mutable_example() =
        protocol::logic_object(saved);
    call(trial);
    ASSERT_EQ(
        testing_support::wait_task(
            [&](auto request) {
                return call(request);
            },
            "matching-logic")
            .state(),
        task_wire::SUCCEEDED);
    research::proto::ResearchRequest read;
    read.mutable_result()->set_id("matching-logic");
    const auto source = call(read);
    ASSERT_TRUE(source.logic_trial().all_match());
    const auto input = process_input();
    research::proto::ResearchRequest assemble;
    auto& frozen = *assemble.mutable_strategy()->mutable_mutation();
    frozen.set_id("assemble-logic");
    auto& package = *frozen.mutable_logic();
    package.set_id("logic-package");
    package.set_name("Independent rule candidate");
    package.set_purpose("Explicit research backtest");
    package.set_limitations("Fictional checking is not return evidence or live authorization");
    auto& reference = *package.mutable_source();
    *reference.mutable_owner() = saved.owner();
    reference.set_kind("logic_trial_result");
    reference.set_id("matching-logic");
    reference.set_version(source.result_task().result_digest());
    *package.mutable_contract() = input.paper().contracts(0).dataset().contract();
    package.set_interval_minutes(1);
    *package.mutable_strategy() = protocol::encode_strategy(
        {{"quantity", "1"},
            {"sides", "both"},
            {"rule", {{"kind", "moving_average"}, {"fast", 1}, {"slow", 3}}}});
    const auto candidate = call(assemble).strategy().package();
    ASSERT_TRUE(candidate.has_logic());
    EXPECT_FALSE(candidate.has_reference_result());
    research::proto::ResearchRequest deliver;
    auto& draft_request = *deliver.mutable_strategy()->mutable_mutation();
    draft_request.set_id("save-draft");
    draft_request.mutable_deliver()->set_id("logic-draft");
    draft_request.mutable_deliver()->set_target_task_id("logic-backtest");
    *draft_request.mutable_deliver()->mutable_package() =
        protocol::strategy_package_object(candidate);
    const auto draft = call(deliver).strategy().delivery();
    research::proto::ResearchRequest lookup;
    lookup.mutable_strategy()->mutable_delivery()->set_id(draft.id());
    EXPECT_FALSE(call(lookup).strategy().target_accepted());
    research::proto::ResearchRequest retirement;
    retirement.mutable_retirement()->set_id("matching-logic");
    EXPECT_EQ(call(retirement).retirement().state(), task_wire::TaskRetirement::BLOCKED);
    research::proto::ResearchRequest save_project;
    auto& saved_project = *save_project.mutable_project()->mutable_mutation();
    saved_project.set_id("save-backtest-project");
    saved_project.mutable_save()->set_project_id("backtest-question");
    auto& question = *saved_project.mutable_save()->mutable_definition();
    question.set_title("Synthetic fixed backtest hypothesis");
    question.set_question("Does this declared rule meet the recorded research criterion?");
    question.set_hypothesis("A proposed relation, not an established market fact");
    question.set_support("Declared independent evidence supports the stated mechanism");
    question.set_refutation("Opposite observations or insufficient evidence");
    const auto project = call(save_project).project().draft();
    research::proto::ResearchRequest fix_plan;
    auto& fixing = *fix_plan.mutable_project()->mutable_mutation();
    fixing.set_id("fix-backtest-plan");
    fixing.mutable_fix()->set_project_id(project.id());
    fixing.mutable_fix()->set_plan_id("before-backtest");
    fixing.mutable_fix()->set_expected_draft_digest(project.digest());
    const auto plan = call(fix_plan).project().plan();
    research::proto::ProjectBinding binding;
    binding.set_project_id(plan.project_id());
    binding.set_plan_id(plan.id());
    binding.set_definition_digest(plan.definition_digest());
    *binding.mutable_owner() = plan.owner();
    research::proto::ResearchRequest accept;
    auto& submission = *accept.mutable_submit();
    submission.set_id(draft.target_task_id());
    *submission.mutable_research_plan() = binding;
    auto& spec = *submission.mutable_backtest();
    auto& contract = *spec.add_contracts();
    *contract.mutable_data() =
        protocol::bar_dataset_selection(input.paper().contracts(0).dataset());
    *contract.mutable_cost_schedule() = input.paper().contracts(0).cost_schedule();
    contract.set_slippage_ticks(input.paper().contracts(0).slippage_ticks());
    *spec.mutable_deposit() = input.paper().deposit();
    *spec.mutable_risk() = input.paper().risk();
    *spec.add_strategies() = candidate.definition().strategy();
    *submission.mutable_strategy_use()->mutable_package() =
        protocol::strategy_package_object(candidate);
    *submission.mutable_strategy_use()->mutable_delivery() =
        protocol::strategy_delivery_object(draft);
    auto wrong_plan = accept;
    wrong_plan.mutable_submit()->mutable_research_plan()->set_plan_id("missing-plan");
    EXPECT_THROW(call(wrong_plan), std::runtime_error);
    auto missing_risk = accept;
    missing_risk.mutable_submit()->mutable_backtest()->clear_risk();
    EXPECT_THROW(call(missing_risk), std::runtime_error);
    auto wrong_rule = accept;
    wrong_rule.mutable_submit()
        ->mutable_backtest()
        ->mutable_strategies(0)
        ->mutable_moving_average()
        ->set_fast(2);
    EXPECT_THROW(call(wrong_rule), std::runtime_error);
    call(accept);
    const auto completed = testing_support::wait_task(
        [&](auto request) {
            return call(request);
        },
        draft.target_task_id());
    ASSERT_EQ(completed.state(), task_wire::SUCCEEDED) << completed.error();
    research::proto::ResearchRequest report;
    report.mutable_result()->set_id(draft.target_task_id());
    const auto actual = call(report);
    EXPECT_EQ(
        actual.result_task().strategy_use().request_sha256(),
        sha256_bytes(spec.SerializeAsString()));
    EXPECT_EQ(
        actual.result_task().strategy_use().package().SerializeAsString(),
        submission.strategy_use().package().SerializeAsString());
    EXPECT_EQ(
        actual.result_task().input().strategies(0).SerializeAsString(),
        candidate.definition().strategy().SerializeAsString());
    EXPECT_TRUE(call(lookup).strategy().target_accepted());
    EXPECT_EQ(
        actual.result_task().research_plan().SerializeAsString(),
        binding.SerializeAsString());
    research::proto::ResearchRequest project_tasks;
    project_tasks.mutable_project_tasks()->set_project_id(project.id());
    project_tasks.mutable_project_tasks()->set_limit(30);
    EXPECT_EQ(call(project_tasks).project_tasks().tasks_size(), 1);
    research::proto::ResearchRequest conclude;
    auto& conclusion = *conclude.mutable_project()->mutable_mutation();
    conclusion.set_id("human-original-verdict");
    auto& declaration = *conclusion.mutable_conclude();
    declaration.set_id("original-verdict");
    *declaration.mutable_plan() = binding;
    *declaration.mutable_result()->mutable_owner() = binding.owner();
    declaration.mutable_result()->set_kind("backtest_result");
    declaration.mutable_result()->set_id(draft.target_task_id());
    declaration.mutable_result()->set_version(actual.result_task().result_digest());
    declaration.set_judgment(research::proto::INSUFFICIENT);
    declaration.set_reason(
        "Synthetic simulation is not independent market evidence; metrics are not an automatic verdict");
    const auto original_verdict = call(conclude).project().conclusion();
    EXPECT_FALSE(original_verdict.retrospective());
    auto changed_project = save_project;
    changed_project.mutable_project()->mutable_mutation()->set_id("revise-after-result");
    auto* revised = changed_project.mutable_project()->mutable_mutation()->mutable_save();
    revised->set_expected_digest(project.digest());
    revised->mutable_definition()->set_support("Changed criterion after seeing the result");
    const auto later_draft = call(changed_project).project().draft();
    auto late_fix = fix_plan;
    auto* later = late_fix.mutable_project()->mutable_mutation();
    later->set_id("fix-later-plan");
    later->mutable_fix()->set_plan_id("after-backtest");
    later->mutable_fix()->set_expected_draft_digest(later_draft.digest());
    later->mutable_fix()->set_parent_plan_id(plan.id());
    later->mutable_fix()->set_revision_reason(
        "Follow-up criterion, never replace the original standard");
    const auto late_plan = call(late_fix).project().plan();
    auto late_binding = binding;
    late_binding.set_plan_id(late_plan.id());
    late_binding.set_definition_digest(late_plan.definition_digest());
    auto late_conclusion = conclude;
    auto* late = late_conclusion.mutable_project()->mutable_mutation();
    late->set_id("human-later-verdict");
    late->mutable_conclude()->set_id("later-verdict");
    *late->mutable_conclude()->mutable_plan() = late_binding;
    const auto retrospective = call(late_conclusion).project().conclusion();
    EXPECT_TRUE(retrospective.retrospective());
    auto replace_plan = accept;
    *replace_plan.mutable_submit()->mutable_research_plan() = late_binding;
    EXPECT_THROW(call(replace_plan), std::runtime_error);
    EXPECT_EQ(
        call(conclude).project().conclusion().SerializeAsString(),
        original_verdict.SerializeAsString());
    research::proto::ResearchRequest deprecate;
    auto& removal = *deprecate.mutable_strategy()->mutable_mutation();
    removal.set_id("deprecate-package");
    *removal.mutable_retire()->mutable_package() = protocol::strategy_package_object(candidate);
    removal.mutable_retire()->set_reason("No new use; keep actual admitted source");
    call(deprecate);
    const auto held = call(retirement).retirement();
    EXPECT_EQ(held.state(), task_wire::TaskRetirement::BLOCKED);
    EXPECT_NE(std::ranges::find(held.consumers(), draft.target_task_id()), held.consumers().end());
    service.reset();
    data_service.reset();
    start();
    EXPECT_EQ(call(accept).task().state(), task_wire::SUCCEEDED);
    EXPECT_EQ(call(accept).task().research_plan().SerializeAsString(), binding.SerializeAsString());
    EXPECT_EQ(
        call(conclude).project().conclusion().SerializeAsString(),
        original_verdict.SerializeAsString());
    EXPECT_TRUE(call(late_conclusion).project().conclusion().retrospective());

    auto changed = accept;
    changed.mutable_submit()->mutable_backtest()->mutable_deposit()->set_units(
        Decimal::parse("200000").raw());
    EXPECT_THROW(call(changed), std::runtime_error);
    EXPECT_EQ(call(report).backtest().SerializeAsString(), actual.backtest().SerializeAsString());
    EXPECT_EQ(
        call(report).result_task().strategy_use().SerializeAsString(),
        actual.result_task().strategy_use().SerializeAsString());
    EXPECT_TRUE(call(lookup).strategy().target_accepted());
}

TEST_F(TaskProcess, StrategyCandidateDraftAndSourceHoldPersistAcrossResearchRestart)
{
    const auto original = process_input();
    research::proto::ResearchRequest create;
    create.mutable_submit()->set_id("strategy-reference");
    auto& spec = *create.mutable_submit()->mutable_backtest();
    const auto& contract = original.paper().contracts(0);
    auto& source_contract = *spec.add_contracts();
    *source_contract.mutable_data() = protocol::bar_dataset_selection(contract.dataset());
    *source_contract.mutable_cost_schedule() = contract.cost_schedule();
    source_contract.set_slippage_ticks(contract.slippage_ticks());
    *spec.mutable_deposit() = original.paper().deposit();
    *spec.mutable_risk() = original.paper().risk();
    *spec.mutable_strategies() = original.strategies();
    ASSERT_EQ(call(create).task().state(), task_wire::QUEUED);
    const auto reference_state = testing_support::wait_task(
        [&](auto request) {
            return call(request);
        },
        "strategy-reference");
    ASSERT_EQ(reference_state.state(), task_wire::SUCCEEDED) << reference_state.error();
    research::proto::ResearchRequest result;
    result.mutable_result()->set_id("strategy-reference");
    const auto evidence = call(result);
    research::proto::ResearchRequest freeze;
    auto& mutation = *freeze.mutable_strategy()->mutable_mutation();
    mutation.set_id("freeze-candidate");
    auto& registration = *mutation.mutable_freeze();
    registration.set_id("candidate");
    registration.set_name("Synthetic single CTA");
    registration.set_purpose("Explicit fixed-input research delivery");
    registration.set_limitations("Synthetic evidence, no predictive or trading authorization");
    auto& source = *registration.mutable_source();
    source.mutable_owner()->set_node_id("fixture-node");
    source.mutable_owner()->set_service_id("task");
    source.mutable_owner()->set_domain("research");
    source.set_kind("backtest_result");
    source.set_id("strategy-reference");
    source.set_version(evidence.result_task().result_digest());
    const auto candidate = call(freeze).strategy().package();
    EXPECT_EQ(
        candidate.source_input_sha256(),
        sha256_bytes(evidence.result_task().input().SerializeAsString()));
    EXPECT_EQ(candidate.definition().program_artifact(), fixture_program_artifact);
    EXPECT_EQ(candidate.retirement_reason(), "");
    EXPECT_EQ(call(freeze).strategy().package().SerializeAsString(), candidate.SerializeAsString());
    auto changed = freeze;
    changed.mutable_strategy()->mutable_mutation()->mutable_freeze()->set_purpose("changed");
    EXPECT_THROW(call(changed), std::runtime_error);
    research::proto::ResearchRequest retirement;
    retirement.mutable_retirement()->set_id("strategy-reference");
    retirement.mutable_retirement()->set_execute(true);
    const auto held = call(retirement).retirement();
    ASSERT_EQ(held.state(), task_wire::TaskRetirement::BLOCKED);
    ASSERT_EQ(held.consumers_size(), 1);
    EXPECT_EQ(held.consumers(0), "strategy_package/candidate");
    research::proto::ResearchRequest deliver;
    auto& creation = *deliver.mutable_strategy()->mutable_mutation();
    creation.set_id("create-delivery");
    creation.mutable_deliver()->set_id("delivery");
    creation.mutable_deliver()->set_target_task_id("explicit-future-run");
    *creation.mutable_deliver()->mutable_package() = protocol::strategy_package_object(candidate);
    const auto draft = call(deliver).strategy().delivery();
    research::proto::ResearchRequest lookup;
    lookup.mutable_strategy()->mutable_delivery()->set_id("delivery");
    EXPECT_FALSE(call(lookup).strategy().target_accepted());
    research::proto::ResearchRequest missing;
    missing.mutable_get()->set_id("explicit-future-run");
    EXPECT_THROW(call(missing), std::runtime_error);
    research::proto::ResearchRequest catalogue;
    catalogue.mutable_resources()->set_kind("strategy_package");
    catalogue.mutable_resources()->set_limit(10);
    EXPECT_EQ(call(catalogue).resources().items_size(), 1);
    research::proto::ResearchRequest accept;
    auto& submitted = *accept.mutable_submit();
    submitted.set_id(draft.target_task_id());
    *submitted.mutable_strategy_use()->mutable_package() = draft.package();
    *submitted.mutable_strategy_use()->mutable_delivery() =
        protocol::strategy_delivery_object(draft);
    auto& fixed = *submitted.mutable_backtest_reuse();
    fixed.set_choice(backtest::proto::BacktestReuse::FIXED);
    fixed.set_source_id(draft.reference_result().id());
    fixed.set_source_result_digest(draft.reference_result().version());
    fixed.set_source_input_sha256(draft.source_input_sha256());
    auto altered = accept;
    altered.mutable_submit()->mutable_backtest_reuse()->set_slippage_ticks(2);
    EXPECT_THROW(call(altered), std::runtime_error);
    auto wrong_source = accept;
    wrong_source.mutable_submit()->mutable_backtest_reuse()->set_source_input_sha256(
        std::string(64, 'f'));
    EXPECT_THROW(call(wrong_source), std::runtime_error);
    const auto admitted = call(accept).task();
    EXPECT_EQ(
        admitted.strategy_use().SerializeAsString(),
        submitted.strategy_use().SerializeAsString());
    EXPECT_TRUE(call(lookup).strategy().target_accepted());
    ASSERT_EQ(
        testing_support::wait_task(
            [&](auto request) {
                return call(request);
            },
            draft.target_task_id())
            .state(),
        task_wire::SUCCEEDED);
    result.mutable_result()->set_id(draft.target_task_id());
    const auto used = call(result);
    EXPECT_EQ(
        used.result_task().strategy_use().SerializeAsString(),
        submitted.strategy_use().SerializeAsString());
    EXPECT_EQ(
        used.result_task().input().SerializeAsString(),
        evidence.result_task().input().SerializeAsString());
    service.reset();
    data_service.reset();
    start();
    EXPECT_TRUE(call(lookup).strategy().target_accepted());
    EXPECT_EQ(call(accept).task().state(), task_wire::SUCCEEDED);
    EXPECT_EQ(call(freeze).strategy().package().SerializeAsString(), candidate.SerializeAsString());
    EXPECT_EQ(call(lookup).strategy().delivery().SerializeAsString(), draft.SerializeAsString());
    EXPECT_TRUE(call(lookup).strategy().target_accepted());
    EXPECT_EQ(call(retirement).retirement().state(), task_wire::TaskRetirement::BLOCKED);
    research::proto::ResearchRequest deprecate;
    auto& removal = *deprecate.mutable_strategy()->mutable_mutation();
    removal.set_id("deprecate-candidate");
    *removal.mutable_retire()->mutable_package() = protocol::strategy_package_object(candidate);
    removal.mutable_retire()->set_reason("Research candidate withdrawn");
    EXPECT_EQ(
        call(deprecate).strategy().package().retirement_reason(),
        "Research candidate withdrawn");
    retirement.mutable_retirement()->set_execute(false);
    const auto retained = call(retirement).retirement();
    EXPECT_EQ(retained.state(), task_wire::TaskRetirement::BLOCKED);
    ASSERT_EQ(retained.consumers_size(), 1);
    EXPECT_EQ(retained.consumers(0), draft.target_task_id());
    EXPECT_EQ(call(accept).task().state(), task_wire::SUCCEEDED);
    EXPECT_EQ(
        call(result).result_task().strategy_use().SerializeAsString(),
        submitted.strategy_use().SerializeAsString());
    EXPECT_EQ(call(freeze).strategy().package().SerializeAsString(), candidate.SerializeAsString());
    EXPECT_EQ(call(lookup).strategy().delivery().SerializeAsString(), draft.SerializeAsString());
    auto forbidden = deliver;
    auto* next = forbidden.mutable_strategy()->mutable_mutation();
    next->set_id("new-delivery");
    next->mutable_deliver()->set_id("forbidden-delivery");
    next->mutable_deliver()->set_target_task_id("forbidden-run");
    EXPECT_THROW(call(forbidden), std::runtime_error);
}

TEST_F(TaskProcess, CausalVariablesPersistDefinitionsAndActualFactorEvidenceAcrossRestart)
{
    EXPECT_FALSE(data_service);
    research::proto::ResearchRequest preview;
    auto& input = *preview.mutable_variable()->mutable_preview();
    input.set_expression("forward_return");
    input.set_window(2);
    const auto invalid = call(preview).variable().preview();
    EXPECT_FALSE(invalid.valid());
    ASSERT_TRUE(invalid.has_error_offset());
    EXPECT_EQ(invalid.error_offset(), 0U);
    EXPECT_EQ(invalid.diagnostic(), "unknown field or function");
    input.set_expression("close / lag(close,window) - 1");
    const auto checked = call(preview).variable().preview();
    EXPECT_TRUE(checked.valid());
    EXPECT_EQ(checked.required_history(), 2U);
    EXPECT_EQ(checked.price_power(), 0);
    service.reset();
    {
        sqlite::Database database(directory.path / "tasks.sqlite");
        sqlite::Database::Statement tables(
            database,
            "SELECT count(*) FROM sqlite_master WHERE name LIKE 'research_variable_%'");
        ASSERT_TRUE(tables.step());
        EXPECT_EQ(tables.integer(0), 0);
    }
    start();
    research::proto::ResearchRequest registration;
    auto& mutation = *registration.mutable_variable()->mutable_mutation();
    mutation.set_id("variable-operation-one");
    auto& definition = *mutation.mutable_registration();
    definition.set_id("variable-one");
    definition.set_name("A declared variable");
    definition.set_purpose("Research predictor after a completed close, not a future label");
    definition.set_expression(input.expression());
    definition.set_window(2);
    const auto original = call(registration).variable().definition();
    EXPECT_EQ(original.required_history(), 2U);
    EXPECT_EQ(original.owner().node_id(), "fixture-node");
    EXPECT_EQ(original.owner().service_id(), "task");
    auto next = registration;
    next.mutable_variable()->mutable_mutation()->set_id("variable-operation-two");
    next.mutable_variable()->mutable_mutation()->mutable_registration()->set_id("variable-two");
    next.mutable_variable()->mutable_mutation()->mutable_registration()->set_window(3);
    const auto second = call(next).variable().definition();
    EXPECT_NE(second.digest(), original.digest());
    EXPECT_EQ(second.name(), original.name());
    EXPECT_EQ(
        call(registration).variable().definition().SerializeAsString(),
        original.SerializeAsString());
    auto changed = registration;
    changed.mutable_variable()->mutable_mutation()->mutable_registration()->set_window(3);
    EXPECT_THROW(call(changed), std::runtime_error);
    research::proto::ResearchRequest page;
    page.mutable_variable()->mutable_list()->set_limit(1);
    const auto first_page = call(page).variable();
    ASSERT_EQ(first_page.definitions_size(), 1);
    EXPECT_EQ(first_page.definitions(0).id(), "variable-one");
    EXPECT_EQ(first_page.next_after(), "variable-one");
    page.mutable_variable()->mutable_list()->set_after(first_page.next_after());
    const auto second_page = call(page).variable();
    ASSERT_EQ(second_page.definitions_size(), 1);
    EXPECT_EQ(second_page.definitions(0).id(), "variable-two");
    EXPECT_TRUE(second_page.next_after().empty());
    service.reset();
    start();
    research::proto::ResearchRequest get;
    get.mutable_variable()->mutable_get()->set_id(original.id());
    EXPECT_EQ(call(get).variable().definition().SerializeAsString(), original.SerializeAsString());
    EXPECT_EQ(
        call(registration).variable().definition().SerializeAsString(),
        original.SerializeAsString());
    research::proto::ResearchRequest resources;
    resources.mutable_resources()->set_kind("research_variable");
    resources.mutable_resources()->set_limit(10);
    const auto catalogue = call(resources).resources();
    ASSERT_EQ(catalogue.items_size(), 2);
    EXPECT_EQ(catalogue.owner().SerializeAsString(), original.owner().SerializeAsString());
    EXPECT_EQ(catalogue.items(0).object().version(), original.digest());
    EXPECT_FALSE(data_service);
    auto spec = maximum_factor_input();
    std::vector<int> prices(40);
    for (int i = 0; i < 40; ++i)
        prices[static_cast<std::size_t>(i)] = 100 + i + i % 3;
    prices[0] = 100;
    prices[1] = 102;
    prices[2] = 101;
    prices[3] = 103;
    *factor_bars(spec) = published_dataset(prices, "expression-data");
    spec.mutable_data_owner()->set_node_id("fixture-node");
    spec.mutable_data_owner()->set_service_id("historical-data");
    spec.set_dataset_revision(protocol::factor_revision(spec.series()));
    spec.set_factor(protocol::proto::FACTOR_UNSPECIFIED);
    spec.set_retain_observations(true);
    spec.set_horizon(1);
    spec.mutable_lookbacks()->Clear();
    spec.add_lookbacks(original.window());
    *spec.mutable_variable_definition() = original;
    task_wire::Task fixed;
    fixed.set_kind(task_wire::FACTOR);
    *fixed.mutable_factor() = spec;
    research::proto::ResearchRequest submit;
    auto& admission = *submit.mutable_submit();
    admission.set_id("expression-original");
    *admission.mutable_factor_request() = protocol::task_execution(fixed, "").factor();
    *admission.mutable_data_owner() = spec.data_owner();
    const auto admitted = call(submit, {}, 10s).task();
    EXPECT_EQ(
        admitted.variable_definition().SerializeAsString(),
        protocol::variable_object(original).SerializeAsString());
    const auto finished = testing_support::wait_task(
        [&](auto request) {
            return call(request);
        },
        "expression-original");
    ASSERT_EQ(finished.state(), task_wire::SUCCEEDED) << finished.error();
    research::proto::ResearchRequest result;
    result.mutable_result()->set_id("expression-original");
    const auto evidence = call(result);
    EXPECT_EQ(
        evidence.result_task().factor().variable_definition().SerializeAsString(),
        original.SerializeAsString());
    EXPECT_EQ(evidence.result_task().program_artifact(), fixture_program_artifact);
    ASSERT_EQ(evidence.factor().observations_size(), 40);
    EXPECT_NEAR(evidence.factor().observations(2).value(), 0.01, 1e-12);
    EXPECT_FALSE(evidence.factor().observations(1).has_value());
    EXPECT_FALSE(evidence.factor().observations(39).has_forward_return());
    auto revision = registration;
    revision.mutable_variable()->mutable_mutation()->set_id("variable-operation-three");
    revision.mutable_variable()->mutable_mutation()->mutable_registration()->set_id(
        "variable-three");
    revision.mutable_variable()->mutable_mutation()->mutable_registration()->set_window(4);
    const auto third = call(revision).variable().definition();
    EXPECT_EQ(third.name(), original.name());
    EXPECT_NE(third.digest(), original.digest());
    auto duplicate = submit;
    auto& changed_factor = *duplicate.mutable_submit()->mutable_factor_request();
    *changed_factor.mutable_variable_definition() = protocol::variable_object(third);
    changed_factor.mutable_lookbacks()->Clear();
    changed_factor.add_lookbacks(4);
    EXPECT_THROW(call(duplicate), std::runtime_error);
    auto wrong_definition = submit;
    wrong_definition.mutable_submit()->set_id("expression-wrong");
    wrong_definition.mutable_submit()
        ->mutable_factor_request()
        ->mutable_variable_definition()
        ->set_version(std::string(64, 'f'));
    EXPECT_THROW(call(wrong_definition), std::runtime_error);
    EXPECT_EQ(call(result).factor().SerializeAsString(), evidence.factor().SerializeAsString());
    for (const bool missing : {false, true}) {
        const std::string id =
            missing ? "expression-prepared-missing" : "expression-prepared-present";
        service.reset();
        {
            tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
            auto prepared = task_submission(store, id, spec);
            asterion::tasks::set_program_artifact(prepared, fixture_program_artifact);
            store.admit_submission(prepared);
            prepared.prepare_files();
            const auto captured = store.prepared_submission_record(id);
            EXPECT_EQ(
                captured.execution().variable_definition().SerializeAsString(),
                original.SerializeAsString());
            EXPECT_EQ(
                captured.record().task().variable_definition().SerializeAsString(),
                protocol::variable_object(original).SerializeAsString());
            if (missing)
                std::filesystem::remove(directory.path / id / "input.pb");
        }
        start();
        research::proto::ResearchRequest recover;
        recover.mutable_recover_submission()->set_id(id);
        EXPECT_EQ(
            call(recover, {}, 10s).task().variable_definition().SerializeAsString(),
            protocol::variable_object(original).SerializeAsString());
        const auto recovered = testing_support::wait_task(
            [&](auto request) {
                return call(request);
            },
            id);
        ASSERT_EQ(recovered.state(), task_wire::SUCCEEDED) << recovered.error();
        research::proto::ResearchRequest read;
        read.mutable_result()->set_id(id);
        const auto recovered_result = call(read);
        EXPECT_EQ(
            recovered_result.result_task().factor().variable_definition().SerializeAsString(),
            original.SerializeAsString());
        EXPECT_EQ(
            recovered_result.factor().SerializeAsString(),
            evidence.factor().SerializeAsString());
    }
    data_service.reset();
    service.reset();
    start();
    const auto replay = call(result);
    EXPECT_EQ(
        replay.result_task().factor().variable_definition().SerializeAsString(),
        original.SerializeAsString());
    EXPECT_EQ(replay.factor().SerializeAsString(), evidence.factor().SerializeAsString());
    EXPECT_EQ(replay.result_task().program_artifact(), fixture_program_artifact);
    EXPECT_EQ(call(get).variable().definition().SerializeAsString(), original.SerializeAsString());
    EXPECT_FALSE(data_service);
}

TEST_F(TaskProcess, RawSamplesUseFixedDataWithoutAnyFactorAndRestoreMissingPreparation)
{
    const auto dataset = published_dataset({100, -1, 0, 2, -2, 4}, "raw-sample-source");
    research::proto::ResearchRequest submit;
    auto& selected = *submit.mutable_submit();
    selected.set_id("raw-sample");
    selected.mutable_data_owner()->set_node_id("fixture-node");
    selected.mutable_data_owner()->set_service_id("historical-data");
    auto& request = *selected.mutable_sample();
    *request.mutable_selection() = protocol::bar_dataset_selection(dataset);
    request.set_use_scope("Synthetic raw close exploration, not historical eligibility proof");
    request.set_mode(research::proto::SampleRequest::POST_HOC);
    request.set_development_end(4);
    const auto accepted = call(submit, {}, 10s).task();
    EXPECT_EQ(accepted.kind(), task_wire::SAMPLE);
    const auto complete = testing_support::wait_task(
        [&](auto value) {
            return call(value);
        },
        "raw-sample");
    ASSERT_EQ(complete.state(), task_wire::SUCCEEDED) << complete.error();
    research::proto::ResearchRequest read;
    read.mutable_result()->set_id("raw-sample");
    const auto evidence = call(read);
    ASSERT_TRUE(evidence.has_sample());
    EXPECT_EQ(evidence.result_task().program_artifact(), fixture_program_artifact);
    ASSERT_EQ(evidence.sample().rows_size(), 6);
    EXPECT_EQ(evidence.sample().rows(3).role(), research::proto::SampleRow::DEVELOPMENT);
    EXPECT_EQ(evidence.sample().rows(4).role(), research::proto::SampleRow::HOLDOUT);
    const auto owned = evidence.result_task().sample();
    EXPECT_EQ(owned.dataset().bars(1).close().units(), -100000000LL);
    EXPECT_EQ(owned.dataset().bars(2).close().units(), 0);
    EXPECT_EQ(owned.request().mode(), research::proto::SampleRequest::POST_HOC);
    resource::proto::Object consumer;
    *consumer.mutable_owner() = owned.owner();
    consumer.set_kind("sample");
    consumer.set_id("raw-sample");
    consumer.set_version(sha256_bytes(owned.SerializeAsString()));
    protocol::DataClient data(endpoint + ".data", "historical-data");
    data::proto::DataRequest hold;
    *hold.mutable_retention()->mutable_get() = consumer;
    const auto retained = data.call(hold).retention();
    EXPECT_NE(retained.phase(), data::proto::DataRetentionRecord::RELEASED);
    ASSERT_EQ(retained.retention().versions_size(), 2);
    EXPECT_EQ(retained.retention().consumer().SerializeAsString(), consumer.SerializeAsString());
    service.reset();
    {
        tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
        auto preparation = task_submission(store, "raw-prepared", owned);
        asterion::tasks::set_program_artifact(preparation, fixture_program_artifact);
        store.admit_submission(preparation);
        preparation.prepare_files();
        EXPECT_LT(
            store.prepared_submission_record("raw-prepared").execution().ByteSizeLong(),
            owned.ByteSizeLong());
        std::filesystem::remove(directory.path / "raw-prepared/input.pb");
    }
    start();
    research::proto::ResearchRequest recover;
    recover.mutable_recover_submission()->set_id("raw-prepared");
    EXPECT_EQ(call(recover, {}, 10s).task().kind(), task_wire::SAMPLE);
    const auto recovered = testing_support::wait_task(
        [&](auto value) {
            return call(value);
        },
        "raw-prepared");
    ASSERT_EQ(recovered.state(), task_wire::SUCCEEDED) << recovered.error();
    research::proto::ResearchRequest recovered_read;
    recovered_read.mutable_result()->set_id("raw-prepared");
    EXPECT_EQ(
        call(recovered_read).sample().SerializeAsString(),
        evidence.sample().SerializeAsString());
    data_service.reset();
    service.reset();
    start();
    EXPECT_FALSE(data_service);
    EXPECT_EQ(call(submit).task().submission_sequence(), accepted.submission_sequence());
    auto changed = submit;
    changed.mutable_submit()->mutable_sample()->set_use_scope("different exploration");
    EXPECT_THROW(call(changed), std::runtime_error);
    EXPECT_EQ(call(read).sample().SerializeAsString(), evidence.sample().SerializeAsString());
    EXPECT_EQ(
        call(recovered_read).sample().SerializeAsString(),
        evidence.sample().SerializeAsString());
    EXPECT_EQ(call(read).result_task().sample().SerializeAsString(), owned.SerializeAsString());
    research::proto::ResearchRequest statistics;
    statistics.mutable_submit()->set_id("raw-statistics");
    auto& selection = *statistics.mutable_submit()->mutable_statistics();
    *selection.mutable_source()->mutable_owner() = owned.owner();
    selection.mutable_source()->set_kind("sample_result");
    selection.mutable_source()->set_id("raw-sample");
    selection.mutable_source()->set_version(evidence.result_task().result_digest());
    selection.set_column(research::proto::StatisticsRequest::RAW_CLOSE);
    selection.set_role(research::proto::StatisticsRequest::DEVELOPMENT);
    selection.set_use_scope("Synthetic original raw-close distribution with Data offline");
    selection.set_end_index(6);
    selection.set_bins(3);
    selection.set_grouping(research::proto::StatisticsRequest::TRADING_DAY);
    call(statistics, {}, 10s);
    const auto analysed = testing_support::wait_task(
        [&](auto value) {
            return call(value);
        },
        "raw-statistics");
    ASSERT_EQ(analysed.state(), task_wire::SUCCEEDED) << analysed.error();
    research::proto::ResearchRequest statistics_read;
    statistics_read.mutable_result()->set_id("raw-statistics");
    const auto report = call(statistics_read);
    EXPECT_EQ(report.statistics().summary().valid(), 4U);
    EXPECT_DOUBLE_EQ(report.statistics().summary().mean().value(), 25.25);
    ASSERT_EQ(report.statistics().groups_size(), 1);
    EXPECT_EQ(report.statistics().groups(0).trading_day(), owned.dataset().bars(0).trading_day());
    EXPECT_EQ(report.statistics().rows(0).trading_day(), owned.dataset().bars(0).trading_day());
    EXPECT_DOUBLE_EQ(report.statistics().groups(0).summary().mean().value(), 25.25);
    auto changed_grouping = statistics;
    changed_grouping.mutable_submit()->mutable_statistics()->set_grouping(
        research::proto::StatisticsRequest::NONE);
    EXPECT_THROW(call(changed_grouping), std::runtime_error);
    EXPECT_DOUBLE_EQ(report.statistics().summary().minimum().value(), -1);
    EXPECT_DOUBLE_EQ(report.statistics().rows(2).value(), 0);
    EXPECT_FALSE(report.statistics().rows(4).has_value());
    EXPECT_EQ(report.statistics().price_power(), 1);
    auto wrong_column = statistics;
    wrong_column.mutable_submit()->mutable_statistics()->set_column(
        research::proto::StatisticsRequest::FORWARD_RETURN);
    EXPECT_THROW(call(wrong_column), std::runtime_error);
    research::proto::ResearchRequest retention;
    retention.mutable_retirement()->set_id("raw-sample");
    EXPECT_EQ(call(retention).retirement().consumers_size(), 1);
    service.reset();
    start();
    EXPECT_FALSE(data_service);
    EXPECT_EQ(
        call(statistics_read).statistics().SerializeAsString(),
        report.statistics().SerializeAsString());
    EXPECT_EQ(call(read).sample().SerializeAsString(), evidence.sample().SerializeAsString());
}

TEST_F(TaskProcess, StatisticsConsumeOriginalFactorWithoutDataAndRecoverExactPreparedInput)
{
    auto spec = maximum_factor_input();
    std::vector<int> prices(90);
    for (int i = 0; i < 90; ++i)
        prices[static_cast<std::size_t>(i)] = 100 + i + i % 3;
    *factor_bars(spec) = published_dataset(prices, "statistics-source-data");
    spec.mutable_data_owner()->set_node_id("fixture-node");
    spec.mutable_data_owner()->set_service_id("historical-data");
    spec.set_dataset_revision(protocol::factor_revision(spec.series()));
    spec.set_factor(protocol::proto::PRICE_MOMENTUM);
    spec.set_retain_observations(true);
    spec.set_horizon(3);
    spec.set_holdout_start(45);
    spec.mutable_lookbacks()->Clear();
    spec.add_lookbacks(2);
    task_wire::Task fixed;
    fixed.set_kind(task_wire::FACTOR);
    *fixed.mutable_factor() = spec;
    research::proto::ResearchRequest source_submit;
    source_submit.mutable_submit()->set_id("statistics-source");
    *source_submit.mutable_submit()->mutable_factor_request() =
        protocol::task_execution(fixed, "").factor();
    *source_submit.mutable_submit()->mutable_data_owner() = spec.data_owner();
    call(source_submit, {}, 10s);
    const auto original = testing_support::wait_task(
        [&](auto request) {
            return call(request);
        },
        "statistics-source");
    ASSERT_EQ(original.state(), task_wire::SUCCEEDED) << original.error();
    research::proto::ResearchRequest source_read;
    source_read.mutable_result()->set_id("statistics-source");
    const auto source = call(source_read);
    data_service.reset();
    research::proto::ResearchRequest submit;
    submit.mutable_submit()->set_id("statistics-report");
    auto& selected = *submit.mutable_submit()->mutable_statistics();
    auto& object = *selected.mutable_source();
    object.mutable_owner()->set_node_id("fixture-node");
    object.mutable_owner()->set_service_id("task");
    object.mutable_owner()->set_domain("research");
    object.set_kind("factor_result");
    object.set_id("statistics-source");
    object.set_version(source.result_task().result_digest());
    selected.set_use_scope("Synthetic original development feature distribution");
    selected.set_column(research::proto::StatisticsRequest::FEATURE);
    selected.set_role(research::proto::StatisticsRequest::DEVELOPMENT);
    selected.set_end_index(90);
    selected.set_bins(8);
    const auto admitted = call(submit, {}, 10s).task();
    const auto done = testing_support::wait_task(
        [&](auto request) {
            return call(request);
        },
        "statistics-report");
    ASSERT_EQ(done.state(), task_wire::SUCCEEDED) << done.error();
    EXPECT_EQ(call(submit).task().submission_sequence(), admitted.submission_sequence());
    auto changed = submit;
    changed.mutable_submit()->mutable_statistics()->set_bins(4);
    EXPECT_THROW(call(changed), std::runtime_error);
    research::proto::ResearchRequest read;
    read.mutable_result()->set_id("statistics-report");
    const auto report = call(read);
    ASSERT_TRUE(report.has_statistics());
    EXPECT_EQ(report.statistics().summary().valid(), 43U);
    EXPECT_EQ(report.statistics().summary().allowed(), 45U);
    EXPECT_EQ(report.result_task().program_artifact(), fixture_program_artifact);
    EXPECT_EQ(report.statistics().source().SerializeAsString(), object.SerializeAsString());
    service.reset();
    {
        tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
        EXPECT_EQ(store.retirement("statistics-source", "fixture-node").consumers_size(), 1);
        const auto captured = store.get("statistics-report").statistics();
        auto prepared = task_submission(store, "statistics-prepared", captured);
        asterion::tasks::set_program_artifact(prepared, fixture_program_artifact);
        store.admit_submission(prepared);
        prepared.prepare_files();
        std::filesystem::remove(directory.path / "statistics-prepared/input.pb");
    }
    start();
    EXPECT_FALSE(data_service);
    research::proto::ResearchRequest recover;
    recover.mutable_recover_submission()->set_id("statistics-prepared");
    EXPECT_EQ(call(recover, {}, 10s).task().kind(), task_wire::STATISTICS);
    const auto recovered = testing_support::wait_task(
        [&](auto request) {
            return call(request);
        },
        "statistics-prepared");
    ASSERT_EQ(recovered.state(), task_wire::SUCCEEDED) << recovered.error();
    research::proto::ResearchRequest recovered_read;
    recovered_read.mutable_result()->set_id("statistics-prepared");
    EXPECT_EQ(
        call(recovered_read).statistics().SerializeAsString(),
        report.statistics().SerializeAsString());
    EXPECT_EQ(call(read).statistics().SerializeAsString(), report.statistics().SerializeAsString());
    EXPECT_EQ(call(source_read).factor().SerializeAsString(), source.factor().SerializeAsString());
    research::proto::ResearchRequest retirement;
    retirement.mutable_retirement()->set_id("statistics-report");
    retirement.mutable_retirement()->set_execute(true);
    const auto retired = call(retirement).retirement();
    EXPECT_EQ(retired.state(), task_wire::TaskRetirement::RETIRED);
    EXPECT_TRUE(retired.source_release_confirmed());
    EXPECT_EQ(call(read).statistics().SerializeAsString(), report.statistics().SerializeAsString());
    retirement.mutable_retirement()->set_id("statistics-source");
    retirement.mutable_retirement()->set_execute(false);
    EXPECT_EQ(call(retirement).retirement().consumers_size(), 1);
}

TEST_F(TaskProcess, FixedExpressionHoldoutRecoversOriginalRolesAndValuesWithoutData)
{
    research::proto::ResearchRequest registration;
    auto& mutation = *registration.mutable_variable()->mutable_mutation();
    mutation.set_id("holdout-variable-register");
    auto& definition = *mutation.mutable_registration();
    definition.set_id("holdout-variable");
    definition.set_name("Synthetic causal holdout predictor");
    definition.set_purpose("Fixed definition, development and holdout labels remain separate");
    definition.set_expression("close / lag(close,window) - 1");
    definition.set_window(2);
    const auto original = call(registration).variable().definition();
    auto spec = maximum_factor_input();
    std::vector<int> prices(90);
    for (int i = 0; i < 90; ++i)
        prices[static_cast<std::size_t>(i)] = 100 + i + i % 3;
    *factor_bars(spec) = published_dataset(prices, "expression-holdout-data");
    spec.mutable_data_owner()->set_node_id("fixture-node");
    spec.mutable_data_owner()->set_service_id("historical-data");
    spec.set_dataset_revision(protocol::factor_revision(spec.series()));
    spec.set_factor(protocol::proto::FACTOR_UNSPECIFIED);
    spec.set_retain_observations(true);
    spec.set_horizon(3);
    spec.set_holdout_start(45);
    spec.mutable_lookbacks()->Clear();
    spec.add_lookbacks(2);
    *spec.mutable_variable_definition() = original;
    task_wire::Task fixed;
    fixed.set_kind(task_wire::FACTOR);
    *fixed.mutable_factor() = spec;
    research::proto::ResearchRequest submit;
    auto& admission = *submit.mutable_submit();
    admission.set_id("expression-held");
    *admission.mutable_factor_request() = protocol::task_execution(fixed, "").factor();
    *admission.mutable_data_owner() = spec.data_owner();
    const auto accepted = call(submit, {}, 10s).task();
    const auto finished = testing_support::wait_task(
        [&](auto request) {
            return call(request);
        },
        "expression-held");
    ASSERT_EQ(finished.state(), task_wire::SUCCEEDED) << finished.error();
    EXPECT_EQ(call(submit).task().submission_sequence(), accepted.submission_sequence());
    research::proto::ResearchRequest read;
    read.mutable_result()->set_id("expression-held");
    const auto evidence = call(read);
    EXPECT_EQ(evidence.result_task().program_artifact(), fixture_program_artifact);
    EXPECT_EQ(
        evidence.result_task().factor().variable_definition().SerializeAsString(),
        original.SerializeAsString());
    EXPECT_EQ(evidence.result_task().factor().holdout_start(), 45U);
    ASSERT_EQ(evidence.factor().partitions_size(), 2);
    EXPECT_EQ(evidence.factor().partitions(0).sample_count(), 40U);
    EXPECT_EQ(evidence.factor().partitions(1).sample_count(), 42U);
    EXPECT_EQ(evidence.factor().purged_count(), 3U);
    EXPECT_TRUE(evidence.factor().observations(42).label_crosses_boundary());
    EXPECT_FALSE(evidence.factor().observations(42).included());
    EXPECT_EQ(evidence.factor().observations(45).role(), factor::proto::FactorObservation::HOLDOUT);
    EXPECT_FALSE(evidence.factor().observations(89).has_forward_return());
    service.reset();
    {
        tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
        auto prepared = task_submission(store, "expression-held-prepared", spec);
        asterion::tasks::set_program_artifact(prepared, fixture_program_artifact);
        store.admit_submission(prepared);
        prepared.prepare_files();
        EXPECT_EQ(
            store.prepared_submission_record("expression-held-prepared")
                .execution()
                .factor()
                .holdout_start(),
            45U);
        std::filesystem::remove(directory.path / "expression-held-prepared/input.pb");
    }
    start();
    research::proto::ResearchRequest recover;
    recover.mutable_recover_submission()->set_id("expression-held-prepared");
    EXPECT_EQ(call(recover, {}, 10s).task().variable_definition().version(), original.digest());
    const auto recovered = testing_support::wait_task(
        [&](auto request) {
            return call(request);
        },
        "expression-held-prepared");
    ASSERT_EQ(recovered.state(), task_wire::SUCCEEDED) << recovered.error();
    research::proto::ResearchRequest recovered_read;
    recovered_read.mutable_result()->set_id("expression-held-prepared");
    EXPECT_EQ(
        call(recovered_read).factor().SerializeAsString(),
        evidence.factor().SerializeAsString());
    data_service.reset();
    service.reset();
    start();
    EXPECT_FALSE(data_service);
    EXPECT_EQ(call(read).factor().SerializeAsString(), evidence.factor().SerializeAsString());
    const auto replay = call(recovered_read);
    EXPECT_EQ(replay.factor().SerializeAsString(), evidence.factor().SerializeAsString());
    EXPECT_EQ(
        replay.result_task().factor().variable_definition().SerializeAsString(),
        original.SerializeAsString());
    EXPECT_EQ(replay.result_task().program_artifact(), fixture_program_artifact);
}

TEST_F(
    TaskProcess,
    ResearchProjectsPersistPlansAndOriginalReceiptsWithoutDataOrClientAcknowledgement)
{
    EXPECT_FALSE(data_service);
    research::proto::ResearchRequest save;
    auto& command = *save.mutable_project()->mutable_mutation();
    command.set_id("project-save-one");
    auto& draft = *command.mutable_save();
    draft.set_project_id("research-project");
    auto& definition = *draft.mutable_definition();
    definition.set_title("Synthetic research question");
    definition.set_question("Does a fixed factor relate to later returns?");
    definition.set_hypothesis("An explicitly unproven relation");
    definition.set_support("Declared direction and adequate independent observations");
    definition.set_refutation("Opposite direction or inadequate evidence");
    research::proto::ResearchRequest get;
    get.mutable_project()->mutable_draft()->set_id("research-project");
    research::proto::ProjectDraft first;
    {
        auto disconnected = ipc::Channel::connect(endpoint, 2s);
        auto sent = save;
        sent.set_version(1);
        sent.set_service_id("task");
        sent.set_correlation_id(unique_process_id());
        disconnected.send(sent.SerializeAsString(), 2s);
        const auto until = std::chrono::steady_clock::now() + 5s;
        for (;;) {
            try {
                first = call(get).project().draft();
                break;
            } catch (const std::exception&) {
                if (std::chrono::steady_clock::now() >= until)
                    throw;
            }
            std::this_thread::sleep_for(20ms);
        }
        // The independent query proves commit happened; never consume its ACK.
    }
    EXPECT_EQ(call(save).project().draft().SerializeAsString(), first.SerializeAsString());
    EXPECT_EQ(first.owner().service_id(), "task");
    EXPECT_EQ(first.owner().domain(), "research");
    research::proto::ResearchRequest fix;
    auto& plan_command = *fix.mutable_project()->mutable_mutation();
    plan_command.set_id("project-fix-one");
    auto& planned = *plan_command.mutable_fix();
    planned.set_project_id(first.id());
    planned.set_plan_id("plan-one");
    planned.set_expected_draft_digest(first.digest());
    const auto original_plan = call(fix).project().plan();
    EXPECT_FALSE(original_plan.exploratory());
    auto edit = save;
    edit.mutable_project()->mutable_mutation()->set_id("project-save-two");
    auto& change = *edit.mutable_project()->mutable_mutation()->mutable_save();
    change.set_expected_digest(first.digest());
    change.mutable_definition()->set_support("A revised standard for subsequent attempts");
    const auto revised_draft = call(edit).project().draft();
    auto revise = fix;
    revise.mutable_project()->mutable_mutation()->set_id("project-fix-two");
    auto& revised = *revise.mutable_project()->mutable_mutation()->mutable_fix();
    revised.set_plan_id("plan-two");
    revised.set_expected_draft_digest(revised_draft.digest());
    revised.set_parent_plan_id(original_plan.id());
    revised.set_revision_reason("Retain original standards and explain the revision");
    const auto revised_plan = call(revise).project().plan();
    EXPECT_NE(revised_plan.definition_digest(), original_plan.definition_digest());
    research::proto::ResearchRequest list;
    auto& query = *list.mutable_project()->mutable_plans();
    query.set_project_id(first.id());
    query.set_limit(1);
    const auto page = call(list).project();
    ASSERT_EQ(page.plans_size(), 1);
    EXPECT_FALSE(page.next_after().empty());
    query.set_after(page.next_after());
    EXPECT_EQ(call(list).project().plans_size(), 1);
    service.reset();
    start();
    research::proto::ResearchRequest read_plan;
    read_plan.mutable_project()->mutable_plan()->set_id(original_plan.id());
    EXPECT_EQ(
        call(read_plan).project().plan().SerializeAsString(),
        original_plan.SerializeAsString());
    EXPECT_EQ(call(save).project().draft().SerializeAsString(), first.SerializeAsString());
    EXPECT_EQ(call(get).project().draft().SerializeAsString(), revised_draft.SerializeAsString());
    EXPECT_FALSE(data_service);
    research::proto::ResearchRequest heartbeat;
    heartbeat.mutable_heartbeat();
    EXPECT_TRUE(call(heartbeat).health().initialized());
}

TEST_F(
    TaskProcess,
    FixedResearchPlansSurviveCriteriaChangesPreparationRecoveryAndQueuedCancellation)
{
    research::proto::ResearchRequest save;
    auto& mutation = *save.mutable_project()->mutable_mutation();
    mutation.set_id("plan-save-one");
    auto& draft = *mutation.mutable_save();
    draft.set_project_id("factor-question");
    draft.mutable_definition()->set_title("Synthetic factor question");
    draft.mutable_definition()->set_question("Does this fixed factor relate to later returns?");
    draft.mutable_definition()->set_hypothesis("A hypothesis, not an established fact");
    draft.mutable_definition()->set_support("The declared criterion for this original attempt");
    draft.mutable_definition()->set_refutation("Opposite direction or insufficient evidence");
    const auto first_draft = call(save).project().draft();
    research::proto::ResearchRequest fix;
    auto& command = *fix.mutable_project()->mutable_mutation();
    command.set_id("plan-fix-one");
    command.mutable_fix()->set_project_id(first_draft.id());
    command.mutable_fix()->set_plan_id("original-plan");
    command.mutable_fix()->set_expected_draft_digest(first_draft.digest());
    const auto first_plan = call(fix).project().plan();
    research::proto::ProjectBinding binding;
    *binding.mutable_owner() = first_plan.owner();
    binding.set_project_id(first_plan.project_id());
    binding.set_plan_id(first_plan.id());
    binding.set_definition_digest(first_plan.definition_digest());
    auto spec = maximum_factor_input();
    std::vector<int> prices(40);
    for (int i = 0; i < 40; ++i)
        prices[static_cast<std::size_t>(i)] = 100 + i + i % 3;
    *factor_bars(spec) = published_dataset(prices, "project-factor-data");
    spec.mutable_data_owner()->set_node_id("fixture-node");
    spec.mutable_data_owner()->set_service_id("historical-data");
    spec.set_dataset_revision(protocol::factor_revision(spec.series()));
    task_wire::Task fixed;
    *fixed.mutable_factor() = spec;
    research::proto::ResearchRequest submit;
    auto& admission = *submit.mutable_submit();
    admission.set_id("planned-factor");
    *admission.mutable_factor_request() = protocol::task_execution(fixed, "").factor();
    *admission.mutable_data_owner() = spec.data_owner();
    *admission.mutable_research_plan() = binding;
    EXPECT_EQ(call(submit).task().research_plan().SerializeAsString(), binding.SerializeAsString());
    const auto finished = testing_support::wait_task(
        [&](auto request) {
            return call(request);
        },
        "planned-factor");
    ASSERT_EQ(finished.state(), task_wire::SUCCEEDED) << finished.error();
    research::proto::ResearchRequest result;
    result.mutable_result()->set_id("planned-factor");
    const auto original = call(result);
    EXPECT_EQ(
        original.result_task().research_plan().SerializeAsString(),
        binding.SerializeAsString());
    research::proto::ResearchRequest conclusions;
    auto* listing = conclusions.mutable_project()->mutable_conclusions();
    listing->set_project_id(binding.project_id());
    listing->set_limit(10);
    EXPECT_TRUE(call(conclusions).project().conclusions().empty()); // Statistics create no verdict.
    research::proto::ResearchRequest conclude;
    auto* action = conclude.mutable_project()->mutable_mutation();
    action->set_id("manual-conclusion-command");
    auto* declaration = action->mutable_conclude();
    declaration->set_id("manual-conclusion");
    *declaration->mutable_plan() = binding;
    auto* evidence = declaration->mutable_result();
    *evidence->mutable_owner() = binding.owner();
    evidence->set_kind("factor_result");
    evidence->set_id("planned-factor");
    evidence->set_version(original.result_task().result_digest());
    declaration->set_judgment(research::proto::INSUFFICIENT);
    declaration->set_reason("Human review: synthetic evidence alone establishes no market claim");
    const auto human = call(conclude).project().conclusion();
    EXPECT_FALSE(human.retrospective());
    EXPECT_EQ(human.program_artifact(), original.result_task().program_artifact());
    EXPECT_EQ(human.input_sha256().size(), 64U);
    EXPECT_EQ(human.declaration().judgment(), research::proto::INSUFFICIENT);
    EXPECT_EQ(call(conclude).project().conclusion().SerializeAsString(), human.SerializeAsString());
    auto changed_command = conclude;
    changed_command.mutable_project()->mutable_mutation()->mutable_conclude()->set_judgment(
        research::proto::SUPPORTED);
    EXPECT_THROW(call(changed_command), std::runtime_error);
    auto edit = save;
    edit.mutable_project()->mutable_mutation()->set_id("plan-save-two");
    edit.mutable_project()->mutable_mutation()->mutable_save()->set_expected_digest(
        first_draft.digest());
    edit.mutable_project()->mutable_mutation()->mutable_save()->mutable_definition()->set_support(
        "Changed only for subsequent attempts");
    const auto changed = call(edit).project().draft();
    auto revise = fix;
    revise.mutable_project()->mutable_mutation()->set_id("plan-fix-two");
    auto& revision = *revise.mutable_project()->mutable_mutation()->mutable_fix();
    revision.set_plan_id("revised-plan");
    revision.set_expected_draft_digest(changed.digest());
    revision.set_parent_plan_id(first_plan.id());
    revision.set_revision_reason("Preserve original criteria while revising later work");
    const auto revised = call(revise).project().plan();
    auto retrospective = conclude;
    auto* retrospective_action = retrospective.mutable_project()->mutable_mutation();
    retrospective_action->set_id("retrospective-command");
    auto* retrospective_declaration = retrospective_action->mutable_conclude();
    retrospective_declaration->set_id("retrospective-conclusion");
    retrospective_declaration->mutable_plan()->set_plan_id(revised.id());
    retrospective_declaration->mutable_plan()->set_definition_digest(revised.definition_digest());
    EXPECT_TRUE(call(retrospective).project().conclusion().retrospective());
    auto correction = conclude;
    auto* correction_action = correction.mutable_project()->mutable_mutation();
    correction_action->set_id("manual-correction-command");
    correction_action->mutable_conclude()->set_id("manual-correction");
    correction_action->mutable_conclude()->set_previous_id(human.declaration().id());
    EXPECT_THROW(call(correction), std::runtime_error); // Missing human revision reason.
    correction_action->mutable_conclude()->set_revision_reason(
        "Clarify the limits without replacing the first judgement");
    correction_action->mutable_conclude()->set_reason(
        "Human clarification: no out-of-sample market claim");
    const auto clarified = call(correction).project().conclusion();
    EXPECT_EQ(clarified.declaration().previous_id(), human.declaration().id());
    EXPECT_EQ(call(conclusions).project().conclusions_size(), 3);
    auto wrong_result = conclude;
    wrong_result.mutable_project()->mutable_mutation()->set_id("wrong-conclusion-command");
    wrong_result.mutable_project()->mutable_mutation()->mutable_conclude()->set_id(
        "wrong-conclusion");
    wrong_result.mutable_project()
        ->mutable_mutation()
        ->mutable_conclude()
        ->mutable_result()
        ->set_version(std::string(64, 'a'));
    EXPECT_THROW(call(wrong_result), std::runtime_error);
    auto duplicate = submit;
    duplicate.mutable_submit()->mutable_research_plan()->set_plan_id(revised.id());
    duplicate.mutable_submit()->mutable_research_plan()->set_definition_digest(
        revised.definition_digest());
    try {
        (void)call(duplicate);
        FAIL() << "same task id accepted a different plan";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("task research plan changed"), std::string::npos);
    }
    EXPECT_EQ(call(result).result_task().result_digest(), original.result_task().result_digest());
    for (const bool missing : {false, true}) {
        const std::string id = missing ? "plan-prepared-missing" : "plan-prepared-present";
        service.reset();
        {
            tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
            auto prepared = task_submission(store, id, spec);
            asterion::tasks::set_program_artifact(prepared, fixture_program_artifact);
            asterion::tasks::set_research_plan(store, prepared, binding);
            store.admit_submission(prepared);
            prepared.prepare_files();
            const auto record = store.prepared_submission_record(id);
            EXPECT_EQ(
                record.record().task().research_plan().SerializeAsString(),
                binding.SerializeAsString());
            EXPECT_EQ(
                record.execution().research_plan().SerializeAsString(),
                binding.SerializeAsString());
        }
        if (missing)
            std::filesystem::remove(
                directory.path / id / "input.pb"); // Only this synthetic fixture.
        start();
        research::proto::ResearchRequest recover;
        recover.mutable_recover_submission()->set_id(id);
        EXPECT_EQ(
            call(recover).task().research_plan().SerializeAsString(),
            binding.SerializeAsString());
        const auto recovered = testing_support::wait_task(
            [&](auto request) {
                return call(request);
            },
            id);
        ASSERT_EQ(recovered.state(), task_wire::SUCCEEDED) << recovered.error();
        EXPECT_EQ(recovered.research_plan().SerializeAsString(), binding.SerializeAsString());
    }
    service.reset();
    data_service.reset();
    {
        tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
        auto pending = task_submission(store, "cancelled-plan", spec);
        asterion::tasks::set_program_artifact(pending, fixture_program_artifact);
        asterion::tasks::set_research_plan(store, pending, binding);
        store.admit_submission(pending);
        pending.prepare_files();
        store.commit(store.register_submission(pending));
        auto unrelated = task_submission(store, "unrelated-factor", spec);
        asterion::tasks::set_program_artifact(unrelated, fixture_program_artifact);
        store.admit_submission(unrelated);
        unrelated.prepare_files();
        store.commit(store.register_submission(unrelated));
    }
    start();
    research::proto::ResearchRequest cancel;
    cancel.mutable_cancel()->set_id("cancelled-plan");
    const auto cancelled = call(cancel).task();
    EXPECT_EQ(cancelled.state(), task_wire::CANCELLED);
    EXPECT_TRUE(cancelled.result_digest().empty());
    EXPECT_EQ(cancelled.research_plan().SerializeAsString(), binding.SerializeAsString());
    research::proto::ResearchRequest project_page;
    project_page.mutable_project_tasks()->set_project_id(binding.project_id());
    project_page.mutable_project_tasks()->set_limit(1);
    std::set<std::string> seen;
    for (unsigned pages = 0; pages < 6; ++pages) {
        const auto page = call(project_page).project_tasks();
        for (const auto& task : page.tasks()) {
            EXPECT_TRUE(seen.insert(task.id()).second);
            EXPECT_NE(task.id(), "unrelated-factor");
            EXPECT_EQ(task.research_plan().SerializeAsString(), binding.SerializeAsString());
        }
        if (!page.next_before_sequence())
            break;
        project_page.mutable_project_tasks()->set_before_sequence(page.next_before_sequence());
    }
    EXPECT_EQ(seen.size(), 4U);
    EXPECT_TRUE(seen.contains("cancelled-plan"));
    const auto replay = call(result);
    EXPECT_EQ(
        replay.result_task().research_plan().SerializeAsString(),
        binding.SerializeAsString());
    EXPECT_EQ(replay.result_task().program_artifact(), original.result_task().program_artifact());
    EXPECT_EQ(replay.factor().SerializeAsString(), original.factor().SerializeAsString());
    research::proto::ResearchRequest read_conclusion;
    read_conclusion.mutable_project()->mutable_conclusion()->set_id(human.declaration().id());
    EXPECT_EQ(
        call(read_conclusion).project().conclusion().SerializeAsString(),
        human.SerializeAsString());
    EXPECT_EQ(call(conclusions).project().conclusions_size(), 3);
    EXPECT_EQ(call(conclude).project().conclusion().SerializeAsString(), human.SerializeAsString());
    for (const auto& [kind, count] : std::vector<std::pair<std::string, unsigned>>{
             {"research_project", 1},
             {"research_plan", 2},
             {"research_conclusion", 3}
         }) {
        research::proto::ResearchRequest catalogue;
        auto& page = *catalogue.mutable_resources();
        page.set_kind(kind);
        page.set_limit(1);
        std::set<std::string> ids;
        for (unsigned page_number = 0; page_number <= count; ++page_number) {
            const auto result = call(catalogue).resources();
            EXPECT_EQ(result.owner().SerializeAsString(), binding.owner().SerializeAsString());
            EXPECT_EQ(result.kind(), kind);
            ASSERT_EQ(result.items_size(), 1);
            const auto& row = result.items(0);
            ASSERT_TRUE(ids.insert(row.object().id()).second);
            EXPECT_EQ(
                row.object().owner().SerializeAsString(),
                binding.owner().SerializeAsString());
            EXPECT_EQ(row.object().kind(), kind);
            EXPECT_EQ(row.object().version().size(), 64U);
            EXPECT_EQ(row.relative_location(), "tasks.sqlite");
            EXPECT_EQ(
                row.state(),
                kind == "research_project" ? resource::proto::CatalogueEntry::REGISTERED
                                           : resource::proto::CatalogueEntry::SEALED);
            if (result.next_after().empty())
                break;
            page.set_after(result.next_after());
        }
        EXPECT_EQ(ids.size(), count);
    }
    research::proto::ResearchRequest read_plan;
    read_plan.mutable_project()->mutable_plan()->set_id(first_plan.id());
    EXPECT_EQ(call(read_plan).project().plan().SerializeAsString(), first_plan.SerializeAsString());
}

TEST_F(TaskProcess, ServiceOwnsConcurrencyCancellationAndUpgradeDrain)
{
    service.reset();
    replace_file_durably(sockets / "research.json", R"({"task_concurrency":1})");
    auto spec = maximum_factor_input();
    *factor_bars(spec) =
        published_dataset(std::vector<int>(protocol::max_dataset_bars, 100), "pool-input");
    spec.set_dataset_revision(spec.series(0).bars().revision());
    {
        tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
        persist(store, "first", spec);
        persist(store, "second", spec);
    }
    start();
    research::proto::ResearchRequest get;
    get.mutable_metadata_get()->set_id("first");
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (call(get).task().state() == task_wire::QUEUED) {
        ASSERT_LT(std::chrono::steady_clock::now(), deadline);
        std::this_thread::sleep_for(10ms);
    }
    ASSERT_EQ(call(get).task().state(), task_wire::RUNNING);
    get.mutable_metadata_get()->set_id("second");
    ASSERT_EQ(call(get).task().state(), task_wire::QUEUED);
    research::proto::ResearchRequest freeze;
    freeze.mutable_quiesce();
    ASSERT_TRUE(call(freeze, endpoint + ".health").has_health());
    research::proto::ResearchRequest stop;
    stop.mutable_quiesce()->set_stop(true);
    EXPECT_THROW(call(stop, endpoint + ".health"), std::runtime_error);
    research::proto::ResearchRequest cancel;
    cancel.mutable_cancel()->set_id("first");
    ASSERT_EQ(call(cancel).task().state(), task_wire::CANCEL_REQUESTED);
    EXPECT_EQ(
        testing_support::wait_task(
            [&](auto request) {
                return call(request);
            },
            "first")
            .state(),
        task_wire::CANCELLED);
    EXPECT_EQ(call(get).task().state(), task_wire::QUEUED);
    ASSERT_TRUE(call(stop, endpoint + ".health").has_health());
    ASSERT_TRUE(service->wait(10s));
    ASSERT_EQ(service->exit_code(), 0);
    start();
    EXPECT_EQ(
        testing_support::wait_task(
            [&](auto request) {
                return call(request);
            },
            "second")
            .state(),
        task_wire::SUCCEEDED);
}

TEST_F(TaskProcess, MaximumFactorInputCompletesAndRestartedResultReadKeepsControlsAvailable)
{
    service.reset();
    auto spec = maximum_factor_input();
    *factor_bars(spec) =
        published_dataset(std::vector<int>(protocol::max_dataset_bars, 100), "maximum");
    spec.set_dataset_revision(spec.series(0).bars().revision());
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        persist(store, "maximum", spec);
        persist(store, "other", input());
    }
    start(testing_support::bound(10s));
    const auto completed = testing_support::wait_task(
        [&](auto request) {
            const auto begin = std::chrono::steady_clock::now();
            auto response = call(request);
            EXPECT_LT(std::chrono::steady_clock::now() - begin, testing_support::bound(1s));
            return response;
        },
        "maximum");
    ASSERT_EQ(completed.state(), task_wire::SUCCEEDED) << completed.error();
    service.reset();
    start(testing_support::bound(10s));
    // The first read after restart validates saved result evidence without running the engine.
    research::proto::ResearchRequest request;
    request.mutable_result()->set_id("maximum");
    auto pending = std::async(std::launch::async, [&] {
        return call(request, "", protocol::task_verification_timeout + 5s);
    });
    unsigned observations = 0;
    std::chrono::steady_clock::duration longest{};
    while (pending.wait_for(10ms) != std::future_status::ready) {
        research::proto::ResearchRequest list;
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
    RecordProperty(
        "factor_json_bytes",
        protocol::decode_factor_result(spec, response.factor()).dump().size());
    research::proto::ResearchRequest cancel;
    cancel.mutable_cancel()->set_id("other");
    EXPECT_TRUE(call(cancel).has_task());
    RecordProperty("concurrent_status_observations", observations);
    RecordProperty(
        "longest_status_ms",
        std::chrono::duration_cast<std::chrono::milliseconds>(longest).count());
}

TEST(TaskStore, RejectsMismatchedResultContractAndMetrics)
{
    TaskDirectory directory;
    tasks::Store store(asterion::research::task_domain(), directory.path, tasks::Identity{"task"});
    tasks::submit(store, "job", input());
    const auto token = store.commit(store.claim("job")).token();
    auto result = backtest::run(input());
    result.mutable_account()->mutable_contracts(0)->mutable_contract()->set_symbol("rb2611");
    EXPECT_THROW(store.commit(tasks::finish(store, "job", token, result)), std::invalid_argument);
    result = backtest::run(input());
    result.mutable_max_drawdown()->set_units(0);
    EXPECT_THROW(store.commit(tasks::finish(store, "job", token, result)), std::invalid_argument);
    EXPECT_EQ(store.get("job").state(), task::proto::RUNNING);
    EXPECT_FALSE(std::filesystem::exists(directory.path / "job" / "results" / "1.pb"));
    store.commit(tasks::finish(store, "job", token, backtest::run(input())));
}

TEST(TaskStore, RejectsResultWithDifferentRiskConfiguration)
{
    TaskDirectory directory;
    tasks::Store store(asterion::research::task_domain(), directory.path, tasks::Identity{"task"});
    const auto spec = input();
    tasks::submit(store, "risk-result", spec);
    const auto token = store.commit(store.claim("risk-result")).token();
    auto result = backtest::run(spec);
    result.mutable_account()->mutable_risk()->set_max_working_orders(999);
    EXPECT_THROW(
        store.commit(tasks::finish(store, "risk-result", token, result)),
        std::invalid_argument);
    EXPECT_EQ(store.get("risk-result").state(), task::proto::RUNNING);
    store.commit(tasks::finish(store, "risk-result", token, backtest::run(spec)));
    EXPECT_EQ(
        tasks::result(store, "risk-result").account().risk().SerializeAsString(),
        spec.paper().risk().SerializeAsString());
}

namespace {

struct TaskClock final : Clock
{
    Nanoseconds value = 1790298000000000000LL;

    Nanoseconds utc_now() const override
    {
        return value;
    }

    Nanoseconds monotonic_now() const override
    {
        return 0;
    }
};

} // namespace

TEST(TaskStore, SubmissionOrderSurvivesEqualTimesClockRollbackAndRestart)
{
    TaskDirectory directory;
    auto clock = std::make_shared<TaskClock>();
    task::proto::Task first, latest;
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"},
            clock);
        first = tasks::submit(store, "z-first", input());
        tasks::submit(store, "a-second", input()); // Same millisecond, opposite ID order.
        clock->value -= 60000000000LL;
        latest = tasks::submit(store, "m-latest", input());
        EXPECT_LT(latest.submitted_at_ms(), first.submitted_at_ms());
        ASSERT_EQ(store.list().tasks_size(), 3);
        EXPECT_EQ(store.list().tasks(0).id(), "z-first");
        EXPECT_EQ(store.list().tasks(1).id(), "a-second");
        EXPECT_EQ(store.list().tasks(2).id(), "m-latest");
        EXPECT_EQ(
            tasks::submit(store, "z-first", input()).SerializeAsString(),
            first.SerializeAsString());
        EXPECT_EQ(latest.submission_sequence(), 3U);
    }
    tasks::Store restored(
        asterion::research::task_domain(),
        directory.path,
        tasks::Identity{"task"},
        clock);
    EXPECT_EQ(restored.get("z-first").SerializeAsString(), first.SerializeAsString());
    EXPECT_EQ(restored.get("m-latest").SerializeAsString(), latest.SerializeAsString());
    EXPECT_EQ(restored.list().tasks(2).id(), "m-latest");
    EXPECT_EQ(tasks::submit(restored, "b-next", input()).submission_sequence(), 4U);
    EXPECT_EQ(restored.list().tasks(3).id(), "b-next");
}

TEST(TaskStore, MetadataUpdatesPreserveLargeImmutableInputsAcrossRecovery)
{
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
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
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
            std::set<std::string> allowance;
        }
        RecordProperty(
            "metadata_128_polls_us",
            std::to_string(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - begin)
                    .count()));
        RecordProperty("immutable_input_bytes", std::to_string(original.size()));
        auto token = store.commit(store.claim("large")).token();
        for (unsigned completed = 1; completed <= 50; ++completed)
            store.commit(store.progress("large", token, completed));
        EXPECT_EQ(store.list().tasks(0).completed(), 50U);
        std::set<std::string> allowance;

        EXPECT_EQ(
            store.commit(store.cancel("large")).task().state(),
            task::proto::CANCEL_REQUESTED);
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
    tasks::Store restored(
        asterion::research::task_domain(),
        directory.path,
        tasks::Identity{"task"});
    const auto recovered = restored.get("large");
    EXPECT_EQ(recovered.state(), task::proto::INTERRUPTED);
    EXPECT_EQ(recovered.attempt(), 3U);
    EXPECT_EQ(recovered.completed(), 55U);
    EXPECT_EQ(recovered.input().SerializeAsString(), original);
    EXPECT_EQ(
        restored.commit(restored.retry("large")).task().definition_case(),
        task::proto::Task::DEFINITION_NOT_SET);
    EXPECT_EQ(restored.get("large").input().SerializeAsString(), original);
    const auto queued = restored.list().tasks(0);
    EXPECT_EQ(restored.list().capacity().retained_tasks(), 1U);
    EXPECT_EQ(queued.state(), task::proto::QUEUED);
    EXPECT_EQ(queued.total(), 10000U);
    EXPECT_EQ(queued.completed(), 0U);
    EXPECT_TRUE(queued.error().empty());
    EXPECT_EQ(queued.definition_case(), task::proto::Task::DEFINITION_NOT_SET);
}

TEST(TaskStore, BinaryPayloadsRemainCompactAndDefinitionsLoadOnDemand)
{
    TaskDirectory directory;
    const auto spec = input();
    const auto original = spec.SerializeAsString();
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
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
    tasks::Store store(asterion::research::task_domain(), directory.path, tasks::Identity{"task"});
    // Metadata operations never read or retain the input. Explicit input reads
    // and worker claims must still reject missing or changed owned bytes.
    std::filesystem::remove(path);
    EXPECT_EQ(store.list().tasks(0).total(), 7U);
    EXPECT_EQ(store.commit(store.cancel("binary")).task().state(), task::proto::CANCELLED);
    EXPECT_EQ(
        store.commit(store.retry("binary")).task().definition_case(),
        task::proto::Task::DEFINITION_NOT_SET);
    EXPECT_THROW(store.get("binary"), std::invalid_argument);
    EXPECT_THROW(store.commit(store.claim("binary")).token(), std::invalid_argument);
    EXPECT_EQ(store.describe("binary").state(), task::proto::QUEUED);
    write_file_durably(path, original);
    EXPECT_EQ(store.get("binary").input().SerializeAsString(), original);
    const auto expected = backtest::run(spec);
    store.commit(
        tasks::finish(store, "binary", store.commit(store.claim("binary")).token(), expected));
    EXPECT_EQ(
        std::filesystem::file_size(directory.path / "binary/results/1.pb"),
        expected.ByteSizeLong());
    EXPECT_EQ(tasks::result(store, "binary").SerializeAsString(), expected.SerializeAsString());
}

TEST(TaskStore, InvalidPayloadOrPreviousManifestIsRejectedWithoutRewritingFiles)
{
    // Opening does not rescan payloads; reading the fixed input rejects a damaged one.
    for (const auto& mode : {"missing", "truncated", "symlink", "protobuf", "old"}) {
        SCOPED_TRACE(mode);
        TaskDirectory directory;
        {
            tasks::Store store(
                asterion::research::task_domain(),
                directory.path,
                tasks::Identity{"task"});
            tasks::submit(
                store,
                "first",
                input()); // Same digest exercises restored metadata reuse.
            tasks::submit(store, "preserve", input());
            (void)store.commit(store.claim("preserve")).token();
        }
        const auto path = directory.path / "preserve/input.pb";
        const std::string kind = mode;
        if (kind == "missing")
            std::filesystem::remove(path);
        else if (kind == "truncated")
            std::filesystem::resize_file(path, 1);
        else if (kind == "symlink") {
            std::filesystem::rename(path, directory.path / "preserve/other.pb");
            std::filesystem::create_symlink("other.pb", path);
        } else {
            sqlite::Database db(directory.path / "tasks.sqlite");
            Json manifest;
            {
                sqlite::Database::Statement read(
                    db,
                    "SELECT manifest FROM tasks WHERE id='preserve'");
                ASSERT_TRUE(read.step());
                manifest = Json::parse(read.text(0));
            }

            // A matching digest cannot make malformed Protobuf a valid definition.
            write_file_durably(path, "\xff");
            manifest["input_bytes"] = 1;
            manifest["input_sha256"] = sha256_bytes("\xff");
            sqlite::Database::Statement update(
                db,
                "UPDATE tasks SET manifest=? WHERE id='preserve'");
            update.bind(1, manifest.dump()).step();
        }
        const auto input_hash =
            std::filesystem::is_regular_file(path) && !std::filesystem::is_symlink(path)
            ? sha256_file(path)
            : std::string{};
        EXPECT_THROW(
            {
                tasks::Store store(
                    asterion::research::task_domain(),
                    directory.path,
                    tasks::Identity{"task"});
                (void)store.get("preserve");
            },
            std::exception);
        EXPECT_THROW(
            tasks::Store::inspect_history_usage(
                asterion::research::task_domain(),
                directory.path,
                tasks::Identity{"task"},
                std::string(64, 'a')),
            std::exception);
        if (!input_hash.empty())
            EXPECT_EQ(sha256_file(path), input_hash);
        if (kind == "missing")
            EXPECT_FALSE(std::filesystem::exists(path));
        if (kind == "symlink")
            EXPECT_TRUE(std::filesystem::is_symlink(path));
    }
}

TEST(TaskStore, BacktestBeyondTwentyThousandBarsHasReadableResult)
{
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
    tasks::Store store(asterion::research::task_domain(), directory.path, tasks::Identity{"task"});
    tasks::submit(store, "beyond-old-limit", spec);
    store.commit(
        tasks::finish(
            store,
            "beyond-old-limit",
            store.commit(store.claim("beyond-old-limit")).token(),
            expected));
    EXPECT_EQ(
        tasks::result(store, "beyond-old-limit").SerializeAsString(),
        expected.SerializeAsString());
    auto maximum = expected.account();
    maximum.set_total(static_cast<unsigned>(protocol::max_dataset_bars));
    maximum.set_cursor(maximum.total());
    EXPECT_NO_THROW(protocol::decode_snapshot(maximum));
    maximum.set_total(maximum.total() + 1);
    EXPECT_THROW(protocol::decode_snapshot(maximum), std::invalid_argument);
}

TEST_F(TaskProcess, DistinctMaximumInputsExposeRecoveryHealthBeforeAdmissions)
{
    service.reset();
    constexpr int count = 8;
    auto spec = maximum_factor_input();
    std::vector<std::string> expected;
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        for (int i = 0; i < count; ++i) {
            auto* dataset = factor_bars(spec);
            auto* last = dataset->mutable_bars(dataset->bars_size() - 1);
            const auto units = d("100").raw() + static_cast<std::int64_t>(i) * d("1").raw();
            for (auto* price :
                {last->mutable_open(),
                    last->mutable_high(),
                    last->mutable_low(),
                    last->mutable_close()})
                price->set_units(units);
            dataset->set_revision(protocol::bar_dataset_revision(*dataset));
            spec.set_dataset_revision(dataset->revision());
            expected.push_back(dataset->revision());
            persist(store, "unique-" + std::to_string(i), spec);
        }
    }
    const auto begin = std::chrono::steady_clock::now();
    launch();
    unsigned recovering = 0;
    std::string instance;
    const auto deadline = begin + testing_support::bound(60s);
    while (std::chrono::steady_clock::now() < deadline) {
        research::proto::ResearchRequest ping;
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
        research::proto::ResearchRequest dispatch;
        dispatch.mutable_list()->set_limit(1);
        EXPECT_THROW(call(dispatch, endpoint + ".health"), std::runtime_error);
        std::this_thread::sleep_for(20ms);
    }
    ASSERT_LT(std::chrono::steady_clock::now(), deadline);
    EXPECT_GT(recovering, 0U);
    research::proto::ResearchRequest list;
    list.mutable_list()->set_limit(200);
    const auto tasks = call(list).tasks();
    ASSERT_EQ(tasks.tasks_size(), count);
    for (int i = 0; i < count; ++i) {
        EXPECT_EQ(tasks.tasks(i).state(), task::proto::QUEUED);
        EXPECT_EQ(tasks.tasks(i).attempt(), 0U);
        EXPECT_EQ(tasks.tasks(i).source_name(), expected[static_cast<std::size_t>(i)]);
        EXPECT_EQ(tasks.tasks(i).definition_case(), task::proto::Task::DEFINITION_NOT_SET);
    }
    RecordProperty("distinct_maximum_input_count", count);
    RecordProperty("recovery_heartbeats", recovering);
    RecordProperty(
        "recovery_ms",
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - begin)
            .count());
}

TEST(TaskStore, ManyCompletedMaximumInputsRestoreWithOnePayloadWorkingSet)
{
    TaskDirectory directory;
    const auto spec = maximum_factor_input();
    const auto expected = factor::run(spec);
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        tasks::submit(store, "batch-0", spec);
        store.commit(
            tasks::finish(
                store,
                "batch-0",
                store.commit(store.claim("batch-0")).token(),
                expected));
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
        task::proto::StoredTaskRecord base_record;
        {
            sqlite::Database::Statement read(
                db,
                "SELECT manifest, record FROM tasks WHERE id='batch-0'");
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
                const auto target =
                    dest / item.path().lexically_relative(directory.path / "batch-0");
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
                db,
                "INSERT INTO tasks SELECT ?1, ?2, kind, ?3, state, attempt, updated_at_ms, ?4 " "FROM tasks WHERE id='batch-0'");
            add.bind(1, id)
                .bind(2, i + 1)
                .bind(3, manifest.dump())
                .bind_blob(4, record.SerializeAsString())
                .step();
            sqlite::Database::Statement events(
                db,
                "INSERT INTO task_events SELECT ?1, sequence, body " "FROM task_events WHERE task_id='batch-0'");
            events.bind(1, id).step();
        }
        transaction.commit();
    }
    const auto start = std::chrono::steady_clock::now();
    {
        tasks::Store restored(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        const auto list = restored.list();
        EXPECT_EQ(list.tasks_size(), count);
        for (const auto& task : list.tasks()) {
            EXPECT_EQ(task.state(), task::proto::SUCCEEDED);
            EXPECT_EQ(task.total(), static_cast<unsigned>(protocol::max_dataset_bars));
            EXPECT_EQ(task.definition_case(), task::proto::Task::DEFINITION_NOT_SET);
        }
        EXPECT_EQ(restored.get("batch-23").factor().SerializeAsString(), spec.SerializeAsString());
        EXPECT_EQ(
            tasks::factor_result(restored, "batch-23").SerializeAsString(),
            expected.SerializeAsString());
        RecordProperty("completed_payload_bytes", std::to_string(input_bytes * count));
        RecordProperty("single_payload_bytes", std::to_string(input_bytes));
        RecordProperty("completed_task_count", std::to_string(count));
        RecordProperty(
            "restore_and_verify_ms",
            std::to_string(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start)
                    .count()));
    }
    // Bounded stopped-service reference inspection still fails closed, but its
    // total-I/O budget must not prevent a valid writer from reopening its store.
    EXPECT_THROW(
        tasks::Store::inspect_history_usage(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"},
            std::string(64, 'a')),
        std::invalid_argument);
}

TEST(TaskStore, DurableUpdatesDoNotChangeSubmissionIdentityOrReorderRetries)
{
    TaskDirectory directory;
    auto clock = std::make_shared<TaskClock>();
    task::proto::Task retried;
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"},
            clock);
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
    tasks::Store restored(
        asterion::research::task_domain(),
        directory.path,
        tasks::Identity{"task"},
        clock);
    EXPECT_EQ(restored.describe("first").SerializeAsString(), retried.SerializeAsString());
}

TEST(TaskStore, RejectsMissingOrDuplicateChronologyWithoutRewritingEvidence)
{
    for (const auto& mode : {"missing", "duplicate", "gap", "negative", "overflow"}) {
        SCOPED_TRACE(mode);
        TaskDirectory directory;
        {
            tasks::Store store(
                asterion::research::task_domain(),
                directory.path,
                tasks::Identity{"task"});
            tasks::submit(store, "first", input());
            tasks::submit(store, "second", input());
        }
        const auto manifest = [&] {
            sqlite::Database database(directory.path / "tasks.sqlite");
            sqlite::Database::Statement read(
                database,
                "SELECT manifest FROM tasks WHERE id='second'");
            EXPECT_TRUE(read.step());
            return Json::parse(read.text(0));
        };
        auto record = manifest();
        const std::string kind = mode;
        if (kind == "missing")
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
            sqlite::Database::Statement write(
                database,
                "UPDATE tasks SET manifest=? WHERE id='second'");
            write.bind(1, record.dump()).step();
        }
        EXPECT_THROW(
            tasks::Store rejected(
                asterion::research::task_domain(),
                directory.path,
                tasks::Identity{"task"}),
            std::exception);
        EXPECT_EQ(manifest(), record);
    }
}

TEST_F(TaskProcess, SilentExternalPeerDoesNotBlockQueriesAndCancellation)
{
    submit("cancel-me");
    auto silent = ipc::Channel::connect(endpoint, 2s);
    research::proto::ResearchRequest cancel;
    cancel.mutable_cancel()->set_id("cancel-me");
    EXPECT_EQ(call(cancel).task().state(), task_wire::CANCELLED);
}

TEST(TaskStore, CompletedResultCarriesPersistedExperimentAndRejectsMismatchedEvidence)
{
    TaskDirectory directory;
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        tasks::submit(store, "evidence", input());
        store.commit(
            tasks::finish(
                store,
                "evidence",
                store.commit(store.claim("evidence")).token(),
                backtest::run(input())));
    }
    tasks::Store restored(
        asterion::research::task_domain(),
        directory.path,
        tasks::Identity{"task"});
    task::proto::TaskResponse response;
    *response.mutable_backtest() = tasks::result(restored, "evidence");
    *response.mutable_result_task() = restored.get("evidence");
    const auto value = protocol::decode_task_result(response, "evidence");
    EXPECT_EQ(
        value.at("experiment").at("strategies").at(0),
        protocol::decode_strategy(testing_support::moving_average(1, 3)));
    EXPECT_EQ(value.at("experiment").at("paper").at("deposit"), "10000");
    EXPECT_EQ(
        value.at("experiment")
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
    bad.mutable_result_task()->set_state(task::proto::RUNNING);
    EXPECT_THROW(protocol::decode_task_result(bad, "evidence"), std::invalid_argument);
    bad = response;
    bad.mutable_backtest()->set_dataset_revision(std::string(64, '0'));
    EXPECT_THROW(protocol::decode_task_result(bad, "evidence"), std::invalid_argument);
    bad = response;
    bad.mutable_result_task()->clear_definition();
    EXPECT_THROW(protocol::decode_task_result(bad, "evidence"), std::invalid_argument);
    bad = response;
    bad.mutable_result_task()->set_kind(task::proto::FACTOR);
    EXPECT_THROW(protocol::decode_task_result(bad, "evidence"), std::invalid_argument);
}

TEST(Backtest, ExplicitTradingDaysDoNotInferFromWallClock)
{
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

backtest::proto::BacktestInput multiday_input()
{
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
            {tick->mutable_open(),
                tick->mutable_high(),
                tick->mutable_low(),
                tick->mutable_close()})
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

TEST(Backtest, MultidaySettlementCarriesSmaAndChargesYesterdayClose)
{
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
    EXPECT_EQ(result.account().orders(1).offset(), protocol::proto::CLOSE_YESTERDAY);
    ASSERT_EQ(result.equity_size(), 10);
    EXPECT_EQ(result.equity(4).event(), backtest::proto::DAILY_SETTLEMENT);
    EXPECT_EQ(
        result.equity(4).timestamp_ns(),
        spec.paper().contracts(0).dataset().bars(3).timestamp_ns());
    EXPECT_EQ(result.equity(5).event(), backtest::proto::TRADE_MARK);
    EXPECT_EQ(result.equity(9).equity().units(), result.account().equity().units());
    EXPECT_EQ(result.SerializeAsString(), backtest::run(spec).SerializeAsString());
}

TEST(Backtest, FinalSettlementRevaluesOpenPositionAndParticipatesInDrawdown)
{
    auto spec = multiday_input();
    spec.mutable_paper()->mutable_contracts(0)->mutable_dataset()->mutable_days()->DeleteSubrange(
        1,
        1);
    spec.mutable_paper()->mutable_contracts(0)->mutable_dataset()->mutable_bars()->DeleteSubrange(
        4,
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
    EXPECT_EQ(result.equity(4).event(), backtest::proto::DAILY_SETTLEMENT);
}

TEST(Backtest, MultidayRequiresExplicitValidCompleteEvidence)
{
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

    // A bar needs a day: what is traded is settled.
    spec = original;
    spec.mutable_paper()->mutable_contracts(0)->mutable_dataset()->mutable_days()->RemoveLast();
    spec.mutable_paper()->mutable_contracts(0)->mutable_dataset()->set_revision(
        protocol::bar_dataset_revision(spec.paper().contracts(0).dataset()));
    spec.set_dataset_revision(spec.paper().contracts(0).dataset().revision());
    EXPECT_THROW(backtest::run(spec), std::invalid_argument);
}

// A day needs no bars. The contract settles on the second day without
// trading: the lot bought on the first is held over it and settled at its
// price, when its session closes.
TEST(Backtest, ADayWithoutBarsIsSettled)
{
    auto spec = multiday_input();
    spec.mutable_paper()->mutable_contracts(0)->mutable_dataset()->mutable_bars()->DeleteSubrange(
        4,
        4);
    spec.mutable_paper()->mutable_contracts(0)->mutable_dataset()->set_revision(
        protocol::bar_dataset_revision(spec.paper().contracts(0).dataset()));
    spec.set_dataset_revision(spec.paper().contracts(0).dataset().revision());
    const auto result = backtest::run(spec);
    ASSERT_EQ(result.settlements_size(), 2);
    EXPECT_EQ(result.settlements(0).equity().units(), d("10038").raw());
    EXPECT_EQ(result.settlements(1).trading_day(), "2026-09-28");
    EXPECT_EQ(result.settlements(1).timestamp_ns(), parse_shanghai_time("2026-09-28 15:00:00"));
    EXPECT_EQ(result.settlements(1).contracts(0).price().units(), d("110").raw());
    EXPECT_EQ(result.settlements(1).contracts(0).position_quantity().units(), d("1").raw());
    // Five a lot of ten, from the 105 it settled at to 110.
    EXPECT_EQ(result.account().equity().units(), d("10088").raw());
    // A mark at each of the four bars and a settlement for each of the two days.
    ASSERT_EQ(result.equity_size(), 6);
    EXPECT_EQ(result.equity(5).event(), backtest::proto::DAILY_SETTLEMENT);
    EXPECT_EQ(result.equity(5).timestamp_ns(), result.settlements(1).timestamp_ns());
    // The Task service takes this result as that of the input.
    TaskDirectory root;
    tasks::Store store(asterion::research::task_domain(), root.path, tasks::Identity{"task"});
    tasks::submit(store, "idle-day", spec);
    const auto token = store.commit(store.claim("idle-day")).token();
    store.commit(tasks::finish(store, "idle-day", token, result));
    EXPECT_EQ(store.get("idle-day").state(), task::proto::SUCCEEDED);
}

TEST(TaskStore, MultidayEvidenceRestoresAndForgedSettlementCannotCommit)
{
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
        tasks::Store store(asterion::research::task_domain(), root.path, tasks::Identity{"task"});
        tasks::submit(store, "multiday", spec);
        const auto token = store.commit(store.claim("multiday")).token();
        EXPECT_THROW(
            store.commit(tasks::finish(store, "multiday", token, wrong)),
            std::invalid_argument);
        auto forged = expected;
        forged.mutable_settlements(0)->mutable_contracts(0)->mutable_price()->set_units(
            d("106").raw());
        EXPECT_THROW(
            store.commit(tasks::finish(store, "multiday", token, forged)),
            std::invalid_argument);
        forged = expected;
        forged.mutable_equity(0)->set_timestamp_ns(expected.equity(0).timestamp_ns() + 1);
        EXPECT_THROW(
            store.commit(tasks::finish(store, "multiday", token, forged)),
            std::invalid_argument);
        store.commit(tasks::finish(store, "multiday", token, expected));
    }
    tasks::Store restored(asterion::research::task_domain(), root.path, tasks::Identity{"task"});
    EXPECT_EQ(
        tasks::result(restored, "multiday").SerializeAsString(),
        expected.SerializeAsString());
    EXPECT_EQ(restored.get("multiday").input().SerializeAsString(), spec.SerializeAsString());
}

TEST(TaskStore, PinnedRiskArtifactIsRetainedButResultReadsDoNotLoadAlgorithm)
{
    TaskDirectory directory;
    std::string artifact;
    const auto expected = backtest::run(input());
    const auto file = risk_providers::Module::filename(directory.path / "done");
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        artifact = tasks::submit(store, "done", input()).risk_artifact();
        EXPECT_EQ(artifact, sha256_file(file));
        store.commit(
            tasks::finish(store, "done", store.commit(store.claim("done")).token(), expected));
        EXPECT_EQ(tasks::result(store, "done").SerializeAsString(), expected.SerializeAsString());
        const auto saved = directory.path / "original";
        std::filesystem::rename(file, saved);
        EXPECT_EQ(tasks::result(store, "done").SerializeAsString(), expected.SerializeAsString());
        EXPECT_THROW(
            risk_providers::Module::pinned(directory.path / "done", artifact),
            std::exception);
        std::ofstream(file) << "corrupt fixture";
        EXPECT_EQ(tasks::result(store, "done").SerializeAsString(), expected.SerializeAsString());
        EXPECT_THROW(
            risk_providers::Module::pinned(directory.path / "done", artifact),
            std::exception);
        EXPECT_EQ(std::filesystem::file_size(file), 15U);
        std::filesystem::remove(file);
        std::filesystem::rename(saved, file);
    }
    tasks::Store recovered(
        asterion::research::task_domain(),
        directory.path,
        tasks::Identity{"task"});
    EXPECT_EQ(recovered.get("done").risk_artifact(), artifact);
    EXPECT_EQ(tasks::result(recovered, "done").SerializeAsString(), expected.SerializeAsString());
}

#include "bar_dataset_source.hpp"
#include "data/history_fixture.hpp"

namespace {

// The days a dataset settles on without bars: the contract did not trade on
// them, or their bars are missing.
std::vector<std::string> days_without_bars(const data::proto::BarDataset& dataset)
{
    std::set<std::string> traded;
    for (const auto& bar : dataset.bars())
        traded.insert(bar.trading_day());
    std::vector<std::string> result;
    for (const auto& day : dataset.days())
        if (!traded.contains(day.trading_day()))
            result.push_back(day.trading_day());
    return result;
}

} // namespace

TEST(NamedDatasets, ASettlementDayWithoutMinuteBarsIsADayOfTheDataset)
{
    TaskDirectory directory;
    const auto selection = test::seed_history(
        directory.path,
        {100, 101, 102, 103},
        "gap",
        {"2026-09-23", "2026-09-25"},
        {"2026-09-23", "2026-09-24", "2026-09-25"});
    data::Store store(directory.path, "historical-data");
    const Json request{
        {"source_dataset_ids", selection.at("source_dataset_ids")},
        {"daily", selection.at("daily")},
        {"begin_day", ""},
        {"end_day", ""},
        {"contract", "SHFE/rb/2026-10"}
    };
    const auto sources = store.sources(protocol::encode_bar_dataset_request(request));
    const auto dataset = data::resolve_bar_dataset(store.tables(), sources);
    ASSERT_EQ(dataset.bars_size(), 4);
    // A position held over the 24th is settled on it, bars or none.
    EXPECT_EQ(dataset.days_size(), 3);
    EXPECT_EQ(days_without_bars(dataset), std::vector<std::string>{"2026-09-24"});
    EXPECT_EQ(dataset.revision(), protocol::bar_dataset_revision(dataset));
}

#include "history_coverage.hpp"

// The days a minute version is compared with are those the market-wide daily
// table shows the contract traded on: there rebar traded on all three days.
TEST(NamedDatasets, CoverageCountsTradingDaysPerContractAcrossTheArchive)
{
    TaskDirectory directory;
    const auto rebar = test::seed_history(
        directory.path,
        {100, 101, 102, 103},
        "coverage",
        {"2026-09-23", "2026-09-25"},
        {"2026-09-23", "2026-09-24", "2026-09-25"});
    // Coil's minutes run to 09-28, past the last day the table holds.
    const auto coil = test::seed_history(
        directory.path,
        {100, 101},
        "coverage-later",
        {"2026-09-25", "2026-09-28"},
        {"2026-09-25"},
        "hc");
    history_files::Archive archive(directory.path / "history");
    history_files::TableStore tables(directory.path / "tables");
    data::proto::HistoryCoverageQuery query;
    query.mutable_filter()->set_venue("SHFE");
    query.mutable_filter()->set_product("rb");
    *query.mutable_daily() = protocol::encode_daily_table(test::latest_daily({rebar, coil}));
    const auto coverage = data::history_coverage(archive, tables, query);
    ASSERT_EQ(coverage.items_size(), 1);
    const auto& row = coverage.items(0);
    EXPECT_EQ(row.contract_id(), "SHFE/rb/2026-10");
    EXPECT_EQ(row.minute_days(), 2U);
    EXPECT_EQ(row.minute_first(), "2026-09-23");
    EXPECT_EQ(row.compared_first(), "2026-09-23");
    EXPECT_EQ(row.compared_last(), "2026-09-25");
    EXPECT_EQ(row.traded_days(), 3U);
    EXPECT_EQ(row.uncovered(), 1U);
    ASSERT_EQ(row.uncovered_days_size(), 1);
    EXPECT_EQ(row.uncovered_days(0), "2026-09-24");
    // What the table does not hold yet is not compared: no day is missing.
    query.mutable_filter()->set_product("hc");
    const auto later = data::history_coverage(archive, tables, query);
    ASSERT_EQ(later.items_size(), 1);
    EXPECT_EQ(later.items(0).minute_last(), "2026-09-28");
    EXPECT_EQ(later.items(0).compared_last(), "2026-09-25");
    EXPECT_EQ(later.items(0).traded_days(), 1U);
    EXPECT_EQ(later.items(0).uncovered(), 0U);
    query.mutable_filter()->set_product("cu");
    EXPECT_EQ(data::history_coverage(archive, tables, query).items_size(), 0);
}

TEST(Backtest, PortfolioOfIdenticalContractsDoublesEveryLedgerEffect)
{
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
    const auto doubled = [](const protocol::proto::Decimal& value) {
        return value.units() * 2;
    };
    EXPECT_EQ(two.account().fees().units(), doubled(one.account().fees()));
    EXPECT_EQ(two.account().realized().units(), doubled(one.account().realized()));
    EXPECT_EQ(two.account().fills_size(), one.account().fills_size() * 2);
    ASSERT_EQ(two.settlements_size(), one.settlements_size());
    for (int day = 0; day < two.settlements_size(); ++day) {
        const auto& settled = two.settlements(day);
        ASSERT_EQ(settled.contracts_size(), 2);
        EXPECT_EQ(settled.contracts(0).symbol(), "rb2610");
        EXPECT_EQ(settled.contracts(1).symbol(), "hc2610");
        EXPECT_EQ(
            settled.contracts(1).position_quantity().units(),
            one.settlements(day).contracts(0).position_quantity().units());
        // Equity moves twice as far from the shared deposit.
        EXPECT_EQ(
            settled.equity().units() - d("10000").raw(),
            (one.settlements(day).equity().units() - d("10000").raw()) * 2);
    }
    auto shifted = pair;
    shifted.mutable_paper()
        ->mutable_contracts(1)
        ->mutable_dataset()
        ->mutable_days()
        ->DeleteSubrange(1, 1);
    shifted.mutable_paper()
        ->mutable_contracts(1)
        ->mutable_dataset()
        ->mutable_bars()
        ->DeleteSubrange(4, 4);
    auto* shifted_data = shifted.mutable_paper()->mutable_contracts(1)->mutable_dataset();
    shifted_data->set_revision(protocol::bar_dataset_revision(*shifted_data));
    shifted.set_dataset_revision(protocol::dataset_revision(shifted.paper()));
    EXPECT_THROW(backtest::run(shifted), std::invalid_argument);
}

TEST(Backtest, FullBarBudgetRunsThroughFinalSettlement)
{
    auto spec = input();
    std::vector<MarketBar> bars;
    for (std::size_t i = 0; i < protocol::max_dataset_bars; ++i)
        bars.push_back(
            test::flat("2026-09-25", 1790298000000000000LL + i * 1000000LL, "100", "10"));
    *spec.mutable_paper()->mutable_contracts(0)->mutable_dataset() =
        test::dataset(bars, {{"2026-09-25", d("100")}});
    spec.set_dataset_revision(protocol::dataset_revision(spec.paper()));
    const auto result = backtest::run(spec);
    EXPECT_EQ(result.equity_size(), protocol::max_dataset_bars + 1);
    ASSERT_EQ(result.settlements_size(), 1);
    EXPECT_EQ(result.settlements(0).balance().units(), d("10000").raw());
    EXPECT_EQ(result.account().cursor(), protocol::max_dataset_bars);
}

TEST(TaskStore, DatedCostsArePinnedSwitchAtSettlementAndRejectForgedResults)
{
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
    EXPECT_EQ(
        expected.account().contracts(0).cost_schedule().SerializeAsString(),
        schedule->SerializeAsString());
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        tasks::submit(store, "dated", spec);
        const auto token = store.commit(store.claim("dated")).token();
        auto forged = expected;
        forged.mutable_account()
            ->mutable_contracts(0)
            ->mutable_cost_schedule()
            ->mutable_versions(1)
            ->set_source("unrecorded replacement");
        EXPECT_THROW(
            store.commit(tasks::finish(store, "dated", token, forged)),
            std::invalid_argument);
        forged = expected;
        forged.mutable_account()
            ->mutable_contracts(0)
            ->mutable_costs()
            ->mutable_margin_per_lot()
            ->set_units(d("100").raw());
        EXPECT_THROW(
            store.commit(tasks::finish(store, "dated", token, forged)),
            std::invalid_argument);
        store.commit(tasks::finish(store, "dated", token, expected));
    }
    tasks::Store restored(
        asterion::research::task_domain(),
        directory.path,
        tasks::Identity{"task"});
    EXPECT_EQ(tasks::result(restored, "dated").SerializeAsString(), expected.SerializeAsString());
    EXPECT_EQ(restored.get("dated").input().SerializeAsString(), spec.SerializeAsString());
}

TEST(NamedDatasets, CoverageDoesNotMergeIntervalsOrRevisions)
{
    TaskDirectory directory;
    const auto sparse = test::seed_history(
        directory.path,
        {100, 101, 102, 103},
        "sparse",
        {"2026-09-23", "2026-09-25"},
        {"2026-09-23", "2026-09-24", "2026-09-25"});
    const auto filled = test::seed_history(
        directory.path,
        {100, 101, 102, 103, 104, 105},
        "filled",
        {"2026-09-23", "2026-09-24", "2026-09-25"},
        {},
        "rb",
        5);
    history_files::Archive archive(directory.path / "history");
    history_files::TableStore tables(directory.path / "tables");
    data::proto::HistoryCoverageQuery query;
    *query.mutable_daily() = protocol::encode_daily_table(test::latest_daily({sparse, filled}));
    const auto result = data::history_coverage(archive, tables, query);
    // One row for each minute version.
    ASSERT_EQ(result.items_size(), 2);
    for (const auto& row : result.items()) {
        if (row.minute_dataset_id() == sparse.at("source_dataset_ids").at(0).get<std::string>()) {
            EXPECT_EQ(row.interval_minutes(), 1);
            EXPECT_EQ(row.uncovered(), 1);
            EXPECT_EQ(row.uncovered_days(0), "2026-09-24");
        } else {
            EXPECT_EQ(
                row.minute_dataset_id(),
                filled.at("source_dataset_ids").at(0).get<std::string>());
            EXPECT_EQ(row.interval_minutes(), 5);
            EXPECT_EQ(row.uncovered(), 0);
        }
    }
}

TEST(NamedDatasets, CoverageRejectsChangedManifestEvenWhenChunksAreValid)
{
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
    history_files::TableStore tables(directory.path / "tables");
    data::proto::HistoryCoverageQuery query;
    *query.mutable_daily() = protocol::encode_daily_table(selected.at("daily"));
    EXPECT_THROW(data::history_coverage(archive, tables, query), std::invalid_argument);
}

TEST(NamedDatasets, RequestedWindowExcludesUnrelatedMissingSettlement)
{
    TaskDirectory directory;
    const auto selection = test::seed_history(
        directory.path,
        {100, 101, 102, 103},
        "window",
        {"2026-09-23", "2026-09-25"},
        {"2026-09-25"});
    data::Store store(directory.path, "historical-data");
    data::proto::BarDatasetRequest request;
    request.add_source_dataset_ids(selection.at("source_dataset_ids").at(0).get<std::string>());
    *request.mutable_daily() = protocol::encode_daily_table(selection.at("daily"));
    request.set_contract("SHFE/rb/2026-10");
    EXPECT_THROW(
        data::resolve_bar_dataset(store.tables(), store.sources(request)),
        std::invalid_argument);
    request.set_begin_day("2026-09-25");
    request.set_end_day("2026-09-25");
    const auto dataset = data::resolve_bar_dataset(store.tables(), store.sources(request));
    ASSERT_EQ(dataset.days_size(), 1);
    EXPECT_EQ(dataset.days(0).trading_day(), "2026-09-25");
    EXPECT_EQ(dataset.bars_size(), 2);
}

TEST(NamedDatasets, ATradingDayTheDailyTableLacksIsNamedAndASnapshotKeepsWhatItRead)
{
    TaskDirectory directory;
    const auto selection = test::seed_history(
        directory.path,
        {100, 101, 102, 103},
        "hole",
        {"2026-09-23", "2026-09-25"});
    // The exchange turns out to have been open on the 24th as well, and nothing
    // of that day is stored.
    std::string later;
    {
        history_files::TableStore tables(directory.path / "tables");
        const auto partition = table_partition(
            history_table("futures.calendar"),
            "SHFE",
            "2026",
            {{"2026-09-23", "1", std::nullopt},
                {"2026-09-24", "1", std::nullopt},
                {"2026-09-25", "1", std::nullopt}});
        later = tables
                    .record(
                        partition,
                        {"futures.calendar", 1, "fixture.calendar.v1"},
                        tables.write(partition),
                        std::string(64, 'a'),
                        1790000000000000000)
                    .revision.snapshot;
    }
    data::Store store(directory.path, "historical-data");
    data::proto::BarDatasetRequest request;
    request.add_source_dataset_ids(selection.at("source_dataset_ids").at(0).get<std::string>());
    *request.mutable_daily() = protocol::encode_daily_table(selection.at("daily"));
    request.set_contract("SHFE/rb/2026-10");
    // Under the snapshot it was selected with, the dataset is what it was.
    const auto dataset = data::resolve_bar_dataset(store.tables(), store.sources(request));
    ASSERT_EQ(dataset.days_size(), 2);
    EXPECT_EQ(dataset.days(0).settlement_price().units(), Decimal::parse("110").raw());
    EXPECT_TRUE(days_without_bars(dataset).empty());
    EXPECT_EQ(dataset.daily().snapshot(), selection.at("daily").at("snapshot").get<std::string>());
    EXPECT_EQ(dataset.daily_acquired_at_ns(), 1790000000000000000);
    // Under the later one the missing day is named, not read around.
    request.mutable_daily()->set_snapshot(later);
    try {
        (void)data::resolve_bar_dataset(store.tables(), store.sources(request));
        FAIL() << "a dataset was read around a trading day the table lacks";
    } catch (const Error& error) {
        EXPECT_STREQ(
            error.what(),
            "futures.daily is missing trading days of the range: SHFE 2026-09-24");
    }
}

TEST(NamedDatasets, ADailyBarIsADayTheContractTradedAndADownloadOfDailyBarsIsNoSource)
{
    TaskDirectory directory;
    const auto seeded = test::seed_history(directory.path, {100, 101}, "daily-bars");
    // Traded on the 23rd and the 25th. On the 24th it only settled, and on the
    // 22nd ten lots changed hands outside the auction: neither day has a price
    // an order could have been filled at.
    const auto table = test::seed_daily_table(
        directory.path,
        "SHFE/rb/2026-10",
        {{"2026-09-22", 3005, 500, {}, 10},
            {"2026-09-23", 3010, 500, test::DailyTrade{3000, 3020, 2990, 3015, 1200}},
            {"2026-09-24", 3012, 500},
            {"2026-09-25", 3030, 520, test::DailyTrade{3015, 3040, 3010, 3035, 800}}});
    data::Store store(directory.path, "historical-data");
    data::proto::BarDatasetRequest request;
    *request.mutable_daily() = protocol::encode_daily_table(table);
    request.set_contract("SHFE/rb/2026-10");
    // The table does not bound the days as an archive version does.
    EXPECT_THROW(store.sources(request), std::invalid_argument);
    request.set_begin_day("2026-09-22");
    request.set_end_day("2026-09-25");
    const auto dataset = data::resolve_bar_dataset(store.tables(), store.sources(request));
    EXPECT_EQ(dataset.interval_minutes(), 0U);
    EXPECT_EQ(dataset.source(), "fixture.daily.v1");
    EXPECT_EQ(dataset.source_dataset_ids_size(), 0);
    EXPECT_EQ(dataset.history_evidence_size(), 0);
    ASSERT_EQ(dataset.bars_size(), 2);
    const auto bars = protocol::dataset_bars(dataset);
    EXPECT_EQ(bars[0].trading_day, "2026-09-23");
    // 15:00 in Shanghai on the trading day.
    EXPECT_EQ(bars[0].timestamp_ns, parse_shanghai_time("2026-09-23 15:00:00"));
    EXPECT_EQ(bars[0].open, Decimal::parse("3000"));
    EXPECT_EQ(bars[0].high, Decimal::parse("3020"));
    EXPECT_EQ(bars[0].low, Decimal::parse("2990"));
    EXPECT_EQ(bars[0].close, Decimal::parse("3015"));
    EXPECT_EQ(bars[0].volume, Decimal::parse("1200"));
    EXPECT_EQ(bars[1].close, Decimal::parse("3035"));
    // Every day it settled on is a day of the dataset, at that day's price.
    ASSERT_EQ(dataset.days_size(), 4);
    EXPECT_EQ(dataset.days(0).settlement_price().units(), Decimal::parse("3005").raw());
    EXPECT_EQ(dataset.days(1).settlement_price().units(), Decimal::parse("3010").raw());
    EXPECT_EQ(dataset.days(2).settlement_price().units(), Decimal::parse("3012").raw());
    EXPECT_EQ(dataset.days(3).settlement_price().units(), Decimal::parse("3030").raw());
    EXPECT_EQ(days_without_bars(dataset), (std::vector<std::string>{"2026-09-22", "2026-09-24"}));
}

// The contract table has today's lot of a contract, and a wrong multiplier
// moves every profit and loss without failing anything. The daily table's
// turnover says what a lot was worth.
TEST(NamedDatasets, AMultiplierTheTurnoverContradictsIsRefused)
{
    // Copper is five tonnes a lot. 1,200 lots at an average of 70,010 are worth
    // 420,060,000 and 1,000 at 70,480 are worth 352,400,000. On the third day
    // lots changed hands far from the day's prices, as in a delivery month: 800
    // lots worth 304,000,000 are 76,000 a tonne against a settlement of 70,300.
    const auto ask = [](int lot) {
        TaskDirectory directory;
        {
            data::Store created(directory.path, "historical-data");
        }
        const auto table = test::seed_daily_table(
            directory.path,
            "SHFE/cu/2026-10",
            {{"2026-09-23",
                 70000,
                 500,
                 test::DailyTrade{69900, 70200, 69800, 70100, 1200, 420060000}},
                {"2026-09-24",
                    70500,
                    500,
                    test::DailyTrade{70100, 70700, 70000, 70600, 1000, 352400000}},
                {"2026-09-25",
                    70300,
                    500,
                    test::DailyTrade{70600, 70700, 70100, 70200, 800, 304000000}}},
            {},
            lot);
        data::Store store(directory.path, "historical-data");
        data::proto::BarDatasetRequest request;
        *request.mutable_daily() = protocol::encode_daily_table(table);
        request.set_contract("SHFE/cu/2026-10");
        request.set_begin_day("2026-09-23");
        request.set_end_day("2026-09-25");
        return data::resolve_bar_dataset(store.tables(), store.sources(request));
    };
    // Two of the three days are within a twentieth of five.
    const auto dataset = ask(5);
    EXPECT_EQ(dataset.bars_size(), 3);
    EXPECT_EQ(dataset.contract().multiplier().units(), Decimal::parse("5").raw());
    try {
        (void)ask(10);
        FAIL() << "a multiplier twice what the turnover implies was taken";
    } catch (const std::invalid_argument& error) {
        // The middle of 4.99858, 5.00071 and 5.40541.
        EXPECT_STREQ(
            error.what(),
            "the contract multiplier does not match the turnover of " "SHFE/cu/2026-10: its days imply about 5.00071");
    }
}

// The contract table has today's price increment only: each day's is the one
// its prices show. Here the exchange moved the product from 5 to 2 for the
// 26th of fifty trading days.
TEST(NamedDatasets, EachDayTakesThePriceIncrementItsPricesShow)
{
    TaskDirectory directory;
    {
        data::Store created(directory.path, "historical-data");
    }
    std::vector<std::string> days;
    for (auto day = std::chrono::sys_days(std::chrono::year(2026) / 7 / 1); days.size() < 50;
        day += std::chrono::days(1))
        if (const std::chrono::weekday weekday(day);
            weekday != std::chrono::Saturday && weekday != std::chrono::Sunday)
            days.push_back(format_trading_date(std::chrono::year_month_day(day)));
    std::vector<test::DailyRow> rows;
    for (int i = 0; i < 50; ++i) {
        const auto step = i < 25 ? 5 : 2, open = 1000 + step * i;
        rows.push_back(
            {days[static_cast<std::size_t>(i)],
                open + step,
                100,
                test::DailyTrade{open, open + 2 * step, open - step, open + step, 100}});
    }
    const auto table = test::seed_daily_table(directory.path, "SHFE/rb/2026-10", rows);
    data::Store store(directory.path, "historical-data");
    const auto read = [&](std::size_t first, std::size_t last) {
        data::proto::BarDatasetRequest request;
        *request.mutable_daily() = protocol::encode_daily_table(table);
        request.set_contract("SHFE/rb/2026-10");
        request.set_begin_day(days[first]);
        request.set_end_day(days[last]);
        return data::resolve_bar_dataset(store.tables(), store.sources(request));
    };
    const auto changes = [](const data::proto::BarDataset& dataset) {
        std::vector<std::pair<std::string, std::string>> result;
        for (const auto& change : dataset.price_increment_changes())
            result.emplace_back(
                change.trading_day(),
                Decimal::from_raw(change.value().units()).str());
        return result;
    };
    const std::vector<std::pair<std::string, std::string>> moved{{days[25], "2"}};
    const auto whole = read(0, 49);
    EXPECT_EQ(whole.contract().price_increment().units(), d("5").raw());
    EXPECT_EQ(changes(whole), moved);
    // The multiplier is the contract table's: ten tonnes a lot.
    EXPECT_EQ(whole.contract().multiplier().units(), d("10").raw());
    // Five days around the change read the same: the days on either side of a
    // range are read with it, and the five alone divide by 1 only.
    const auto around = read(23, 27);
    EXPECT_EQ(around.contract().price_increment().units(), d("5").raw());
    EXPECT_EQ(changes(around), moved);
}

TEST(NamedDatasets, RequestedWindowReportsMissingBoundaryDays)
{
    TaskDirectory directory;
    const auto selection = test::seed_history(
        directory.path,
        {100, 101, 102, 103},
        "boundary",
        {"2026-09-24"},
        {"2026-09-23", "2026-09-24", "2026-09-25"});
    data::Store store(directory.path, "historical-data");
    data::proto::BarDatasetRequest request;
    request.add_source_dataset_ids(selection.at("source_dataset_ids").at(0).get<std::string>());
    *request.mutable_daily() = protocol::encode_daily_table(selection.at("daily"));
    request.set_begin_day("2026-09-23");
    request.set_end_day("2026-09-25");
    request.set_contract("SHFE/rb/2026-10");
    const auto dataset = data::resolve_bar_dataset(store.tables(), store.sources(request));
    EXPECT_EQ(days_without_bars(dataset), (std::vector<std::string>{"2026-09-23", "2026-09-25"}));
    task::proto::Task calculation;
    *factor_bars(*calculation.mutable_factor()) = dataset;
    calculation.mutable_factor()->set_factor(protocol::proto::PRICE_MOMENTUM);
    calculation.mutable_factor()->set_dataset_revision(dataset.revision());
    calculation.mutable_factor()->add_lookbacks(1);
    calculation.mutable_factor()->set_horizon(1);
    calculation.mutable_factor()->set_full_sample(true);
    const auto execution = protocol::task_execution(
        calculation,
        sha256_bytes(calculation.factor().SerializeAsString()));
    EXPECT_EQ(execution.factor().series(0).bars().begin_day(), "2026-09-23");
    EXPECT_EQ(execution.factor().series(0).bars().end_day(), "2026-09-25");
    EXPECT_EQ(
        data::resolve_bar_dataset(
            store.tables(),
            store.sources(execution.factor().series(0).bars()))
            .SerializeAsString(),
        dataset.SerializeAsString());
}

namespace {

data::proto::BarDatasetRequest composed_request(const std::vector<Json>& selections)
{
    std::set<std::string> ids;
    for (const auto& selected : selections)
        for (const auto& id : selected.at("source_dataset_ids"))
            ids.insert(id.get<std::string>());
    return protocol::encode_bar_dataset_request(
        {{"source_dataset_ids", ids},
            {"daily", test::latest_daily(selections)},
            {"begin_day", ""},
            {"end_day", ""},
            {"contract", "SHFE/rb/2026-10"}});
}

} // namespace

TEST(NamedDatasets, ComposesDownloadsDeduplicatesAndRestoresFixedVersions)
{
    TaskDirectory directory;
    const auto first =
        test::seed_history(directory.path, {100, 101}, "compose-first", {"2026-09-23"});
    const auto second =
        test::seed_history(directory.path, {102, 103}, "compose-second", {"2026-09-24"});
    const auto whole = test::seed_history(
        directory.path,
        {100, 101, 102, 103},
        "compose-whole",
        {"2026-09-23", "2026-09-24"});
    auto store = std::make_unique<data::Store>(directory.path, "historical-data");
    const auto request = composed_request({second, first});
    const auto result = data::resolve_bar_dataset(store->tables(), store->sources(request));
    EXPECT_EQ(result.bars_size(), 4);
    EXPECT_EQ(result.days_size(), 2);
    EXPECT_EQ(result.source_dataset_ids_size(), 2);
    EXPECT_TRUE(days_without_bars(result).empty());
    const auto overlapping = data::resolve_bar_dataset(
        store->tables(),
        store->sources(composed_request({first, whole, second})));
    EXPECT_EQ(overlapping.bars_size(), 4);
    EXPECT_EQ(overlapping.revision(), result.revision());
    EXPECT_EQ(
        data::resolve_bar_dataset(store->tables(), store->sources(composed_request({whole})))
            .revision(),
        result.revision());
    auto json = protocol::decode_bar_dataset_request(request);
    std::reverse(json["source_dataset_ids"].begin(), json["source_dataset_ids"].end());
    EXPECT_EQ(
        protocol::encode_bar_dataset_request(json).SerializeAsString(),
        request.SerializeAsString());
    history_files::Archive archive(directory.path / "history");
    data::proto::NamedDataset saved;
    saved.set_name("composed");
    *saved.add_selections() = request;
    saved.add_content_revisions(result.revision());
    saved.set_id(protocol::named_dataset_revision(saved));
    archive.save_named_dataset(saved);
    store.reset();
    data::Store restarted(directory.path, "historical-data");
    const auto restored = archive.named_dataset(saved.id());
    EXPECT_EQ(
        data::resolve_bar_dataset(restarted.tables(), restarted.sources(restored.selections(0)))
            .SerializeAsString(),
        result.SerializeAsString());
    // Without an archive version the bars are the contract's days in the table.
    auto daily = request;
    daily.clear_source_dataset_ids();
    daily.set_begin_day("2026-09-23");
    daily.set_end_day("2026-09-24");
    const auto day_result = data::resolve_bar_dataset(restarted.tables(), restarted.sources(daily));
    EXPECT_EQ(day_result.interval_minutes(), 0);
    EXPECT_EQ(day_result.bars_size(), 2);
}

TEST(NamedDatasets, CompositionRejectsConflictsAndClipsBeforeComparing)
{
    TaskDirectory directory;
    const auto one = test::seed_history(
        directory.path,
        {100, 101, 102, 103},
        "conflict-one",
        {"2026-09-23", "2026-09-24"});
    const auto two = test::seed_history(directory.path, {999, 101}, "conflict-two", {"2026-09-23"});
    data::Store store(directory.path, "historical-data");
    auto request = composed_request({one, two});
    EXPECT_THROW(
        data::resolve_bar_dataset(store.tables(), store.sources(request)),
        std::invalid_argument);
    request.set_begin_day("2026-09-24");
    EXPECT_EQ(data::resolve_bar_dataset(store.tables(), store.sources(request)).bars_size(), 2);
}

TEST(NamedDatasets, CompositionRejectsMixedContractsIntervalsAndSemantics)
{
    TaskDirectory directory;
    const auto one = test::seed_history(directory.path, {100, 101}, "mix-one");
    const auto contract =
        test::seed_history(directory.path, {100, 101}, "mix-contract", {"2026-09-25"}, {}, "cu");
    const auto interval =
        test::seed_history(directory.path, {100, 101}, "mix-interval", {"2026-09-25"}, {}, "rb", 5);
    const auto semantics = test::seed_history(
        directory.path,
        {100, 101},
        "mix-semantics",
        {"2026-09-25"},
        {},
        "rb",
        1,
        110,
        "test.confirmed.v2");
    data::Store store(directory.path, "historical-data");
    for (const auto& other : {contract, interval, semantics}) {
        auto request = composed_request({one, other});
        EXPECT_THROW(
            data::resolve_bar_dataset(store.tables(), store.sources(request)),
            std::invalid_argument);
    }
}

TEST(NamedDatasets, CompositionReportsGapsAndRejectsInvalidVersionLists)
{
    TaskDirectory directory;
    const auto one = test::seed_history(
        directory.path,
        {100, 101},
        "gap-one",
        {"2026-09-23"},
        {"2026-09-23", "2026-09-24", "2026-09-25"});
    const auto two = test::seed_history(directory.path, {102, 103}, "gap-two", {"2026-09-25"});
    data::Store store(directory.path, "historical-data");
    auto request = composed_request({one, two});
    const auto result = data::resolve_bar_dataset(store.tables(), store.sources(request));
    EXPECT_EQ(days_without_bars(result), std::vector<std::string>{"2026-09-24"});
    auto json = protocol::decode_bar_dataset_request(request);
    json["source_dataset_ids"].push_back(json["source_dataset_ids"][0]);
    EXPECT_THROW(protocol::encode_bar_dataset_request(json), std::invalid_argument);
    json["source_dataset_ids"] = Json::array();
    EXPECT_THROW(protocol::encode_bar_dataset_request(json), std::invalid_argument);
    json["source_dataset_ids"] = std::vector<std::string>(33, std::string(64, 'a'));
    EXPECT_THROW(protocol::encode_bar_dataset_request(json), std::invalid_argument);
    // Retired singular wire fields are rejected, not interpreted as a different version list.
    request.GetReflection()->MutableUnknownFields(&request)->AddLengthDelimited(
        6,
        std::string(64, 'a'));
    EXPECT_THROW(protocol::decode_bar_dataset_request(request), std::invalid_argument);
}

TEST(HistoryUpdate, IncrementalMinutePlansStartAfterFixedBoundaries)
{
    TaskDirectory directory;
    const auto seeded = test::seed_history(directory.path, {100, 101}, "update-base");
    history_files::Archive archive(directory.path / "history");
    history_files::TableStore tables(directory.path / "tables");
    auto query = protocol::encode_history_update_query(
        {{"dataset_id", seeded.at("source_dataset_ids").at(0)},
            {"daily", nullptr},
            {"mode", "extend"},
            {"end_day", "2026-09-26"},
            {"requests_per_minute", 60}});
    const auto now = parse_shanghai_time("2026-09-28 01:00:00");
    const auto before = archive.get(query.dataset_id()).SerializeAsString();
    const auto plan = data::history_update_plan(archive, tables, query, now);
    EXPECT_EQ(plan.minutes().begin_ns(), parse_shanghai_time("2026-09-25 09:01:01"));
    EXPECT_EQ(plan.minutes().end_ns(), parse_shanghai_time("2026-09-26 23:59:59"));
    EXPECT_EQ(plan.minutes().source_instrument(), "RB2610.SHF");
    EXPECT_EQ(data::history_update_plan(archive, tables, query, now).id(), plan.id());
    EXPECT_EQ(
        protocol::decode_history_update_plan(plan).at("query"),
        protocol::decode_history_update_query(query));
    EXPECT_EQ(archive.get(query.dataset_id()).SerializeAsString(), before);
    auto changed = plan;
    changed.mutable_minutes()->set_end_ns(plan.minutes().end_ns() + 1);
    EXPECT_THROW(protocol::decode_history_update_plan(changed), std::invalid_argument);
    query.set_end_day("2026-09-28");
    EXPECT_THROW(data::history_update_plan(archive, tables, query, now), std::invalid_argument);
    // A day before the version's last bar asks for nothing later.
    query.set_end_day("2026-09-24");
    EXPECT_THROW(data::history_update_plan(archive, tables, query, now), std::invalid_argument);
}

// The days to fetch again are those the daily table shows the contract traded
// on that the minute version lacks; the whole original window is fetched.
TEST(HistoryUpdate, RepairPinsTheDailyTableAndPreservesNightSessionWindow)
{
    TaskDirectory directory;
    const auto seeded = test::seed_history(
        directory.path,
        {100, 101, 102, 103},
        "update-gaps",
        {"2026-09-21", "2026-09-23"},
        {"2026-09-21", "2026-09-22", "2026-09-23"});
    const auto other =
        test::seed_history(directory.path, {100, 101}, "update-other", {"2026-09-23"}, {}, "cu");
    history_files::Archive archive(directory.path / "history");
    history_files::TableStore tables(directory.path / "tables");
    auto query = protocol::encode_history_update_query(
        {{"dataset_id", seeded.at("source_dataset_ids").at(0)},
            {"daily", seeded.at("daily")},
            {"mode", "repair"},
            {"end_day", ""},
            {"requests_per_minute", 60}});
    const auto now = parse_shanghai_time("2026-09-28 01:00:00");
    const auto source = archive.get(query.dataset_id());
    const auto plan = data::history_update_plan(archive, tables, query, now);
    ASSERT_EQ(plan.missing_days_size(), 1);
    EXPECT_EQ(plan.missing_days(0), "2026-09-22");
    EXPECT_EQ(plan.minutes().begin_ns(), source.minutes().begin_ns());
    EXPECT_EQ(plan.minutes().end_ns(), source.minutes().end_ns());
    EXPECT_EQ(plan.minutes().requests_per_minute(), 60);
    // A snapshot this store never had is refused, not read as another.
    auto unknown = query;
    unknown.mutable_daily()->set_snapshot(std::string(64, 'f'));
    EXPECT_THROW(data::history_update_plan(archive, tables, unknown, now), Error);
    // Copper's one minute day is the one day it traded: nothing to fetch.
    query.set_dataset_id(other.at("source_dataset_ids").at(0).get<std::string>());
    EXPECT_THROW(data::history_update_plan(archive, tables, query, now), std::invalid_argument);
}

TEST(HistoryUpdate, CorruptBaseAndMalformedQueryCannotProducePlans)
{
    TaskDirectory directory;
    const auto seeded = test::seed_history(directory.path, {100, 101}, "update-corrupt");
    history_files::Archive archive(directory.path / "history");
    auto query = protocol::encode_history_update_query(
        {{"dataset_id", seeded.at("source_dataset_ids").at(0)},
            {"daily", nullptr},
            {"mode", "extend"},
            {"end_day", "2026-09-26"},
            {"requests_per_minute", 60}});
    const auto now = parse_shanghai_time("2026-09-28 01:00:00");
    const auto path =
        std::filesystem::path(archive.get(query.dataset_id()).minute_result().directory())
        / "minutes.json";
    std::ifstream file(path);
    Json manifest;
    file >> manifest;
    manifest["rows"] = manifest.at("rows").get<unsigned>() + 1;
    replace_file_durably(path, manifest.dump());
    history_files::TableStore tables(directory.path / "tables");
    EXPECT_THROW(data::history_update_plan(archive, tables, query, now), std::invalid_argument);
    auto json = protocol::decode_history_update_query(query);
    json["requests_per_minute"] = 501;
    EXPECT_THROW(protocol::encode_history_update_query(json), std::invalid_argument);
    json["requests_per_minute"] = 1;
    json["mode"] = "automatic";
    EXPECT_THROW(protocol::encode_history_update_query(json), std::invalid_argument);
    json["mode"] = "repair";
    EXPECT_THROW(protocol::encode_history_update_query(json), std::invalid_argument);
}

TEST_F(TaskProcess, DataAdmissionPersistsExactReviewedRange)
{
    service.reset();
    const auto seeded = test::seed_history(warehouse.path, {100, 101}, "update-rpc");
    start_data();
    protocol::DataClient data(endpoint + ".data", "historical-data");
    data::proto::DataRequest preview;
    *preview.mutable_update_plan() = protocol::encode_history_update_query(
        {{"dataset_id", seeded.at("source_dataset_ids").at(0)},
            {"daily", nullptr},
            {"mode", "extend"},
            {"end_day", "2026-09-26"},
            {"requests_per_minute", 60}});
    const auto plan = data.call(preview).update_plan();
    data::proto::DataRequest capture;
    auto* authorization = capture.mutable_authorize_download();
    authorization->set_task_instance("historical-data");
    authorization->set_task_id("incremental");
    authorization->set_credential("fixture");
    *authorization->mutable_minutes() = plan.minutes();
    const auto captured = data.call(capture).download_authorization();
    data::proto::DataRequest submit;
    submit.mutable_acquisition_task()->mutable_submit()->set_id("wrong-task");
    submit.mutable_acquisition_task()->mutable_submit()->set_authorization(captured.id());
    EXPECT_THROW(data.call(submit), Error);
    data::proto::DataRequest get;
    get.mutable_acquisition_task()->mutable_get()->set_id("incremental");
    EXPECT_THROW(data.call(get), Error);
    submit.mutable_acquisition_task()->mutable_submit()->set_id("incremental");
    const auto created = data.call(submit).acquisition_task().task();
    EXPECT_EQ(created.id(), "incremental");
    EXPECT_EQ(created.kind(), task_wire::MINUTE_DOWNLOAD);
    EXPECT_EQ(
        data.call(get).acquisition_task().task().minutes().SerializeAsString(),
        plan.minutes().SerializeAsString());
    EXPECT_EQ(data.call(submit).acquisition_task().task().id(), created.id());
    data_service.reset();
    std::filesystem::remove(endpoint + ".data");
    std::filesystem::remove(endpoint + ".data-workers");
    start_data();
    EXPECT_EQ(
        data.call(get).acquisition_task().task().minutes().SerializeAsString(),
        plan.minutes().SerializeAsString());
    EXPECT_EQ(data.call(preview).update_plan().id(), plan.id());
}

TEST_F(TaskProcess, HistoryUsageKeepsTaskReferencesAfterCancellationAndRestart)
{
    service.reset();
    std::vector<int> prices(40);
    for (std::size_t i = 0; i < prices.size(); ++i)
        prices[i] = 100 + static_cast<int>(i) + static_cast<int>(i % 3);
    const auto seeded = test::seed_history(warehouse.path, prices, "usage");
    const auto request = composed_request({seeded});
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        data::Store data_store(warehouse.path, "historical-data");
        const auto data =
            data::resolve_bar_dataset(data_store.tables(), data_store.sources(request));
        auto backtest = input();
        *backtest.mutable_paper()->mutable_contracts(0)->mutable_dataset() = data;
        backtest.set_dataset_revision(protocol::dataset_revision(backtest.paper()));
        persist(store, "usage-backtest", backtest);
        store.commit(store.cancel("usage-backtest")).task();
        factor::proto::FactorInput factor;
        factor.set_factor(protocol::proto::PRICE_MOMENTUM);
        *factor_bars(factor) = data;
        factor.set_dataset_revision(data.revision());
        factor.add_lookbacks(1);
        factor.set_horizon(1);
        factor.set_full_sample(true);
        persist(store, "usage-factor", factor);
        store.commit(store.cancel("usage-factor")).task();
    }
    start();
    research::proto::ResearchRequest query;
    query.mutable_history_usage()->set_id(seeded.at("source_dataset_ids").at(0).get<std::string>());
    const auto first = call(query).history_usage();
    ASSERT_EQ(first.references_size(), 2);
    std::set<int> kinds;
    for (const auto& row : first.references()) {
        kinds.insert(row.kind());
        ASSERT_EQ(row.roles_size(), 1);
        EXPECT_EQ(row.roles(0), data::proto::HISTORY_MARKET);
    }
    EXPECT_EQ(kinds, (std::set<int>{data::proto::HISTORY_BACKTEST, data::proto::HISTORY_FACTOR}));
    EXPECT_EQ(protocol::decode_history_usage(first).at("references").size(), 2);
    service.reset();
    start();
    EXPECT_EQ(call(query).history_usage().SerializeAsString(), first.SerializeAsString());
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

TEST_F(TaskProcess, HistoryUsageFindsImportedTaskWithoutLocalArchiveCopy)
{
    service.reset();
    const auto spec = input();
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        persist(store, "external-origin", spec);
        store.commit(store.cancel("external-origin")).task();
    }
    EXPECT_FALSE(std::filesystem::exists(directory.path / "history"));
    start();
    research::proto::ResearchRequest query;
    query.mutable_history_usage()->set_id(
        spec.paper().contracts(0).dataset().source_dataset_ids(0));
    const auto result = call(query).history_usage();
    ASSERT_EQ(result.references_size(), 1);
    EXPECT_EQ(result.references(0).id(), "external-origin");
    EXPECT_EQ(result.references(0).kind(), data::proto::HISTORY_BACKTEST);
    EXPECT_EQ(result.references(0).roles(0), data::proto::HISTORY_MARKET);
    service.reset();
    start();
    EXPECT_EQ(call(query).history_usage().SerializeAsString(), result.SerializeAsString());
}

TEST(HistoryUsage, RejectsUnknownKindsDuplicateReferencesAndInvalidRoles)
{
    data::proto::HistoryUsage usage;
    usage.set_dataset_id(std::string(64, 'a'));
    EXPECT_TRUE(protocol::decode_history_usage(usage).at("references").empty());
    auto* row = usage.add_references();
    row->set_id("consumer");
    row->set_kind(data::proto::HISTORY_BACKTEST);
    row->add_roles(data::proto::HISTORY_MARKET);
    EXPECT_EQ(protocol::decode_history_usage(usage).at("references").at(0).at("roles").size(), 1);
    const auto valid = usage;
    *usage.add_references() = usage.references(0);
    EXPECT_THROW(protocol::decode_history_usage(usage), std::invalid_argument);
    usage = valid;
    usage.mutable_references(0)->set_kind(data::proto::HISTORY_REFERENCE_UNSPECIFIED);
    EXPECT_THROW(protocol::decode_history_usage(usage), std::invalid_argument);
    usage = valid;
    usage.mutable_references(0)->add_roles(data::proto::HISTORY_OUTPUT);
    EXPECT_THROW(protocol::decode_history_usage(usage), std::invalid_argument);
}

TEST(HistoryUsage, StoppedLedgerInspectionDoesNotRecoverTasksOrChangeFiles)
{
    TaskDirectory directory, warehouse;
    const auto seeded = test::seed_history(warehouse.path, {100, 101, 102}, "stopped-usage");
    const auto request = composed_request({seeded});
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        data::Store data_store(warehouse.path, "historical-data");
        const auto data =
            data::resolve_bar_dataset(data_store.tables(), data_store.sources(request));
        auto task = input();
        *task.mutable_paper()->mutable_contracts(0)->mutable_dataset() = data;
        task.set_dataset_revision(protocol::dataset_revision(task.paper()));
        tasks::submit(store, "running", task);
        (void)store.commit(store.claim("running")).token();
        EXPECT_THROW(
            tasks::Store::inspect_history_usage(
                asterion::research::task_domain(),
                directory.path,
                tasks::Identity{"task"},
                seeded.at("source_dataset_ids")[0]),
            std::exception);
    }
    const auto fingerprints = [&] {
        std::map<std::string, std::string> files;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(directory.path))
            if (entry.is_regular_file())
                files[entry.path().lexically_relative(directory.path).string()] =
                    sha256_file(entry.path());
        return files;
    };
    const auto before = fingerprints();
    const auto usage = tasks::Store::inspect_history_usage(
        asterion::research::task_domain(),
        directory.path,
        tasks::Identity{"task"},
        seeded.at("source_dataset_ids")[0]);
    EXPECT_EQ(usage.references_size(), 1); // Only the still-running task owns this reference.
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
        EXPECT_EQ(state.integer(0), task::proto::RUNNING);
    }
    const auto lock = directory.path / "manager.lock";
    std::filesystem::remove(lock);
    EXPECT_THROW(
        tasks::Store::inspect_history_usage(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"},
            seeded.at("source_dataset_ids")[0]),
        std::exception);
    EXPECT_FALSE(std::filesystem::exists(lock));
    EXPECT_THROW(
        tasks::Store rejected(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"}),
        std::runtime_error);
    EXPECT_FALSE(std::filesystem::exists(lock));
    EXPECT_THROW(
        tasks::Store::inspect_history_usage(
            asterion::research::task_domain(),
            directory.path / "missing",
            tasks::Identity{"task"},
            std::string(64, 'a')),
        std::exception);
    EXPECT_FALSE(std::filesystem::exists(directory.path / "missing"));
}

TEST(HistoryUsage, StoppedInspectionRejectsCorruptStateWithoutRepair)
{
    TaskDirectory directory;
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        tasks::submit(store, "bad", input());
    }
    {
        sqlite::Database db(directory.path / "tasks.sqlite");
        db.execute("UPDATE tasks SET manifest='{}'");
    }
    const auto before = sha256_file(directory.path / "tasks.sqlite");
    EXPECT_THROW(
        tasks::Store::inspect_history_usage(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"},
            std::string(64, 'a')),
        std::exception);
    EXPECT_EQ(sha256_file(directory.path / "tasks.sqlite"), before);
    EXPECT_FALSE(std::filesystem::exists(directory.path / "history"));
}

TEST(TaskStore, MixedHistoricalWarehouseIsRejectedWithoutChangingFiles)
{
    TaskDirectory directory;
    std::filesystem::create_directory(directory.path / "history");
    const auto evidence = directory.path / "history" / "preserve.bin";
    replace_file_durably(evidence, "owned historical data");
    EXPECT_THROW(
        tasks::Store rejected(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"}),
        std::invalid_argument);
    EXPECT_EQ(sha256_file(evidence), sha256_bytes("owned historical data"));
    EXPECT_FALSE(std::filesystem::exists(directory.path / "tasks.sqlite"));
    EXPECT_FALSE(std::filesystem::exists(directory.path / "manager.lock"));
}

TEST(TaskStore, ForeignStoreIsRejectedBeforeCreatingTaskSchema)
{
    TaskDirectory directory;
    {
        sqlite::Database database(directory.path / "tasks.sqlite");
        database.execute("CREATE TABLE foreign_data(value TEXT)");
        database.execute("INSERT INTO foreign_data VALUES('preserve')");
    }
    const auto before = sha256_file(directory.path / "tasks.sqlite");
    EXPECT_THROW(
        tasks::Store rejected(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"}),
        std::exception);
    EXPECT_EQ(sha256_file(directory.path / "tasks.sqlite"), before);
    EXPECT_FALSE(std::filesystem::exists(directory.path / "manager.lock"));
    EXPECT_FALSE(std::filesystem::exists(directory.path / "history"));
    sqlite::Database inspect(directory.path / "tasks.sqlite", sqlite::Database::Access::read_only);
    sqlite::Database::Statement tables(
        inspect,
        "SELECT COUNT(*) FROM sqlite_schema WHERE type='table'");
    ASSERT_TRUE(tables.step());
    EXPECT_EQ(tables.integer(0), 1);
}

TEST(TaskStore, RetainedExperimentsContinueInTheSameWarehouseBeyondOneThousandTasks)
{
    TaskDirectory directory, warehouse;
    std::vector<int> prices(40);
    for (std::size_t i = 0; i < prices.size(); ++i)
        prices[i] = 100 + static_cast<int>(i) + static_cast<int>(i % 3);
    const auto selection = test::seed_history(warehouse.path, prices, "long-lived");
    data::Store data_store(warehouse.path, "historical-data");
    const auto source = data::resolve_bar_dataset(
        data_store.tables(),
        data_store.sources(composed_request({selection})));
    factor::proto::FactorInput spec;
    spec.set_factor(protocol::proto::PRICE_MOMENTUM);
    *factor_bars(spec) = source;
    spec.set_dataset_revision(source.revision());
    spec.add_lookbacks(1);
    spec.set_horizon(1);
    spec.set_full_sample(true);
    const auto expected = factor::run(spec);
    const auto dataset_id = selection.at("source_dataset_ids").at(0).get<std::string>();
    std::string original_hash;
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
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
        EXPECT_EQ(tasks::submit(store, "experiment-0", spec).state(), task::proto::SUCCEEDED);
        tasks::submit(store, "retry-original", spec);
        store.commit(store.fail(
            "retry-original",
            store.commit(store.claim("retry-original")).token(),
            "fixture failure"));
        unsigned pages = 0;
        while (store.next_history_page(usage)) {
            ++pages;
            auto reading = std::async(std::launch::async, [&] {
                usage.load_page();
            });
            EXPECT_EQ(store.describe("retry-original").state(), task::proto::FAILED);
            reading.get();
        }
        EXPECT_EQ(pages, 6U);
        EXPECT_EQ(usage.take().references_size(), 1001);
    }
    {
        tasks::Store restored(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        EXPECT_EQ(
            tasks::factor_result(restored, "experiment-0").SerializeAsString(),
            expected.SerializeAsString());
        EXPECT_EQ(
            tasks::factor_result(restored, "experiment-1000").SerializeAsString(),
            expected.SerializeAsString());
        EXPECT_EQ(sha256_file(directory.path / "experiment-0/results/1.pb"), original_hash);
        EXPECT_EQ(restored.history_usage(dataset_id).references_size(), 1002);
        const auto retried = restored.commit(restored.retry("retry-original")).task();
        EXPECT_EQ(retried.submission_sequence(), 1002);
        EXPECT_EQ(retried.attempt(), 1);
        EXPECT_EQ(restored.list().capacity().active_used(), 1);
        restored.commit(
            tasks::finish(
                restored,
                "retry-original",
                restored.commit(restored.claim("retry-original")).token(),
                expected));
        EXPECT_EQ(restored.describe("retry-original").attempt(), 2);
        EXPECT_EQ(restored.list().capacity().active_used(), 0);
        EXPECT_EQ(
            data::resolve_bar_dataset(
                data_store.tables(),
                data_store.sources(composed_request({selection})))
                .SerializeAsString(),
            source.SerializeAsString());
    }
    // Membership is part of the recovery index, never authority to silently skip history.
    {
        sqlite::Database database(directory.path / "tasks.sqlite");
        database.execute("DELETE FROM task_event_members WHERE task_id='experiment-0'");
    }
    const auto evidence = sha256_file(directory.path / "tasks.sqlite");
    EXPECT_THROW(
        (tasks::Store(asterion::research::task_domain(), directory.path, tasks::Identity{"task"})),
        std::invalid_argument);
    EXPECT_EQ(sha256_file(directory.path / "tasks.sqlite"), evidence);
}

TEST(TaskStore, ActiveCapacityAppliesToNewAdmissionsAndExplicitRetries)
{
    TaskDirectory directory;
    factor::proto::FactorInput spec;
    spec.set_factor(protocol::proto::PRICE_MOMENTUM);
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
    tasks::Store store(asterion::research::task_domain(), directory.path, tasks::Identity{"task"});
    for (unsigned i = 0; i < 1000; ++i)
        tasks::submit(store, "queued-" + std::to_string(i), spec);
    EXPECT_EQ(store.list().capacity().active_used(), 1000);
    EXPECT_THROW(tasks::submit(store, "next", spec), std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(directory.path / "next"));
    EXPECT_EQ(store.commit(store.cancel("queued-0")).task().state(), task::proto::CANCELLED);
    EXPECT_EQ(store.list().capacity().active_used(), 999);
    auto pending = task_submission(store, "next", spec);
    store.admit_submission(pending);
    EXPECT_EQ(store.list().capacity().active_reserved(), 1U);
    EXPECT_FALSE(std::filesystem::exists(directory.path / "next"));
    EXPECT_THROW(store.commit(store.retry("queued-0")), std::invalid_argument);
    pending.prepare_files();
    EXPECT_EQ(store.commit(store.register_submission(pending)).task().submission_sequence(), 1001);
    EXPECT_EQ(store.list().capacity().active_reserved(), 0U);
    EXPECT_THROW(store.commit(store.retry("queued-0")).task(), std::invalid_argument);
    EXPECT_EQ(store.describe("queued-0").state(), task::proto::CANCELLED);
    store.commit(store.cancel("next")).task();
    EXPECT_EQ(store.commit(store.retry("queued-0")).task().submission_sequence(), 1);
    EXPECT_EQ(store.list().capacity().retained_tasks(), 1001);
    EXPECT_EQ(store.list().capacity().active_used(), 1000);
}

TEST(TaskStore, AComparisonCountsEveryReplayAndKeepsItsScores)
{
    TaskDirectory directory;
    const auto spec = comparison();
    const auto result = backtest::run(spec);
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        const auto task = tasks::submit(store, "compared", spec);
        EXPECT_EQ(task.total(), 80U);
        const auto token = store.commit(store.claim("compared")).token();
        // A result without the scores of what it compared is not this task's.
        auto bare = result;
        bare.clear_candidates();
        bare.clear_selected();
        EXPECT_THROW(
            store.commit(tasks::finish(store, "compared", token, bare)),
            std::invalid_argument);
        // Nor is one that names another strategy than the best of them.
        auto misnamed = result;
        misnamed.set_selected(0);
        EXPECT_THROW(
            store.commit(tasks::finish(store, "compared", token, misnamed)),
            std::invalid_argument);
        store.commit(tasks::finish(store, "compared", token, result));
    }
    tasks::Store restored(
        asterion::research::task_domain(),
        directory.path,
        tasks::Identity{"task"});
    EXPECT_EQ(restored.get("compared").state(), task::proto::SUCCEEDED);
}

TEST(TaskStore, ARollingComparisonKeepsItsRoundsAndRefusesOthers)
{
    TaskDirectory directory;
    const auto spec = rolling_comparison();
    const auto result = backtest::run(spec);
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        const auto task = tasks::submit(store, "rolled", spec);
        EXPECT_EQ(task.total(), 180U);
        const auto token = store.commit(store.claim("rolled")).token();
        // A result without its rounds, or short of one, is not this task's.
        auto bare = result;
        bare.clear_folds();
        EXPECT_THROW(
            store.commit(tasks::finish(store, "rolled", token, bare)),
            std::invalid_argument);
        auto short_of = result;
        short_of.mutable_folds()->RemoveLast();
        EXPECT_THROW(
            store.commit(tasks::finish(store, "rolled", token, short_of)),
            std::invalid_argument);
        // Nor is one whose round followed another strategy than its best, or
        // that begins on another day.
        auto misnamed = result;
        misnamed.mutable_folds(0)->set_selected(1 - result.folds(0).selected());
        EXPECT_THROW(
            store.commit(tasks::finish(store, "rolled", token, misnamed)),
            std::invalid_argument);
        auto moved = result;
        moved.mutable_folds(1)->set_first_day(result.folds(2).first_day());
        EXPECT_THROW(
            store.commit(tasks::finish(store, "rolled", token, moved)),
            std::invalid_argument);
        for (const auto corrupted : {0, 1, 2, 3, 4}) {
            auto changed = result;
            auto& fold = *changed.mutable_folds(0);
            if (corrupted == 0)
                fold.clear_training_first_day();
            else if (corrupted == 1)
                fold.set_training_last_day(fold.first_day());
            else if (corrupted == 2)
                fold.set_last_day(fold.first_day());
            else if (corrupted == 3)
                fold.set_training_days(fold.training_days() - 1);
            else
                fold.set_validation_days(fold.validation_days() + 1);
            EXPECT_THROW(
                store.commit(tasks::finish(store, "rolled", token, changed)),
                std::invalid_argument);
        }
        store.commit(tasks::finish(store, "rolled", token, result));
    }
    tasks::Store restored(
        asterion::research::task_domain(),
        directory.path,
        tasks::Identity{"task"});
    EXPECT_EQ(restored.get("rolled").state(), task::proto::SUCCEEDED);
}

TEST(TaskStore, FailureCodeRemainsAnOptionalObservationAcrossRestartAndExplicitRetry)
{
    TaskDirectory directory;
    const auto spec = input();
    std::string original;
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        tasks::submit(store, "typed-failure", spec);
        original = store.get("typed-failure").input().SerializeAsString();
        const auto token = store.commit(store.claim("typed-failure")).token();
        store.commit(
            store.fail("typed-failure", token, "fixture source unavailable", "unavailable"));
        tasks::submit(store, "unknown-failure", spec);
        const auto other = store.commit(store.claim("unknown-failure")).token();
        // The same words in a diagnostic are not structured failure evidence.
        store.commit(store.fail("unknown-failure", other, "fixture source unavailable"));
    }
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        EXPECT_EQ(store.get("typed-failure").error_code(), "unavailable");
        EXPECT_TRUE(store.get("unknown-failure").error_code().empty());
        EXPECT_EQ(store.list().tasks(0).error_code(), "unavailable");
        const auto queued = store.commit(store.retry("typed-failure")).task();
        EXPECT_EQ(queued.state(), task::proto::QUEUED);
        EXPECT_TRUE(queued.error_code().empty());
        EXPECT_EQ(store.get("typed-failure").input().SerializeAsString(), original);
    }
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        EXPECT_TRUE(store.get("typed-failure").error_code().empty());
        EXPECT_EQ(store.get("typed-failure").state(), task::proto::QUEUED);
    }
}

TEST(SlippageSensitivity, AVariantPreservesEveryOtherFixedInputByte)
{
    std::vector<MarketBar> bars;
    for (int i = 0; i < 6; ++i) {
        const auto day = "2026-09-" + std::to_string(21 + i);
        bars.push_back(test::flat(day, 1790000000000000000LL + i * 86400000000000LL, "100"));
    }
    auto dataset = test::dataset(bars);
    dataset.set_interval_minutes(0);
    dataset.clear_source_dataset_ids();
    dataset.clear_history_evidence();
    dataset.set_source(dataset.daily().daily_source());
    dataset.set_revision(protocol::bar_dataset_revision(dataset));
    protocol::validate_bar_dataset(dataset);
    auto original = testing_support::backtest_input(dataset);
    original.mutable_strategies(0)->clear_rule();
    original.mutable_strategies(0)->mutable_momentum()->set_lookback(2);
    const auto bytes = original.SerializeAsString();
    auto changed = protocol::backtest_slippage_variant(original, 2);
    EXPECT_EQ(changed.paper().contracts(0).slippage_ticks(), 2);
    changed.mutable_paper()->mutable_contracts(0)->set_slippage_ticks(0);
    EXPECT_EQ(changed.SerializeAsString(), bytes);
    EXPECT_EQ(original.SerializeAsString(), bytes);
    EXPECT_THROW(protocol::backtest_slippage_variant(original, 0), std::invalid_argument);
    EXPECT_THROW(protocol::backtest_slippage_variant(original, 101), std::invalid_argument);
    auto wrong = input();
    EXPECT_THROW(protocol::backtest_slippage_variant(wrong, 2), std::invalid_argument);
}

namespace {

data::proto::DownloadAuthorization refresh_authorization(const std::string& id)
{
    data::proto::DownloadAuthorization authorization;
    authorization.set_id(std::string(64, 'a'));
    authorization.set_provider_artifact(std::string(64, 'b'));
    authorization.set_data_instance("historical-data");
    authorization.set_task_instance("historical-data");
    authorization.set_task_id(id);
    *authorization.mutable_table_update() = protocol::encode_table_update(
        {{"provider", "test.independent.c"},
            {"tables", {"futures.daily"}},
            {"begin_day", "2024-03-01"},
            {"end_day", "2024-03-01"},
            {"requests_per_minute", 60},
            {"refresh",
                {{"partition",
                     {{"table", "futures.daily"}, {"exchange", "SHFE"}, {"period", "2024-03-01"}}},
                    {"source", "fixture.daily.v1"},
                    {"previous_snapshot", std::string(64, 'c')},
                    {"previous_content", std::string(64, 'd')},
                    {"provider_artifact", std::string(64, 'b')}}}});
    return authorization;
}

} // namespace

TEST(TaskStore, RefreshTargetsAndResultsSurviveRecoveryWithoutChangingEvidence)
{
    TaskDirectory directory;
    auto authorization = refresh_authorization("completed-refresh");
    std::string completed, failed, result;
    {
        tasks::Store store(
            asterion::data::task_domain(),
            directory.path,
            tasks::Identity{"historical-data"});
        tasks::submit(store, authorization);
        const auto token = store.commit(store.claim("completed-refresh")).token();
        task::proto::TaskFinish finish;
        finish.set_id("completed-refresh");
        finish.set_token(token);
        finish.mutable_table_update()->set_unpublished(1);
        store.commit(tasks::finish(store, finish));
        completed = store.get("completed-refresh").SerializeAsString();
        result = tasks::verified_result(store, "completed-refresh").SerializeAsString();
        authorization.set_task_id("failed-refresh");
        authorization.set_id(std::string(64, 'e'));
        tasks::submit(store, authorization);
        const auto other = store.commit(store.claim("failed-refresh")).token();
        store.commit(store.fail("failed-refresh", other, "fixture unavailable", "unavailable"));
        failed = store.get("failed-refresh").SerializeAsString();
    }
    {
        tasks::Store store(
            asterion::data::task_domain(),
            directory.path,
            tasks::Identity{"historical-data"});
        EXPECT_EQ(store.get("completed-refresh").SerializeAsString(), completed);
        EXPECT_EQ(store.get("failed-refresh").SerializeAsString(), failed);
        EXPECT_EQ(tasks::verified_result(store, "completed-refresh").SerializeAsString(), result);
        ASSERT_EQ(store.list().tasks_size(), 2);
        EXPECT_TRUE(store.list().tasks(0).has_table_refresh());
        EXPECT_EQ(
            store.list().tasks(0).table_refresh().SerializeAsString(),
            authorization.table_update().refresh().SerializeAsString());
        const auto retried = store.commit(store.retry("failed-refresh")).task();
        EXPECT_TRUE(retried.has_table_refresh());
        EXPECT_EQ(retried.state(), task::proto::QUEUED);
    }
}

TEST(ResearchHost, AdmissionExpiresWithoutAbandoningAnAcceptedCommit)
{
    service::reset_stop_request();
    TaskDirectory directory;
    {
        tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
        tasks::submit(store, "queued", input());
    }
    {
        tasks::ResearchHost host(
            {.directory = directory.path,
                .service = "task",
                .discovery_endpoint = "/unused-review-discovery.sock"});
        host.open();
        const auto operation = [&](research::proto::ResearchRequest request) {
            request.set_version(1);
            request.set_service_id("task");
            request.set_correlation_id(unique_process_id());
            return host.accept(request.SerializeAsString(), tasks::ResearchHost::Lane::client);
        };
        const auto poll = [&](service::RpcHost::Reply& request) {
            host.advance(service::RpcHost::Stage::running);
            return request();
        };
        const auto initial_deadline = std::chrono::steady_clock::now() + 5s;
        bool initialized = false;
        while (!initialized && std::chrono::steady_clock::now() < initial_deadline) {
            research::proto::ResearchRequest request;
            request.mutable_heartbeat();
            auto health = operation(request);
            auto answer = poll(health);
            if (answer) {
                task::proto::TaskResponse response;
                response.ParseFromString(answer->payload);
                initialized = response.health().initialized();
            }
            std::this_thread::sleep_for(5ms);
        }
        ASSERT_TRUE(initialized);

        struct Release
        {
            ~Release()
            {
                sqlite::hold_commits_for_testing(false);
            }
        } release;

        sqlite::hold_commits_for_testing(true);
        research::proto::ResearchRequest cancel_request;
        cancel_request.mutable_cancel()->set_id("queued");
        auto cancel = operation(cancel_request);
        const auto hold_deadline = std::chrono::steady_clock::now() + 3s;
        while (!sqlite::commit_waiting_for_testing()
            && std::chrono::steady_clock::now() < hold_deadline) {
            poll(cancel);
            std::this_thread::sleep_for(5ms);
        }
        const bool held = sqlite::commit_waiting_for_testing();
        research::proto::ResearchRequest list_request;
        list_request.mutable_list()->set_limit(200);
        auto list = operation(list_request);
        std::optional<service::RpcHost::Message> answer;
        const auto started = std::chrono::steady_clock::now();
        while (!answer && std::chrono::steady_clock::now() - started < 11s) {
            answer = poll(list);
            std::this_thread::sleep_for(5ms);
        }
        const bool timed_out_without_journal = answer.has_value();
        sqlite::hold_commits_for_testing(false);
        std::optional<service::RpcHost::Message> cancel_answer;
        const auto drain = std::chrono::steady_clock::now() + 5s;
        while ((!answer || !cancel_answer) && std::chrono::steady_clock::now() < drain) {
            host.advance(service::RpcHost::Stage::running);
            if (!answer)
                answer = list();
            if (!cancel_answer)
                cancel_answer = cancel();
            std::this_thread::sleep_for(5ms);
        }
        EXPECT_TRUE(held);
        EXPECT_TRUE(timed_out_without_journal);
        ASSERT_TRUE(answer.has_value());
        task::proto::TaskResponse response;
        ASSERT_TRUE(response.ParseFromString(answer->payload));
        EXPECT_EQ(response.error().code(), "unavailable");
        EXPECT_EQ(response.error().message(), "task request admission timed out");
        ASSERT_TRUE(cancel_answer);
        ASSERT_TRUE(response.ParseFromString(cancel_answer->payload));
        EXPECT_FALSE(response.has_error());
        EXPECT_EQ(response.task().state(), task::proto::CANCELLED);
    }
    tasks::Store recovered(asterion::research::task_domain(), directory.path, {"task"});
    EXPECT_EQ(recovered.describe("queued").state(), task::proto::CANCELLED);
    EXPECT_EQ(recovered.describe("queued").attempt(), 0U);
    EXPECT_EQ(recovered.list().tasks_size(), 1);
}

TEST(TaskStore, RefreshReceiptsMatchFixedTargetsAndAttemptIdentityAtCompletionAndRead)
{
    for (const bool unchanged : {false, true}) {
        SCOPED_TRACE(unchanged);
        TaskDirectory directory;
        const auto authorization = refresh_authorization("refresh");
        auto owned_store = std::make_unique<tasks::Store>(
            asterion::data::task_domain(),
            directory.path,
            tasks::Identity{"historical-data"});
        auto& store = *owned_store;
        tasks::submit(store, authorization);
        const auto token = store.commit(store.claim("refresh")).token();
        task::proto::TaskFinish finish;
        finish.set_id("refresh");
        finish.set_token(token);
        auto& result = *finish.mutable_table_update();
        result.set_stored(unchanged ? 0 : 1);
        result.set_unchanged(unchanged ? 1 : 0);
        result.set_snapshot(std::string(64, 'e'));
        auto& receipt = *result.mutable_refreshed_revision();
        receipt.set_sequence(1);
        receipt.set_table("futures.daily");
        receipt.set_schema(1);
        receipt.set_source("fixture.daily.v1");
        receipt.set_exchange("SHFE");
        receipt.set_period("2024-03-01");
        receipt.set_rows(1);
        receipt.set_bytes(10);
        receipt.set_acquired_at_ns(1);
        receipt.set_content(std::string(64, 'e'));
        receipt.set_object(std::string(64, 'f'));
        receipt.set_snapshot(result.snapshot());
        // Unchanged content can have been stored by an earlier provider artifact.
        receipt.set_provider_artifact(std::string(64, unchanged ? '9' : 'b'));
        auto& observation = *result.mutable_refresh_observation();
        observation.set_id(std::string(64, 'a'));
        observation.set_operation(std::string(64, 'b'));
        observation.set_data_instance("historical-data");
        observation.set_task_instance("historical-data");
        observation.set_task_id("refresh");
        observation.set_attempt(1);
        observation.set_provider(authorization.table_update().provider());
        observation.set_provider_artifact(authorization.provider_artifact());
        observation.set_selected_snapshot(
            authorization.table_update().refresh().previous_snapshot());
        observation.set_selected_content(authorization.table_update().refresh().previous_content());
        *observation.mutable_revision() = receipt;
        observation.set_unchanged(unchanged);
        observation.mutable_response()->set_format("tushare.fut_daily.raw.v1");
        observation.mutable_response()->set_body("fixture response");
        observation.mutable_response()->set_received_at_ns(2);
        observation.set_recorded_at_ns(3);
        observation.set_response_bytes(observation.response().body().size());
        observation.set_response_sha256(sha256_bytes(observation.response().body()));
        auto wrong = finish;
        wrong.mutable_table_update()->mutable_refreshed_revision()->set_exchange("DCE");
        wrong.mutable_table_update()->mutable_refreshed_revision()->set_period("2024-04-01");
        EXPECT_THROW(tasks::finish(store, wrong), std::invalid_argument);
        wrong = finish;
        wrong.mutable_table_update()->mutable_refresh_observation()->set_attempt(2);
        EXPECT_THROW(tasks::finish(store, wrong), std::invalid_argument);
        wrong = finish;
        wrong.mutable_table_update()->mutable_refresh_observation()->set_provider_artifact(
            std::string(64, '8'));
        EXPECT_THROW(tasks::finish(store, wrong), std::invalid_argument);
        wrong = finish;
        wrong.mutable_table_update()->clear_refreshed_revision();
        wrong.mutable_table_update()->clear_refresh_observation();
        EXPECT_THROW(tasks::finish(store, wrong), std::invalid_argument);
        EXPECT_EQ(store.describe("refresh").state(), task::proto::RUNNING);
        EXPECT_FALSE(std::filesystem::exists(directory.path / "refresh/results/1.pb"));
        store.commit(tasks::finish(store, finish));
        EXPECT_EQ(
            tasks::verified_result(store, "refresh").table_update().SerializeAsString(),
            result.SerializeAsString());
    }
}

namespace {

backtest::proto::BacktestInput settlement_margin_input()
{
    std::vector<MarketBar> bars;
    for (int i = 0; i < 4; ++i) {
        const auto day = "2026-09-" + std::to_string(25 + i);
        const auto open = std::to_string(99 + i), close = std::to_string(100 + i);
        bars.push_back(
            test::bar(
                day,
                trading_day_close_ns(day),
                open.c_str(),
                close.c_str(),
                open.c_str(),
                close.c_str(),
                "100"));
    }
    auto input = testing_support::backtest_input(test::dataset(bars, {}, test::contract(), 0));
    *input.mutable_strategies(0) = testing_support::moving_average(1, 2);
    auto* costs = input.mutable_paper()->mutable_contracts(0)->mutable_cost_schedule();
    costs->mutable_versions(0)->mutable_values()->mutable_margin_per_lot()->set_units(0);
    costs->mutable_versions(0)->mutable_values()->mutable_margin_rate()->set_units(d("0.14").raw());
    const auto next = costs->versions(0);
    *costs->add_versions() = next;
    costs->mutable_versions(1)->set_effective_from("2026-09-28");
    costs->mutable_versions(1)->mutable_values()->mutable_margin_rate()->set_units(d("0.17").raw());
    for (const auto& [day, rate] : std::vector<std::pair<std::string, const char*>>{
             {"2026-09-27", "0.16"},
             {"2026-09-28", "0.18"}
         }) {
        auto* event = input.add_settlement_margins();
        event->set_trading_day(day);
        event->mutable_rate()->set_units(d(rate).raw());
        event->set_source("Test-only explicit model assumption");
    }
    return input;
}

} // namespace

TEST(Backtest, SettlementMarginRevaluesAfterPnlAndSurvivesTaskRestart)
{
    const auto input = settlement_margin_input();
    const auto result = backtest::run(input);
    ASSERT_NO_THROW(protocol::validate_backtest_result(input, result));
    const auto json = protocol::decode_backtest_result(result);
    const auto& third = json.at("settlements").at(2);
    EXPECT_EQ(third.at("balance"), "10008");
    EXPECT_EQ(third.at("fees"), "2");
    EXPECT_EQ(third.at("margin_revaluation").at("previous_rate"), "0.14");
    EXPECT_EQ(third.at("margin_revaluation").at("margin_before"), "142.8");
    EXPECT_EQ(third.at("occupancy").at("margin"), "163.2");
    EXPECT_EQ(third.at("occupancy").at("available"), "9844.8");
    const auto& last = json.at("settlements").at(3);
    EXPECT_EQ(last.at("margin_revaluation").at("previous_rate"), "0.17");
    EXPECT_EQ(last.at("margin_revaluation").at("margin_before"), "175.1");
    EXPECT_EQ(last.at("occupancy").at("margin"), "185.4");
    EXPECT_EQ(last.at("balance"), "10018");
    EXPECT_EQ(last.at("fees"), "2");
    EXPECT_EQ(last.at("contracts").at(0).at("position_quantity"), "1");
    const auto exposure = portfolio_exposure(input, result);
    EXPECT_EQ(exposure.margin, d("185.4"));
    EXPECT_EQ(exposure.margin, exposure.recorded_margin);
    const auto audit = reconcile_backtest(input, result);
    ASSERT_EQ(audit.state, AccountReconciliation::State::available);
    for (const auto& check : audit.final_checks)
        EXPECT_EQ(check.expected, check.recorded) << check.metric;

    TaskDirectory directory;
    {
        tasks::Store store(
            asterion::research::task_domain(),
            directory.path,
            tasks::Identity{"task"});
        tasks::submit(store, "margin", input);
        const auto token = store.commit(store.claim("margin")).token();
        store.commit(tasks::finish(store, "margin", token, result));
    }
    tasks::Store restored(
        asterion::research::task_domain(),
        directory.path,
        tasks::Identity{"task"});
    const auto fixed = restored.get("margin");
    EXPECT_EQ(fixed.input().SerializeAsString(), input.SerializeAsString());
    EXPECT_EQ(tasks::result(restored, "margin").SerializeAsString(), result.SerializeAsString());
    const auto execution = protocol::task_execution(fixed, std::string(64, 'a'));
    ASSERT_EQ(execution.backtest().settlement_margins_size(), 2);
    EXPECT_EQ(
        execution.backtest().settlement_margins(1).SerializeAsString(),
        input.settlement_margins(1).SerializeAsString());
}

TEST(Backtest, SettlementMarginRejectsAmbiguousScheduleAndMismatchedEvidence)
{
    auto input = settlement_margin_input();
    input.mutable_paper()
        ->mutable_contracts(0)
        ->mutable_cost_schedule()
        ->mutable_versions()
        ->RemoveLast();
    EXPECT_THROW(protocol::decode_backtest(input), std::invalid_argument);
    input = settlement_margin_input();
    input.mutable_settlement_margins(0)->set_trading_day("2026-09-24");
    EXPECT_THROW(protocol::decode_backtest(input), std::invalid_argument);
    input = settlement_margin_input();
    *input.add_strategies() = testing_support::moving_average(1, 3);
    EXPECT_THROW(protocol::decode_backtest(input), std::invalid_argument);
    input = settlement_margin_input();
    const auto result = backtest::run(input);
    auto changed = result;
    changed.mutable_settlements(2)->clear_margin_revaluation();
    EXPECT_THROW(protocol::validate_backtest_result(input, changed), std::invalid_argument);
    changed = result;
    changed.mutable_settlements(2)->mutable_margin_revaluation()->mutable_rate()->set_units(
        d("0.19").raw());
    EXPECT_THROW(protocol::validate_backtest_result(input, changed), std::invalid_argument);
}

TEST(Backtest, HistoricalMarginsPinBothPhasesThroughRestartAndExecutionProjection)
{
    TaskDirectory directory;
    const auto snapshot = test::seed_shfe_margins(directory.path);
    history_files::TableStore tables(directory.path / "tables");
    data::proto::HistoricalMarginQuery query;
    query.set_snapshot(snapshot);
    query.set_contract(shfe::contract);
    std::vector<MarketBar> bars;
    int price = 100;
    for (const auto* day : shfe::days) {
        query.add_trading_days(day);
        const auto value = std::to_string(price++);
        bars.push_back(test::flat(day, trading_day_close_ns(day), value.c_str()));
    }
    const auto fixed = data::historical_margins(tables, query);
    auto input = testing_support::backtest_input(
        test::dataset(bars, {}, test::contract("SHFE", "au2512", "au", "2025-12"), 0));
    *input.mutable_strategies(0) = testing_support::moving_average(1, 2);
    auto* cost = input.mutable_paper()
                     ->mutable_contracts(0)
                     ->mutable_cost_schedule()
                     ->mutable_versions(0)
                     ->mutable_values();
    cost->mutable_margin_per_lot()->set_units(0);
    cost->mutable_margin_rate()->set_units(d("0.1").raw());
    protocol::apply_historical_margins(input, fixed);
    input.set_dataset_revision(protocol::dataset_revision(input.paper()));
    const auto result = backtest::run(input);
    protocol::validate_backtest_result(input, result);
    EXPECT_EQ(result.settlements(1).margin_revaluation().previous_rate().units(), d("0.14").raw());
    EXPECT_EQ(result.settlements(1).margin_revaluation().rate().units(), d("0.16").raw());
    EXPECT_EQ(
        input.paper().contracts(0).cost_schedule().versions(2).values().margin_rate().units(),
        d("0.16").raw());
    EXPECT_EQ(
        input.paper().contracts(0).cost_schedule().versions(2).values().open_fee().units(),
        d("2").raw());
    const auto task_directory = directory.path / "task";
    std::filesystem::create_directory(task_directory);
    {
        tasks::Store store(asterion::research::task_domain(), task_directory, {"task"});
        tasks::submit(store, "official-margin", input);
        const auto token = store.commit(store.claim("official-margin")).token();
        store.commit(tasks::finish(store, "official-margin", token, result));
    }
    tasks::Store restored(asterion::research::task_domain(), task_directory, {"task"});
    const auto task = restored.get("official-margin");
    EXPECT_EQ(task.input().SerializeAsString(), input.SerializeAsString());
    const auto execution = protocol::task_execution(task, sha256_bytes(input.SerializeAsString()));
    EXPECT_EQ(execution.historical_margins().SerializeAsString(), fixed.SerializeAsString());
    EXPECT_EQ(
        protocol::decode_backtest(input).at("historical_margins"),
        protocol::decode_historical_margins(fixed));
    input.mutable_settlement_margins(1)->mutable_rate()->set_units(d("0.18").raw());
    EXPECT_THROW(protocol::decode_backtest(input), std::invalid_argument);
}

TEST(TaskConfiguration, ReadsPolicyWithoutOverwritingMissingOrInvalidFiles)
{
    TaskDirectory directory;
    const auto file = directory.path / "research.json";
    EXPECT_THROW(tasks::load_task_concurrency(file), std::runtime_error);
    EXPECT_FALSE(std::filesystem::exists(file));
    replace_file_durably(file, R"({"task_concurrency":2})");
    EXPECT_EQ(tasks::load_task_concurrency(file), 2U);
    const std::string invalid = R"({"task_concurrency":0})";
    replace_file_durably(file, invalid);
    try {
        tasks::load_task_concurrency(file);
        FAIL() << "invalid concurrency accepted";
    } catch (const std::invalid_argument& error) {
        EXPECT_NE(std::string(error.what()).find(file.string()), std::string::npos);
    }
    std::ifstream input(file);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(input), {}), invalid);
}

TEST(ResearchDataDiscovery, ResolvesStableOwnerWithoutPersistingItsLocator)
{
    node::proto::Status status;
    status.set_phase(node::proto::Status::READY);
    status.set_node_id("node-one");
    status.set_instance_id("generation-one");
    auto* data = status.add_services();
    data->set_kind(node::proto::DATA);
    data->set_id("data-one");
    data->set_state("running");
    data->set_endpoint("/first/data.sock");
    const auto first = tasks::resolve_research_data(status, {});
    EXPECT_EQ(first.owner.node_id(), "node-one");
    EXPECT_EQ(first.owner.service_id(), "data-one");
    status.set_instance_id("generation-two");
    data->set_endpoint("/second/data.sock");
    const auto second = tasks::resolve_research_data(status, first.owner);
    EXPECT_EQ(first.owner.SerializeAsString(), second.owner.SerializeAsString());
    EXPECT_NE(first.endpoint, second.endpoint);
}

TEST(ResearchDataDiscovery, AmbiguousOrUnavailableOwnerCannotSilentlyChangeSource)
{
    node::proto::Status status;
    status.set_phase(node::proto::Status::READY);
    status.set_node_id("node-one");
    for (const auto* id : {"data-one", "data-two"}) {
        auto* data = status.add_services();
        data->set_kind(node::proto::DATA);
        data->set_id(id);
        data->set_state("running");
        data->set_endpoint(std::string("/") + id + ".sock");
    }
    const auto rejects = [&](const data::proto::DataOwner& owner, ErrorCode expected) {
        try {
            (void)tasks::resolve_research_data(status, owner);
            ADD_FAILURE() << "unexpected data owner substitution";
        } catch (const Error& error) {
            EXPECT_EQ(error.code(), expected);
        }
    };
    rejects({}, ErrorCode::conflict);
    data::proto::DataOwner fixed;
    fixed.set_node_id("node-one");
    fixed.set_service_id("data-one");
    EXPECT_EQ(tasks::resolve_research_data(status, fixed).owner.service_id(), "data-one");
    status.mutable_services(0)->set_state("stopped");
    rejects(fixed, ErrorCode::unavailable);
    status.mutable_services()->DeleteSubrange(0, 1);
    rejects(fixed, ErrorCode::not_found);
    fixed.set_node_id("another-node");
    rejects(fixed, ErrorCode::conflict);
    status.set_phase(node::proto::Status::RECOVERY_REQUIRED);
    rejects({}, ErrorCode::unavailable);
}

TEST(ResearchFactsDiscovery, ExactOwnerIsResolvedWithoutADataServiceOrSubstitution)
{
    node::proto::Status status;
    status.set_phase(node::proto::Status::READY);
    status.set_node_id("node-one");
    auto* service = status.add_services();
    service->set_kind(node::proto::TRADING);
    service->set_id("trading-one");
    service->set_state("running");
    service->set_endpoint("/trading.sock");
    resource::proto::Owner owner;
    owner.set_domain("trading");
    owner.set_node_id("node-one");
    owner.set_service_id("trading-one");
    EXPECT_EQ(tasks::resolve_research_facts(status, owner).endpoint, "/trading.sock");
    service->set_state("stopped");
    EXPECT_THROW((void)tasks::resolve_research_facts(status, owner), Error);
    service->set_state("running");
    service->set_id("trading-two");
    EXPECT_THROW((void)tasks::resolve_research_facts(status, owner), Error);
    owner.set_node_id("other-node");
    EXPECT_THROW((void)tasks::resolve_research_facts(status, owner), Error);
}

TEST(ResearchFactsDiscovery, ProgramIdentityComesFromTheObservedResearchProcess)
{
    node::proto::Status status;
    status.set_phase(node::proto::Status::READY);
    status.set_node_id("node-one");
    auto* service = status.add_services();
    service->set_kind(node::proto::RESEARCH);
    service->set_id("research-one");
    service->set_state("running");
    service->set_pid(123);
    service->set_artifact(std::string(64, 'a'));
    const auto program = tasks::resolve_research_program(status, "research-one", 123);
    EXPECT_EQ(program.owner.node_id(), "node-one");
    EXPECT_EQ(program.owner.domain(), "research");
    EXPECT_EQ(program.artifact, std::string(64, 'a'));
    EXPECT_THROW((void)tasks::resolve_research_program(status, "research-one", 124), Error);
    service->set_artifact("");
    EXPECT_THROW((void)tasks::resolve_research_program(status, "research-one", 123), Error);
}

TEST(ResearchDataDiscovery, FixedInputDigestAndExecutionRetainLogicalOwner)
{
    auto spec = input();
    spec.mutable_data_owner()->set_node_id("node-one");
    spec.mutable_data_owner()->set_service_id("data-one");
    task::proto::Task task;
    *task.mutable_input() = spec;
    const auto original = sha256_bytes(spec.SerializeAsString());
    const auto execution = protocol::task_execution(task, original);
    ASSERT_TRUE(execution.has_data_owner());
    EXPECT_EQ(execution.data_owner().SerializeAsString(), spec.data_owner().SerializeAsString());
    EXPECT_EQ(execution.input_sha256(), original);
    spec.mutable_data_owner()->set_service_id("data-two");
    EXPECT_NE(sha256_bytes(spec.SerializeAsString()), original);
    EXPECT_EQ(execution.data_owner().service_id(), "data-one");
}

TEST_F(TaskProcess, DistinctTasksResolveDistinctDataOwnersAndRetainThemAcrossRestart)
{
    const auto first = process_input();
    TaskDirectory other;
    const auto selected = test::seed_history(
        other.path,
        {200, 201, 202, 201, 200, 201, 203},
        "second-owner",
        {"2024-01-01"},
        {},
        "rb",
        1,
        210,
        "test.confirmed.v1",
        "2026-10",
        100,
        "other-data");
    auto second = input();
    {
        data::Store store(other.path, "other-data");
        auto query = protocol::encode_bar_dataset_request(
            {{"source_dataset_ids", selected.at("source_dataset_ids")},
                {"daily", selected.at("daily")},
                {"begin_day", ""},
                {"end_day", ""},
                {"contract", "SHFE/rb/2026-10"}});
        *second.mutable_paper()->mutable_contracts(0)->mutable_dataset() =
            data::resolve_bar_dataset(store.tables(), store.sources(query));
        second.set_dataset_revision(protocol::dataset_revision(second.paper()));
    }
    auto other_process = std::make_unique<ChildProcess>(
        ASTERION_DATA_SERVICE_PATH,
        std::vector<std::string>{
            "--directory",
            other.path.string(),
            "--session",
            "other-data",
            "--node-id",
            "fixture-node",
            "--endpoint",
            endpoint + ".other-data",
            "--internal-endpoint",
            endpoint + ".other-internal"
        });
    discovery.reset();
    discovery = std::make_unique<testing_support::DataDiscovery>(
        endpoint + ".discovery",
        "historical-data",
        endpoint + ".data",
        std::vector<std::pair<std::string, std::string>>{{"other-data", endpoint + ".other-data"}});
    discovery->research("task", service->id(), fixture_program_artifact);
    protocol::DataClient other_client(endpoint + ".other-data", "other-data");
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    for (;;) {
        try {
            data::proto::DataRequest heartbeat;
            heartbeat.mutable_heartbeat();
            if (other_client.call(heartbeat, 1s).health().initialized())
                break;
        } catch (const Error&) {
            ASSERT_FALSE(other_process->exited());
        }
        ASSERT_LT(std::chrono::steady_clock::now(), deadline);
        std::this_thread::sleep_for(20ms);
    }
    const auto submission = [&](const std::string& id,
                                const backtest::proto::BacktestInput& spec,
                                const std::string& owner) {
        research::proto::ResearchRequest request;
        auto* submit = request.mutable_submit();
        submit->set_id(id);
        submit->mutable_data_owner()->set_node_id("fixture-node");
        submit->mutable_data_owner()->set_service_id(owner);
        task::proto::Task fixed;
        *fixed.mutable_input() = spec;
        *submit->mutable_backtest() = protocol::task_execution(fixed, "").backtest();
        return request;
    };
    auto ambiguous = submission("ambiguous", first, "");
    EXPECT_THROW(call(ambiguous), std::runtime_error);
    EXPECT_EQ(
        call(submission("first-owner", first, "historical-data")).task().state(),
        task_wire::QUEUED);
    EXPECT_EQ(
        call(submission("second-owner", second, "other-data")).task().state(),
        task_wire::QUEUED);
    for (const auto* id : {"first-owner", "second-owner"}) {
        EXPECT_EQ(
            testing_support::wait_task(
                [&](auto request) {
                    return call(request);
                },
                id)
                .state(),
            task_wire::SUCCEEDED);
    }
    const auto saved = [&](const std::string& id) {
        research::proto::ResearchRequest request;
        request.mutable_get()->set_id(id);
        return call(request).task().input();
    };
    const auto one = saved("first-owner"), two = saved("second-owner");
    EXPECT_EQ(one.data_owner().service_id(), "historical-data");
    EXPECT_EQ(two.data_owner().service_id(), "other-data");
    EXPECT_NE(one.dataset_revision(), two.dataset_revision());
    const auto registered = [&](protocol::DataClient& data,
                                const std::string& id,
                                const backtest::proto::BacktestInput& fixed) {
        data::proto::DataRequest request;
        auto* consumer = request.mutable_retention()->mutable_get();
        consumer->mutable_owner()->set_domain("research");
        consumer->mutable_owner()->set_node_id("fixture-node");
        consumer->mutable_owner()->set_service_id("task");
        consumer->set_kind("backtest");
        consumer->set_id(id);
        consumer->set_version(sha256_bytes(fixed.SerializeAsString()));
        return data.call(request).retention();
    };
    protocol::DataClient first_client(endpoint + ".data", "historical-data");
    const auto first_hold = registered(first_client, "first-owner", one);
    const auto second_hold = registered(other_client, "second-owner", two);
    EXPECT_EQ(first_hold.phase(), data::proto::DataRetentionRecord::CONFIRMED);
    EXPECT_EQ(second_hold.phase(), data::proto::DataRetentionRecord::CONFIRMED);
    EXPECT_EQ(first_hold.retention().source().service_id(), "historical-data");
    EXPECT_EQ(second_hold.retention().source().service_id(), "other-data");
    EXPECT_TRUE(std::filesystem::is_regular_file(directory.path / "first-owner/data-retention.pb"));
    research::proto::ResearchRequest original;
    original.mutable_get()->set_id("first-owner");
    const auto accepted = call(original).task();
    research::proto::ResearchRequest reuse;
    auto* submit = reuse.mutable_submit();
    submit->set_id("wrong-fixed-owner");
    submit->mutable_data_owner()->set_service_id("other-data");
    auto* fixed = submit->mutable_backtest_reuse();
    fixed->set_choice(backtest::proto::BacktestReuse::FIXED);
    fixed->set_source_id("first-owner");
    fixed->set_source_result_digest(accepted.result_digest());
    fixed->set_source_input_sha256(sha256_bytes(one.SerializeAsString()));
    EXPECT_THROW(call(reuse), std::runtime_error);
    reuse.mutable_submit()->set_id("fixed-owner");
    reuse.mutable_submit()->mutable_data_owner()->Clear();
    EXPECT_EQ(call(reuse).task().state(), task_wire::QUEUED);
    EXPECT_EQ(
        testing_support::wait_task(
            [&](auto request) {
                return call(request);
            },
            "fixed-owner")
            .state(),
        task_wire::SUCCEEDED);
    EXPECT_EQ(
        saved("fixed-owner").data_owner().SerializeAsString(),
        one.data_owner().SerializeAsString());
    service.reset();
    data_service.reset();
    other_process.reset();
    start();
    EXPECT_EQ(saved("first-owner").SerializeAsString(), one.SerializeAsString());
    EXPECT_EQ(saved("second-owner").SerializeAsString(), two.SerializeAsString());
}

TEST(TaskPreparation, ExplicitResumeReusesOwnedInputAndAllowsIndependentAcceptedWork)
{
    TaskDirectory directory;
    {
        tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
        auto draft = task_submission(store, "pending", input());
        store.admit_submission(draft);
        draft.prepare_files(); // Stop before the task index commit.
        EXPECT_EQ(store.list().tasks_size(), 0);
    }
    const auto payload = directory.path / "pending/input.pb";
    const auto before = sha256_file(payload);
    tasks::Store restored(asterion::research::task_domain(), directory.path, {"task"});
    EXPECT_EQ(restored.list().capacity().uncommitted(), 1U);
    tasks::submit(restored, "independent", input());
    auto draft = restored.resume_submission("pending");
    restored.admit_submission(draft);
    const auto committed = restored.commit(restored.register_submission(draft)).task();
    EXPECT_EQ(committed.id(), "pending");
    EXPECT_EQ(committed.submission_sequence(), 2U);
    EXPECT_EQ(restored.list().capacity().uncommitted(), 0U);
    EXPECT_EQ(restored.list().capacity().retained_tasks(), 2U);
    EXPECT_EQ(sha256_file(payload), before);
    EXPECT_EQ(restored.get("pending").input().SerializeAsString(), input().SerializeAsString());
    auto repeated = restored.resume_submission("pending");
    restored.admit_submission(repeated);
    EXPECT_FALSE(restored.register_submission(repeated).needs_write());
}

TEST(TaskPreparation, MissingPayloadKeepsRecoveryMetadataAndUnknownOrChangedRecordsAreRejected)
{
    TaskDirectory directory;
    {
        tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
        auto draft = task_submission(store, "pending", input());
        store.admit_submission(draft);
        draft.prepare_files();
    }
    const auto record_file = directory.path / "pending/prepared.pb";
    const auto payload = directory.path / "pending/input.pb";
    const auto digest = sha256_file(payload);
    std::filesystem::remove(
        payload); // Isolated simulation: stopped before atomic payload publication.
    tasks::Store restored(asterion::research::task_domain(), directory.path, {"task"});
    const auto prepared = restored.prepared_submission_record("pending");
    EXPECT_EQ(prepared.record().input_digest(), digest);
    EXPECT_EQ(prepared.execution().input_sha256(), digest);
    EXPECT_TRUE(prepared.execution().has_backtest());
    EXPECT_THROW(restored.resume_submission("pending"), std::invalid_argument);
    std::filesystem::create_directory(directory.path / "unknown");
    EXPECT_THROW(restored.prepared_submission_record("unknown"), std::invalid_argument);
    EXPECT_THROW(restored.prepared_submission_record("."), std::invalid_argument);
    EXPECT_THROW(restored.prepared_submission_record(".."), std::invalid_argument);
    auto wrong = prepared;
    wrong.set_instance_id("another-owner");
    replace_file_durably(record_file, wrong.SerializeAsString());
    const auto before = sha256_file(record_file);
    EXPECT_THROW(restored.prepared_submission_record("pending"), std::invalid_argument);
    EXPECT_EQ(sha256_file(record_file), before);
    EXPECT_FALSE(std::filesystem::exists(payload));
}

TEST_F(TaskProcess, PreparedReferenceRecoversWithAndWithoutPublishedInputAndReplaysIdempotently)
{
    auto spec = process_input();
    spec.mutable_data_owner()->set_node_id("fixture-node");
    spec.mutable_data_owner()->set_service_id("historical-data");
    for (const bool missing_input : {false, true}) {
        SCOPED_TRACE(missing_input);
        const std::string id = missing_input ? "prepared-missing-input" : "prepared-input";
        service.reset();
        data::proto::DataRetention retention;
        {
            tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
            auto pending = task_submission(store, id, spec);
            preserve_program();
            asterion::tasks::set_program_artifact(pending, preserved_program);
            store.admit_submission(pending);
            pending.prepare_files();
            tasks::ResearchDataRoute route{spec.data_owner(), endpoint + ".data", "fixture-node"};
            retention = tasks::research_retention(pending.execution(), route, "task", id);
            tasks::save_research_retention(directory.path / id, retention);
            protocol::DataClient data(endpoint + ".data", "historical-data");
            data::proto::DataRequest prepare;
            *prepare.mutable_retention()->mutable_prepare() = retention;
            EXPECT_EQ(
                data.call(prepare).retention().phase(),
                data::proto::DataRetentionRecord::PREPARED);
            // Discard the reply and stop before the Research task index commit.
        }
        const auto payload = directory.path / id / "input.pb";
        const auto before = sha256_file(payload);
        if (missing_input)
            std::filesystem::remove(payload); // Isolated crash-before-payload-publication state.
        start();
        research::proto::ResearchRequest recover;
        recover.mutable_recover_submission()->set_id(id);
        EXPECT_EQ(call(recover, "", 10s).task().state(), task_wire::QUEUED);
        EXPECT_EQ(
            testing_support::wait_task(
                [&](auto request) {
                    return call(request);
                },
                id)
                .state(),
            task_wire::SUCCEEDED);
        EXPECT_EQ(call(recover).task().state(), task_wire::SUCCEEDED);
        EXPECT_EQ(sha256_file(payload), before);
        research::proto::ResearchRequest get;
        get.mutable_get()->set_id(id);
        EXPECT_EQ(call(get).task().input().SerializeAsString(), spec.SerializeAsString());
        protocol::DataClient data(endpoint + ".data", "historical-data");
        data::proto::DataRequest query;
        *query.mutable_retention()->mutable_get() = retention.consumer();
        const auto confirmed = data.call(query).retention();
        EXPECT_EQ(confirmed.phase(), data::proto::DataRetentionRecord::CONFIRMED);
        EXPECT_EQ(confirmed.retention().SerializeAsString(), retention.SerializeAsString());
        research::proto::ResearchRequest list;
        list.mutable_list()->set_limit(200);
        EXPECT_EQ(call(list).tasks().capacity().uncommitted(), 0U);
        if (missing_input) {
            data_service.reset();
            EXPECT_EQ(call(recover).task().state(), task_wire::SUCCEEDED);
            EXPECT_EQ(call(get).task().input().SerializeAsString(), spec.SerializeAsString());
        }
    }
}

namespace {

resource::proto::Object prepared_resource(const task::proto::PreparedTaskSubmission& value)
{
    resource::proto::Object resource;
    resource.mutable_owner()->set_domain("research");
    resource.mutable_owner()->set_node_id("fixture-node");
    resource.mutable_owner()->set_service_id(value.instance_id());
    resource.set_kind("backtest");
    resource.set_id(value.record().task().id());
    resource.set_version(value.record().input_digest());
    return resource;
}

} // namespace

TEST(TaskPreparation, AbandonmentPersistsBeyondPayloadCleanupAndCannotReleaseAcceptedTasks)
{
    TaskDirectory directory;
    {
        tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
        auto draft = task_submission(store, "abandoned", input());
        store.admit_submission(draft);
        draft.prepare_files();
    }
    resource::proto::Object released;
    {
        tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
        const auto pending = store.prepared_submission_record("abandoned");
        released = prepared_resource(pending);
        auto decision = store.abandon_preparation(pending, released);
        EXPECT_EQ(store.list().capacity().uncommitted(), 1U);
        decision.persist();
        EXPECT_EQ(store.list().capacity().uncommitted(), 1U);
        store.confirm(decision);
        EXPECT_EQ(store.list().capacity().uncommitted(), 0U);
        ASSERT_TRUE(store.abandoned_preparation("abandoned"));
        EXPECT_EQ(
            store.abandoned_preparation("abandoned")->SerializeAsString(),
            released.SerializeAsString());
        EXPECT_FALSE(store.commit(store.abandon_preparation(pending, released)).needs_write());
        auto resurrection = store.resume_submission("abandoned");
        EXPECT_THROW(store.admit_submission(resurrection), Error);
        tasks::submit(store, "accepted", input());
        const auto accepted = store.prepared_submission_record("accepted");
        EXPECT_THROW(store.abandon_preparation(accepted, prepared_resource(accepted)), Error);
        EXPECT_EQ(store.describe("accepted").state(), task::proto::QUEUED);
    }
    const auto inspect_cold = [&](bool payload_present) {
        unsigned accepted = 0;
        unsigned abandoned = 0;
        const auto catalogue = tasks::Store::inspect_backup(
            asterion::research::task_domain(),
            directory.path,
            {"task"},
            [&](const auto& record, const auto&) {
                EXPECT_EQ(record.task().id(), "accepted");
                ++accepted;
            },
            {},
            [&](const auto& evidence, const auto& decision) {
                EXPECT_EQ(evidence.record().task().id(), "abandoned");
                EXPECT_EQ(evidence.execution().input_sha256(), released.version());
                EXPECT_EQ(decision.SerializeAsString(), released.SerializeAsString());
                ++abandoned;
            });
        EXPECT_EQ(accepted, 1U);
        EXPECT_EQ(abandoned, 1U);
        EXPECT_EQ(
            std::ranges::find(catalogue.files, std::filesystem::path("abandoned/input.pb"))
                != catalogue.files.end(),
            payload_present);
    };
    inspect_cold(true);
    std::filesystem::remove_all(
        directory.path / "abandoned"); // Isolated payload cleanup; authority stays in SQLite.
    inspect_cold(false);
    {
        tasks::Store restored(asterion::research::task_domain(), directory.path, {"task"});
        EXPECT_EQ(restored.list().capacity().uncommitted(), 0U);
        ASSERT_TRUE(restored.abandoned_preparation("abandoned"));
        EXPECT_EQ(
            restored.abandoned_preparation("abandoned")->SerializeAsString(),
            released.SerializeAsString());
        auto reuse = task_submission(restored, "abandoned", input());
        EXPECT_THROW(restored.admit_submission(reuse), Error);
        EXPECT_EQ(restored.describe("accepted").state(), task::proto::QUEUED);
    }
    // A cleanup cannot erase which fixed sample and candidate were actually prepared.
    sqlite::Database database(directory.path / "tasks.sqlite", sqlite::Database::Access::read_only);
    sqlite::Database::Statement history(
        database,
        "SELECT evidence FROM preparation_decisions WHERE id='abandoned'");
    ASSERT_TRUE(history.step());
    task::proto::PreparedTaskSubmission evidence;
    ASSERT_TRUE(evidence.ParseFromString(history.blob(0)));
    EXPECT_EQ(evidence.execution().input_sha256(), released.version());
    EXPECT_TRUE(evidence.execution().has_backtest());
}

TEST(TaskPreparation, FailedAbandonmentCommitCannotAuthorizeRelease)
{
    TaskDirectory directory;
    {
        tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
        auto draft = task_submission(store, "pending", input());
        store.admit_submission(draft);
        draft.prepare_files();
    }
    {
        tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
        const auto pending = store.prepared_submission_record("pending");
        sqlite::fail_next_commits_for_testing(1);
        EXPECT_THROW(
            store.commit(store.abandon_preparation(pending, prepared_resource(pending))),
            std::runtime_error);
        EXPECT_FALSE(store.abandoned_preparation("pending"));
        EXPECT_EQ(store.list().capacity().uncommitted(), 1U);
    }
    tasks::Store restored(asterion::research::task_domain(), directory.path, {"task"});
    EXPECT_FALSE(restored.abandoned_preparation("pending"));
    EXPECT_EQ(restored.list().capacity().uncommitted(), 1U);
    const auto pending = restored.prepared_submission_record("pending");
    restored.commit(restored.abandon_preparation(pending, prepared_resource(pending)));
    EXPECT_TRUE(restored.abandoned_preparation("pending"));
    EXPECT_EQ(restored.list().capacity().uncommitted(), 0U);
}

TEST(TaskRetirement, DurableLifecycleKeepsEvidenceAndCannotResumeOrReuseTheIdentity)
{
    TaskDirectory directory;
    resource::proto::Object object;
    {
        tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
        tasks::submit(store, "retired-run", input());
        EXPECT_EQ(
            store.retirement("retired-run", "fixture-node").state(),
            task::proto::TaskRetirement::BLOCKED);
        store.commit(store.cancel("retired-run"));
        const auto preview = store.retirement("retired-run", "fixture-node");
        ASSERT_EQ(preview.state(), task::proto::TaskRetirement::AVAILABLE);
        object = preview.object();
        const auto before = sha256_file(directory.path / "retired-run/input.pb");
        auto decision = store.retire(object);
        EXPECT_FALSE(store.describe("retired-run").has_retirement());
        EXPECT_EQ(
            store.retirement("retired-run", "fixture-node").state(),
            task::proto::TaskRetirement::BLOCKED);
        decision.persist();
        store.confirm(decision);
        EXPECT_TRUE(store.describe("retired-run").has_retirement());
        EXPECT_EQ(sha256_file(directory.path / "retired-run/input.pb"), before);
        EXPECT_EQ(
            store.get("retired-run").input().SerializeAsString(),
            input().SerializeAsString());
        EXPECT_FALSE(store.commit(store.retire(object)).needs_write());
        EXPECT_THROW(store.retry("retired-run"), Error);
        auto reuse = task_submission(store, "retired-run", input());
        EXPECT_THROW(store.admit_submission(reuse), Error);
    }
    tasks::Store restored(asterion::research::task_domain(), directory.path, {"task"});
    EXPECT_EQ(
        restored.retirement("retired-run", "fixture-node").state(),
        task::proto::TaskRetirement::RETIRED);
    EXPECT_EQ(
        restored.describe("retired-run").retirement().SerializeAsString(),
        object.SerializeAsString());
    EXPECT_EQ(restored.describe("retired-run").state(), task::proto::CANCELLED);
}

TEST(TaskRetirement, NewDerivationAndRetirementShareOneAdmissionBoundary)
{
    TaskDirectory directory;
    tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
    tasks::submit(store, "parent", input());
    const auto token = store.commit(store.claim("parent")).token();
    store.commit(tasks::finish(store, "parent", token, backtest::run(input())));
    const auto object = store.retirement("parent", "fixture-node").object();
    auto child = task_submission(store, "child", input());
    task::proto::TaskDerivation origin;
    origin.set_source_id("parent");
    origin.set_source_result_digest(store.describe("parent").result_digest());
    origin.set_data_choice("fixed");
    child.set_derivation(origin);
    store.admit_submission(child);
    EXPECT_THROW(store.retire(object), Error);
    child.prepare_files();
    store.commit(store.register_submission(child));
    ASSERT_EQ(store.retirement("parent", "fixture-node").consumers_size(), 1);
    EXPECT_THROW(store.retire(object), Error);
    store.commit(store.cancel("child"));
    store.commit(store.retire(store.retirement("child", "fixture-node").object()));
    auto retirement = store.retire(object);
    auto later = task_submission(store, "later", input());
    later.set_derivation(origin);
    EXPECT_THROW(store.admit_submission(later), Error);
    retirement.persist();
    store.confirm(retirement);
    EXPECT_THROW(store.admit_submission(later), Error);
    EXPECT_EQ(store.list().tasks_size(), 2);
}

TEST(TaskRetirement, FailedPersistenceCannotAuthorizeSourceRelease)
{
    TaskDirectory directory;
    {
        tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
        tasks::submit(store, "candidate", input());
        store.commit(store.cancel("candidate"));
        const auto object = store.retirement("candidate", "fixture-node").object();
        auto retirement = store.retire(object);
        sqlite::fail_next_commits_for_testing(1);
        EXPECT_THROW(retirement.persist(), std::runtime_error);
        sqlite::fail_next_commits_for_testing(0);
        EXPECT_FALSE(store.describe("candidate").has_retirement());
    }
    tasks::Store restored(asterion::research::task_domain(), directory.path, {"task"});
    EXPECT_EQ(
        restored.retirement("candidate", "fixture-node").state(),
        task::proto::TaskRetirement::AVAILABLE);
}

TEST(TaskInputReference, IdenticalInputUsesOwnedImmutableStorageAndPreparedRecoveryKeepsTheLink)
{
    TaskDirectory directory;
    const auto spec = input();
    std::string parent_digest;
    {
        tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
        tasks::submit(store, "reference-parent", spec);
        const auto token = store.commit(store.claim("reference-parent")).token();
        store.commit(tasks::finish(store, "reference-parent", token, backtest::run(spec)));
        parent_digest = store.describe("reference-parent").result_digest();
        auto child = task_submission(store, "reference-child", spec);
        task::proto::TaskDerivation origin;
        origin.set_source_id("reference-parent");
        origin.set_source_result_digest(parent_digest);
        origin.set_data_choice("fixed");
        child.set_derivation(origin);
        store.admit_submission(child);
        child.prepare_files();
        EXPECT_TRUE(
            std::filesystem::equivalent(
                directory.path / "reference-parent/input.pb",
                directory.path / "reference-child/input.pb"));
        EXPECT_EQ(store.list().tasks_size(), 1);
        // Simulated interruption after complete immutable publication, before admission.
    }
    tasks::Store restored(asterion::research::task_domain(), directory.path, {"task"});
    auto child = restored.resume_submission("reference-child");
    restored.admit_submission(child);
    restored.commit(restored.register_submission(child));
    EXPECT_EQ(restored.list().tasks_size(), 2);
    EXPECT_EQ(restored.describe("reference-child").state(), task::proto::QUEUED);
    EXPECT_EQ(restored.describe("reference-parent").result_digest(), parent_digest);
    EXPECT_TRUE(
        std::filesystem::equivalent(
            directory.path / "reference-parent/input.pb",
            directory.path / "reference-child/input.pb"));
    EXPECT_EQ(
        restored.get("reference-child").input().SerializeAsString(),
        spec.SerializeAsString());
    // Ordinary in-place writers retain their single-link protection. They cannot
    // truncate shared business bytes, and credential helper rules are unchanged.
    const auto before = sha256_file(directory.path / "reference-child/input.pb");
    EXPECT_THROW(
        write_file_durably(directory.path / "reference-parent/input.pb", "damaged"),
        std::runtime_error);
    EXPECT_EQ(sha256_file(directory.path / "reference-child/input.pb"), before);
    EXPECT_EQ(restored.retirement("reference-parent", "fixture-node").consumers_size(), 1);
}

TEST(TaskInputReference, DifferentInputsDoNotAliasAndCorruptSourcesCannotBeRepublished)
{
    TaskDirectory directory;
    tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
    const auto spec = input();
    tasks::submit(store, "source", spec);
    const auto token = store.commit(store.claim("source")).token();
    store.commit(tasks::finish(store, "source", token, backtest::run(spec)));
    task::proto::TaskDerivation origin;
    origin.set_source_id("source");
    origin.set_source_result_digest(store.describe("source").result_digest());
    origin.set_data_choice("fixed");
    auto changed_spec = spec;
    *changed_spec.mutable_strategies(0) = testing_support::moving_average(1, 3, "2");
    auto changed = task_submission(store, "changed", changed_spec);
    changed.set_derivation(origin);
    store.admit_submission(changed);
    changed.prepare_files();
    store.commit(store.register_submission(changed));
    EXPECT_FALSE(
        std::filesystem::equivalent(
            directory.path / "source/input.pb",
            directory.path / "changed/input.pb"));
    const auto changed_hash = sha256_file(directory.path / "changed/input.pb");
    replace_file_durably(directory.path / "source/input.pb", "corrupt synthetic source");
    auto candidate = task_submission(store, "candidate", spec);
    candidate.set_derivation(origin);
    store.admit_submission(candidate);
    EXPECT_THROW(candidate.prepare_files(), std::invalid_argument);
    EXPECT_TRUE(store.abandon_submission(candidate));
    EXPECT_FALSE(std::filesystem::exists(directory.path / "candidate/input.pb"));
    EXPECT_TRUE(std::filesystem::exists(directory.path / "candidate/prepared.pb"));
    EXPECT_EQ(
        sha256_file(directory.path / "source/input.pb"),
        sha256_bytes("corrupt synthetic source"));
    EXPECT_EQ(sha256_file(directory.path / "changed/input.pb"), changed_hash);
}

TEST(TaskRetirement, DataAcquisitionKeepsAuthorizationAndRetiresDurably)
{
    TaskDirectory directory;
    const auto authorization = refresh_authorization("retired-acquisition");
    std::string digest;
    resource::proto::Object object;
    {
        tasks::Store store(asterion::data::task_domain(), directory.path, {"historical-data"});
        tasks::submit(store, authorization);
        EXPECT_EQ(
            store.retirement(authorization.task_id(), "fixture-node").state(),
            task::proto::TaskRetirement::BLOCKED);
        const auto token = store.commit(store.claim(authorization.task_id())).token();
        task::proto::TaskFinish finish;
        finish.set_id(authorization.task_id());
        finish.set_token(token);
        finish.mutable_table_update()->set_unpublished(1);
        store.commit(tasks::finish(store, finish));
        EXPECT_EQ(
            asterion::data::table_snapshot_holds(store, std::string(64, 'c')),
            std::vector<std::string>{authorization.task_id()});
        const auto preview = store.retirement(authorization.task_id(), "fixture-node");
        ASSERT_EQ(preview.state(), task::proto::TaskRetirement::AVAILABLE);
        object = preview.object();
        EXPECT_EQ(object.owner().domain(), "data");
        EXPECT_EQ(object.kind(), "table_update");
        digest = sha256_file(directory.path / authorization.task_id() / "input.pb");
        store.commit(store.retire(object));
        EXPECT_TRUE(asterion::data::table_snapshot_holds(store, std::string(64, 'c')).empty());
        EXPECT_FALSE(store.commit(store.retire(object)).needs_write());
        EXPECT_THROW((void)store.retry(authorization.task_id()), std::invalid_argument);
        EXPECT_TRUE(tasks::verified_result(store, authorization.task_id()).has_table_update());
    }
    tasks::Store restored(asterion::data::task_domain(), directory.path, {"historical-data"});
    const auto retired = restored.retirement(authorization.task_id(), "fixture-node");
    ASSERT_EQ(retired.state(), task::proto::TaskRetirement::RETIRED);
    EXPECT_EQ(retired.object().SerializeAsString(), object.SerializeAsString());
    EXPECT_EQ(restored.get(authorization.task_id()).download_authorization(), authorization.id());
    EXPECT_EQ(sha256_file(directory.path / authorization.task_id() / "input.pb"), digest);
    EXPECT_EQ(restored.describe(authorization.task_id()).state(), task::proto::SUCCEEDED);
}

TEST_F(TaskProcess, DataAcquisitionRetirementUsesOwnerProtocolAndSurvivesRestart)
{
    service.reset();
    data_service.reset();
    {
        data::Store initialize(warehouse.path, "historical-data");
    }
    std::filesystem::create_directories(warehouse.path / "acquisition-tasks");
    {
        tasks::Store store(
            asterion::data::task_domain(),
            warehouse.path / "acquisition-tasks",
            {"historical-data"});
        tasks::submit(store, refresh_authorization("rpc-retire"));
        store.commit(store.cancel("rpc-retire"));
    }
    start_data();
    protocol::DataClient client(endpoint + ".data", "historical-data");
    data::proto::DataRequest request;
    request.mutable_acquisition_task()->mutable_retirement()->set_id("rpc-retire");
    const auto preview = client.call(request, 5s).acquisition_task().retirement();
    ASSERT_EQ(preview.state(), task::proto::TaskRetirement::AVAILABLE);
    EXPECT_EQ(preview.object().owner().node_id(), "fixture-node");
    EXPECT_EQ(preview.object().owner().service_id(), "historical-data");
    EXPECT_EQ(preview.object().owner().domain(), "data");
    request.mutable_acquisition_task()->mutable_retirement()->set_execute(true);
    const auto retired = client.call(request, 5s).acquisition_task().retirement();
    ASSERT_EQ(retired.state(), task::proto::TaskRetirement::RETIRED);
    EXPECT_EQ(
        client.call(request, 5s).acquisition_task().retirement().SerializeAsString(),
        retired.SerializeAsString());
    data_service.reset();
    std::filesystem::remove(endpoint + ".data");
    std::filesystem::remove(endpoint + ".data-workers");
    start_data();
    EXPECT_EQ(
        client.call(request, 5s).acquisition_task().retirement().SerializeAsString(),
        retired.SerializeAsString());
    request.mutable_acquisition_task()->mutable_retry()->set_id("rpc-retire");
    EXPECT_THROW(client.call(request, 5s), Error);
    request.mutable_acquisition_task()->mutable_get()->set_id("rpc-retire");
    const auto evidence = client.call(request, 5s).acquisition_task().task();
    EXPECT_TRUE(evidence.has_retirement());
    EXPECT_EQ(evidence.download_authorization(), std::string(64, 'a'));
    EXPECT_EQ(evidence.state(), task::proto::CANCELLED);
}

TEST(TaskRetirement, ActiveUsageExcludesRetiredRecordsButHistoryKeepsEvidence)
{
    TaskDirectory directory, warehouse;
    const auto seeded = test::seed_history(warehouse.path, {100, 101}, "retired-usage");
    const auto id = seeded.at("source_dataset_ids").at(0).get<std::string>();
    data::Store data_store(warehouse.path, "historical-data");
    const auto data = data::resolve_bar_dataset(
        data_store.tables(),
        data_store.sources(composed_request({seeded})));
    auto backtest = input();
    *backtest.mutable_paper()->mutable_contracts(0)->mutable_dataset() = data;
    backtest.set_dataset_revision(protocol::dataset_revision(backtest.paper()));
    {
        tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
        tasks::submit(store, "usage", backtest);
        store.commit(store.cancel("usage"));
        EXPECT_EQ(store.history_usage(id, true).references_size(), 1);
        store.commit(store.retire(store.retirement("usage", "fixture-node").object()));
        EXPECT_EQ(store.history_usage(id).references_size(), 1);
        EXPECT_EQ(store.history_usage(id, true).references_size(), 0);
        EXPECT_EQ(store.get("usage").input().SerializeAsString(), backtest.SerializeAsString());
    }
    tasks::Store restored(asterion::research::task_domain(), directory.path, {"task"});
    EXPECT_EQ(restored.history_usage(id).references_size(), 1);
    EXPECT_EQ(restored.history_usage(id, true).references_size(), 0);
}

TEST(TaskRetirement, SnapshotHoldsRejectUncommittedAndCorruptAcquisitionEvidence)
{
    TaskDirectory directory;
    tasks::Store store(asterion::data::task_domain(), directory.path, {"historical-data"});
    const auto authorization = refresh_authorization("held-evidence");
    auto submission = task_submission(store, authorization);
    store.admit_submission(submission);
    submission.prepare_files();
    EXPECT_THROW(asterion::data::table_snapshot_holds(store, std::string(64, 'c')), Error);
    store.commit(store.register_submission(submission));
    EXPECT_EQ(
        asterion::data::table_snapshot_holds(store, std::string(64, 'c')),
        std::vector<std::string>{authorization.task_id()});
    store.commit(store.cancel(authorization.task_id()));
    const auto path = directory.path / authorization.task_id() / "input.pb";
    replace_file_durably(path, "SYNTHETIC_CORRUPT_INPUT");
    EXPECT_THROW(asterion::data::table_snapshot_holds(store, std::string(64, 'c')), std::exception);
    std::ifstream remaining(path, std::ios::binary);
    EXPECT_EQ(
        std::string(std::istreambuf_iterator<char>(remaining), {}),
        "SYNTHETIC_CORRUPT_INPUT");
    EXPECT_FALSE(store.describe(authorization.task_id()).has_retirement());
}

TEST_F(TaskProcess, TableRetirementRejectsChangedReviewedObjectScope)
{
    service.reset();
    data_service.reset();
    {
        data::Store initialize(warehouse.path, "historical-data");
    }
    std::string first, middle, head, old_object;
    {
        history_files::TableStore tables(warehouse.path / "tables");
        const auto& daily = history_table("futures.daily");
        const history_files::TableSeries series{"futures.daily", 1, "fixture.daily.v1"};
        const auto partition = [&](const std::string& day, const std::string& close) {
            TableSourceRow row(daily.columns.size());
            row[0] = "SHFE/rb/2026-10";
            row[4] = close;
            return table_partition(daily, "SHFE", day, {row});
        };
        const auto a = partition("2025-01-01", "100"), b = partition("2025-01-02", "200"),
                   c = partition("2025-01-01", "300");
        const auto initial =
            tables.record(a, series, tables.write(a), std::string(64, 'a'), 1000).revision;
        first = initial.snapshot;
        old_object = initial.object;
        middle =
            tables.record(b, series, tables.write(b), std::string(64, 'a'), 2000).revision.snapshot;
        head =
            tables.record(c, series, tables.write(c), std::string(64, 'a'), 3000).revision.snapshot;
    }
    start_data();
    protocol::DataClient client(endpoint + ".data", "historical-data");
    const auto preview = [&](const std::string& id) {
        data::proto::DataRequest request;
        request.mutable_table_retirement()->set_id(id);
        return client.call(request, 10s).table_retirement();
    };
    const auto execute = [&](const data::proto::TableRetirement& reviewed) {
        data::proto::DataRequest request;
        auto* operation = request.mutable_table_retirement();
        operation->set_id(reviewed.id());
        operation->set_execute(true);
        operation->set_preview_digest(reviewed.preview_digest());
        return client.call(request, 10s).table_retirement();
    };
    const auto old = preview(middle);
    ASSERT_EQ(old.state(), data::proto::TableRetirement::AVAILABLE);
    EXPECT_TRUE(old.objects().empty());
    ASSERT_EQ(execute(preview(first)).state(), data::proto::TableRetirement::REMOVED);
    const auto current = preview(middle);
    ASSERT_EQ(current.objects_size(), 1);
    EXPECT_EQ(current.objects(0), old_object);
    EXPECT_NE(current.preview_digest(), old.preview_digest());
    EXPECT_THROW(execute(old), Error);
    EXPECT_EQ(preview(middle).state(), data::proto::TableRetirement::AVAILABLE);
    const auto path =
        warehouse.path / "tables/objects" / old_object.substr(0, 2) / (old_object + ".parquet");
    EXPECT_TRUE(std::filesystem::exists(path));
    EXPECT_EQ(execute(current).state(), data::proto::TableRetirement::REMOVED);
    EXPECT_FALSE(std::filesystem::exists(path));
    EXPECT_EQ(execute(current).state(), data::proto::TableRetirement::REMOVED);
    EXPECT_EQ(preview(head).state(), data::proto::TableRetirement::BLOCKED);
}

TEST(ResearchResourceCatalogue, FixedInputAndResultIdentitySurviveRetirementAndReadOnlyPaging)
{
    TaskDirectory directory;
    tasks::Store store(asterion::research::task_domain(), directory.path, {"research-one"});
    tasks::submit(store, "alpha", input());
    const auto token = store.commit(store.claim("alpha")).token();
    store.commit(tasks::finish(store, "alpha", token, backtest::run(input())));
    tasks::submit(store, "beta", input());
    resource::proto::CatalogueQuery query;
    query.set_kind("backtest");
    query.set_limit(1);
    const auto first = asterion::research::task_resources(store, "fixture-node", query);
    ASSERT_EQ(first.items_size(), 1);
    EXPECT_EQ(first.items(0).object().id(), "alpha");
    EXPECT_EQ(first.next_after(), "alpha");
    EXPECT_EQ(
        first.items(0).logical_bytes(),
        std::filesystem::file_size(directory.path / "alpha/input.pb"));
    query.set_after(first.next_after());
    const auto second = asterion::research::task_resources(store, "fixture-node", query);
    ASSERT_EQ(second.items_size(), 1);
    EXPECT_EQ(second.items(0).object().id(), "beta");
    EXPECT_TRUE(second.next_after().empty());
    query.set_after("");
    query.set_kind("backtest_result");
    const auto completed = asterion::research::task_resources(store, "fixture-node", query);
    ASSERT_EQ(completed.items_size(), 1);
    EXPECT_EQ(completed.items(0).object().version(), store.describe("alpha").result_digest());
    EXPECT_FALSE(completed.items(0).has_logical_bytes());
    const auto hash = sha256_file(directory.path / "alpha/results/1.pb");
    store.commit(store.retire(store.retirement("alpha", "fixture-node").object()));
    const auto retired = asterion::research::task_resources(store, "fixture-node", query);
    ASSERT_EQ(retired.items_size(), 1);
    EXPECT_EQ(retired.items(0).state(), resource::proto::CatalogueEntry::RETIRED);
    EXPECT_EQ(
        retired.items(0).object().SerializeAsString(),
        completed.items(0).object().SerializeAsString());
    EXPECT_EQ(sha256_file(directory.path / "alpha/results/1.pb"), hash);
    EXPECT_NO_THROW(protocol::decode_resource_catalogue(retired, query, retired.owner()));
    query.set_kind("model");
    EXPECT_THROW(asterion::research::task_resources(store, "fixture-node", query), Error);
}

TEST_F(TaskProcess, ResourceCataloguesStayWithTheirOwnersWhenDataGoesOffline)
{
    service.reset();
    data_service.reset();
    const auto seeded = test::seed_history(warehouse.path, {100, 101}, "catalogue-rpc");
    {
        tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
        persist(store, "catalogued-input", input());
        store.commit(store.cancel("catalogued-input"));
    }
    start_data();
    start();
    research::proto::ResearchRequest research;
    research.mutable_resources()->set_kind("backtest");
    research.mutable_resources()->set_limit(1);
    const auto original = call(research).resources();
    ASSERT_EQ(original.items_size(), 1);
    EXPECT_EQ(original.items(0).object().id(), "catalogued-input");
    EXPECT_EQ(original.owner().node_id(), "fixture-node");
    EXPECT_EQ(original.owner().domain(), "research");
    protocol::DataClient client(endpoint + ".data", "historical-data");
    data::proto::DataRequest data;
    data.mutable_resources()->set_kind("minute_version");
    const auto archived = client.call(data, 5s).resources();
    ASSERT_EQ(archived.items_size(), 1);
    EXPECT_EQ(
        archived.items(0).object().id(),
        seeded.at("source_dataset_ids")[0].get<std::string>());
    EXPECT_EQ(archived.owner().node_id(), "fixture-node");
    EXPECT_EQ(archived.owner().domain(), "data");
    data_service.reset();
    std::filesystem::remove(endpoint + ".data");
    std::filesystem::remove(endpoint + ".data-workers");
    EXPECT_EQ(call(research).resources().SerializeAsString(), original.SerializeAsString());
    research.mutable_resources()->set_kind("table_snapshot");
    EXPECT_THROW(call(research), std::runtime_error);
}

TEST(PerformanceAssumptions, UnknownIsNotImplicitZeroAndFixedPoliciesSurviveTheExecutionDescriptor)
{
    const auto unknown = protocol::encode_performance_assumptions(
        {{"annual_periods", nullptr}, {"risk_free", "unconfigured"}});
    EXPECT_FALSE(unknown.has_annual_periods());
    EXPECT_EQ(unknown.risk_free(), backtest::proto::PerformanceAssumptions::RISK_FREE_UNSPECIFIED);
    EXPECT_NO_THROW(protocol::require_backtest_scoring_assumptions(1, unknown));
    EXPECT_THROW(protocol::require_backtest_scoring_assumptions(2, unknown), std::invalid_argument);
    auto policy = protocol::encode_performance_assumptions(
        {{"annual_periods", 252}, {"risk_free", "unconfigured"}});
    EXPECT_THROW(protocol::require_backtest_scoring_assumptions(2, policy), std::invalid_argument);
    policy.set_risk_free(backtest::proto::PerformanceAssumptions::EXPLICIT_ZERO);
    EXPECT_NO_THROW(protocol::require_backtest_scoring_assumptions(2, policy));
    auto source = input();
    const auto original = source.SerializeAsString();
    EXPECT_FALSE(protocol::decode_backtest(source).contains("performance_assumptions"));
    backtest::proto::BacktestRequest request;
    *request.mutable_strategies() = source.strategies();
    *request.mutable_performance_assumptions() = policy;
    protocol::set_backtest_strategies(source, request);
    EXPECT_NE(sha256_bytes(original), sha256_bytes(source.SerializeAsString()));
    task::proto::Task task;
    task.set_kind(task::proto::BACKTEST);
    task.set_program_artifact(std::string(64, 'a'));
    *task.mutable_input() = source;
    const auto execution = protocol::task_execution(task, sha256_bytes(source.SerializeAsString()));
    EXPECT_EQ(
        execution.backtest().performance_assumptions().SerializeAsString(),
        policy.SerializeAsString());
    EXPECT_EQ(
        protocol::decode_backtest(source).at("performance_assumptions"),
        protocol::decode_performance_assumptions(policy));
    auto changed = source;
    changed.mutable_performance_assumptions()->set_annual_periods(250);
    EXPECT_NE(sha256_bytes(changed.SerializeAsString()), sha256_bytes(source.SerializeAsString()));
    EXPECT_THROW(
        protocol::encode_performance_assumptions(
            {{"annual_periods", true}, {"risk_free", "explicit_zero"}}),
        std::invalid_argument);
    EXPECT_THROW(
        protocol::encode_performance_assumptions(
            {{"annual_periods", 0}, {"risk_free", "explicit_zero"}}),
        std::invalid_argument);
    EXPECT_THROW(
        protocol::encode_performance_assumptions(
            {{"annual_periods", 252}, {"risk_free", "fixed_nonzero"}}),
        std::invalid_argument);
    request.clear_performance_assumptions();
    protocol::set_backtest_strategies(changed, request);
    EXPECT_FALSE(changed.has_performance_assumptions());
    EXPECT_EQ(changed.SerializeAsString(), original);
}

TEST(Backtest, MissingComparisonAssumptionsRemainInspectableButCannotChooseANewCandidate)
{
    auto source = comparison();
    source.clear_performance_assumptions();
    const auto before = source.SerializeAsString();
    EXPECT_NO_THROW(protocol::decode_backtest(source, protocol::DatasetView::metadata));
    try {
        (void)backtest::run(source);
        FAIL() << "A new comparison must not infer annual periods or zero risk-free input";
    } catch (const std::invalid_argument& error) {
        EXPECT_STREQ(
            error.what(),
            "Sharpe-based comparison requires explicit annual periods and a zero risk-free assumption");
    }
    EXPECT_EQ(source.SerializeAsString(), before);
    source.mutable_performance_assumptions()->set_annual_periods(252);
    EXPECT_THROW(backtest::run(source), std::invalid_argument);
}

namespace {

backtest::proto::BacktestInput fixed_weight_input()
{
    const std::vector<std::string>
        days{"2026-09-21", "2026-09-22", "2026-09-23", "2026-09-24", "2026-09-25", "2026-09-28"};
    std::vector<MarketBar> bars;
    for (std::size_t i = 0; i < days.size(); ++i)
        bars.push_back(
            test::flat(
                days[i],
                1'790'000'000'000'000'000LL + static_cast<std::int64_t>(i) * 86'400'000'000'000LL,
                "1000",
                "100"));
    auto spec = testing_support::backtest_input(test::dataset(bars, {}, test::contract(), 0));
    spec.mutable_paper()->mutable_deposit()->set_units(d("100000").raw());
    auto* second = spec.mutable_paper()->add_contracts();
    *second = spec.paper().contracts(0);
    *second->mutable_dataset()->mutable_contract() =
        test::contract("SHFE", "hc2610", "hc", "2026-10");
    second->mutable_dataset()->set_revision(protocol::bar_dataset_revision(second->dataset()));
    spec.set_dataset_revision(protocol::dataset_revision(spec.paper()));
    const Json members = Json::array(
        {Json{{"venue", "SHFE"}, {"symbol", "rb2610"}, {"weight", "0.4"}},
            Json{{"venue", "SHFE"}, {"symbol", "hc2610"}, {"weight", "-0.2"}}});
    const Json rule{
        {"kind", "fixed_weights"},
        {"rebalance", 2},
        {"gross_weight_limit", "0.8"},
        {"net_weight_limit", "0.3"},
        {"members", members}
    };
    *spec.mutable_strategies(0) =
        protocol::encode_strategy(Json{{"sides", "both"}, {"rule", rule}});
    return spec;
}

} // namespace

TEST(Backtest, FixedWeightDailyPartialFillsExpireWithoutChangingTargetsOrInventingCompletion)
{
    auto spec = fixed_weight_input();
    spec.mutable_strategies(0)->mutable_fixed_weights()->set_rebalance(3);
    for (auto& contract : *spec.mutable_paper()->mutable_contracts()) {
        auto* data = contract.mutable_dataset();
        for (auto& bar : *data->mutable_bars())
            bar.mutable_volume()->set_units(d("10").raw());
        data->set_revision(protocol::bar_dataset_revision(*data));
    }
    spec.set_dataset_revision(protocol::dataset_revision(spec.paper()));
    const auto result = backtest::run(spec);
    EXPECT_NO_THROW(protocol::validate_backtest_result(spec, result));
    ASSERT_EQ(result.portfolio_targets_size(), 2);
    ASSERT_EQ(result.portfolio_fills_size(), 2);
    ASSERT_EQ(result.account().fills_size(), 2);
    EXPECT_EQ(result.account().fees().units(), d("4").raw());
    for (int i = 0; i < result.portfolio_fills_size(); ++i) {
        const auto& actual = result.portfolio_fills(i);
        EXPECT_EQ(actual.decision_source_index(), 4U + (i % 2));
        EXPECT_EQ(actual.fill_id(), result.account().fills(i).id());
        EXPECT_GT(actual.source_index(), actual.decision_source_index());
        EXPECT_EQ(result.account().fills(i).quantity().units(), d("1").raw());
    }
    EXPECT_EQ(result.portfolio_fills(0).source_index(), 6U);
    EXPECT_EQ(result.portfolio_fills(1).source_index(), 7U);
    EXPECT_EQ(result.fill_days(0).trading_day(), "2026-09-24");
    EXPECT_EQ(result.fill_days(1).trading_day(), "2026-09-24");
    EXPECT_EQ(result.portfolio_targets(0).members(0).quantity().units(), d("4").raw());
    EXPECT_EQ(result.portfolio_targets(0).members(1).quantity().units(), d("-2").raw());
    EXPECT_FALSE(result.portfolio_targets(1).has_next_bar());
    EXPECT_EQ(result.portfolio_orders_size(), 2);
    EXPECT_EQ(result.account().orders_size(), 2);
    EXPECT_EQ(result.account().orders(0).quantity().units(), d("4").raw());
    EXPECT_EQ(result.account().orders(0).filled().units(), d("1").raw());
    EXPECT_EQ(result.account().orders(0).state(), protocol::proto::CANCELLED);
    EXPECT_EQ(result.account().orders(1).quantity().units(), d("2").raw());
    EXPECT_EQ(result.account().orders(1).filled().units(), d("1").raw());
    EXPECT_EQ(result.account().orders(1).state(), protocol::proto::CANCELLED);
    EXPECT_EQ(result.account().balance().units(), d("99996").raw());
    ASSERT_EQ(result.account().positions_size(), 2);
    EXPECT_EQ(result.account().positions(0).quantity().units(), d("1").raw());
    EXPECT_EQ(result.account().positions(1).quantity().units(), d("1").raw());
    EXPECT_EQ(result.account().positions(0).side(), protocol::proto::BUY);
    EXPECT_EQ(result.account().positions(1).side(), protocol::proto::SELL);
    EXPECT_EQ(result.SerializeAsString(), backtest::run(spec).SerializeAsString());
}

TEST(Backtest, FixedWeightNewRebalanceUsesActualPartialHoldingsAfterDailyExpiration)
{
    auto spec = fixed_weight_input();
    for (auto& contract : *spec.mutable_paper()->mutable_contracts()) {
        auto* data = contract.mutable_dataset();
        for (auto& bar : *data->mutable_bars())
            bar.mutable_volume()->set_units(d("10").raw());
        data->set_revision(protocol::bar_dataset_revision(*data));
    }
    spec.set_dataset_revision(protocol::dataset_revision(spec.paper()));
    const auto result = backtest::run(spec);
    EXPECT_NO_THROW(protocol::validate_backtest_result(spec, result));
    ASSERT_EQ(result.portfolio_targets_size(), 3);
    ASSERT_EQ(result.account().orders_size(), 3);
    ASSERT_EQ(result.portfolio_orders_size(), 3);
    ASSERT_EQ(result.portfolio_fills_size(), 3);
    const auto& first = result.portfolio_targets(0);
    EXPECT_EQ(first.members(0).quantity().units(), d("4").raw());
    EXPECT_EQ(first.members(1).quantity().units(), d("-2").raw());
    const auto& second = result.portfolio_targets(1);
    EXPECT_EQ(second.equity().units(), d("99996").raw());
    EXPECT_EQ(second.members(0).quantity().units(), d("3").raw());
    EXPECT_EQ(second.members(1).quantity().units(), d("-1").raw());
    // The expired three-lot remainder is not reused or added to the new target.
    // A already holds one, so the new accepted adjustment is two, not three/four.
    const auto& adjustment = result.account().orders(2);
    EXPECT_EQ(adjustment.symbol(), "rb2610");
    EXPECT_EQ(adjustment.quantity().units(), d("2").raw());
    EXPECT_EQ(adjustment.filled().units(), d("1").raw());
    EXPECT_EQ(adjustment.state(), protocol::proto::CANCELLED);
    EXPECT_EQ(result.portfolio_orders(2).order_id(), adjustment.id());
    EXPECT_EQ(result.portfolio_orders(2).intent_id(), second.members(0).intent_id());
    EXPECT_EQ(result.portfolio_orders(2).source_index(), 8U);
    EXPECT_EQ(result.portfolio_fills(2).intent_id(), second.members(0).intent_id());
    EXPECT_EQ(result.portfolio_fills(2).source_index(), 8U);
    EXPECT_EQ(result.account().fees().units(), d("6").raw());
    EXPECT_EQ(result.account().balance().units(), d("99994").raw());
    ASSERT_EQ(result.account().positions_size(), 3);
    Decimal held_a, held_b;
    for (const auto& lot : result.account().positions()) {
        EXPECT_EQ(lot.venue(), "SHFE");
        EXPECT_EQ(lot.quantity().units(), d("1").raw());
        EXPECT_FALSE(lot.today());
        if (lot.symbol() == "rb2610") {
            EXPECT_EQ(lot.side(), protocol::proto::BUY);
            held_a = held_a + d("1");
        } else {
            EXPECT_EQ(lot.symbol(), "hc2610");
            EXPECT_EQ(lot.side(), protocol::proto::SELL);
            held_b = held_b + d("1");
        }
    }
    EXPECT_EQ(held_a, d("2"));
    EXPECT_EQ(held_b, d("1"));
    for (const auto& member : first.members()) {
        ASSERT_TRUE(member.has_observation());
        EXPECT_EQ(member.observation().positions_size(), 0);
        EXPECT_EQ(member.observation().working_orders_size(), 0);
    }
    ASSERT_EQ(second.members(0).observation().positions_size(), 1);
    EXPECT_EQ(second.members(0).observation().positions(0).quantity().units(), d("1").raw());
    EXPECT_FALSE(second.members(0).observation().positions(0).today());
    EXPECT_EQ(second.members(0).observation().working_orders_size(), 0);
    EXPECT_EQ(result.portfolio_targets(2).members(0).observation().positions_size(), 2);
    EXPECT_FALSE(result.portfolio_targets(2).has_next_bar());
    auto forged = result;
    forged.mutable_portfolio_targets(1)->mutable_members(0)->clear_observation();
    EXPECT_THROW(protocol::validate_backtest_result(spec, forged), std::invalid_argument);
    forged = result;
    forged.mutable_portfolio_targets(1)
        ->mutable_members(0)
        ->mutable_observation()
        ->mutable_positions(0)
        ->mutable_quantity()
        ->set_units(d("2").raw());
    EXPECT_THROW(protocol::validate_backtest_result(spec, forged), std::invalid_argument);
    forged = result;
    *forged.mutable_portfolio_targets(1)
         ->mutable_members(0)
         ->mutable_observation()
         ->add_working_orders() = result.account().orders(0);
    EXPECT_THROW(protocol::validate_backtest_result(spec, forged), std::invalid_argument);
    forged = result;
    forged.mutable_portfolio_orders(2)->set_intent_id(first.members(0).intent_id());
    EXPECT_THROW(protocol::validate_backtest_result(spec, forged), std::invalid_argument);
    forged = result;
    forged.mutable_portfolio_fills(2)->set_intent_id(first.members(0).intent_id());
    EXPECT_THROW(protocol::validate_backtest_result(spec, forged), std::invalid_argument);
}

TEST(Backtest, FixedWeightTargetsUseOriginalCommonBoundaryAndOnlyLaterBarsPayActualCosts)
{
    const auto spec = fixed_weight_input();
    const auto result = backtest::run(spec);
    ASSERT_EQ(result.portfolio_targets_size(), 3);
    EXPECT_NO_THROW(protocol::validate_backtest_result(spec, result));
    const auto& first = result.portfolio_targets(0);
    EXPECT_EQ(first.trading_day(), "2026-09-22");
    EXPECT_EQ(first.equity().units(), d("100000").raw());
    ASSERT_EQ(first.members_size(), 2);
    EXPECT_EQ(first.members(0).quantity().units(), d("4").raw());
    EXPECT_EQ(first.members(1).quantity().units(), d("-2").raw());
    EXPECT_EQ(first.members(0).source_index(), 2U);
    EXPECT_EQ(first.members(1).source_index(), 3U);
    EXPECT_EQ(first.members(0).intent_id(), "strategy.2");
    EXPECT_TRUE(first.has_next_bar());
    EXPECT_EQ(first.unallocated_nominal_budget().units(), d("20000").raw());
    ASSERT_EQ(result.account().fills_size(), 4);
    ASSERT_EQ(result.fill_days_size(), 4);
    ASSERT_EQ(result.portfolio_fills_size(), 4);
    for (int i = 0; i < 4; ++i) {
        const auto& actual = result.portfolio_fills(i);
        const auto& target = result.portfolio_targets(i / 2).members(i % 2);
        EXPECT_EQ(actual.fill_id(), result.account().fills(i).id());
        EXPECT_EQ(actual.intent_id(), target.intent_id());
        EXPECT_EQ(actual.decision_source_index(), target.source_index());
        EXPECT_EQ(actual.source_index(), target.source_index() + 2);
        EXPECT_EQ(
            actual.boundary_ns(),
            spec.paper()
                .contracts(i % 2)
                .dataset()
                .bars(static_cast<int>(actual.source_index() / 2))
                .timestamp_ns());
    }
    auto wrong_fill = result;
    wrong_fill.mutable_portfolio_fills(0)->set_source_index(
        result.portfolio_fills(0).decision_source_index());
    EXPECT_THROW(protocol::validate_backtest_result(spec, wrong_fill), std::invalid_argument);
    wrong_fill = result;
    wrong_fill.mutable_portfolio_fills()->RemoveLast();
    EXPECT_THROW(protocol::validate_backtest_result(spec, wrong_fill), std::invalid_argument);
    EXPECT_EQ(result.fill_days(0).trading_day(), "2026-09-23");
    EXPECT_EQ(result.fill_days(1).trading_day(), "2026-09-23");
    EXPECT_EQ(result.fill_days(2).trading_day(), "2026-09-25");
    EXPECT_EQ(result.fill_days(3).trading_day(), "2026-09-25");
    EXPECT_EQ(result.account().fees().units(), d("20").raw());
    EXPECT_EQ(result.account().balance().units(), d("99980").raw());
    const auto& second = result.portfolio_targets(1);
    EXPECT_EQ(second.equity().units(), d("99988").raw());
    EXPECT_EQ(second.members(0).quantity().units(), d("3").raw());
    EXPECT_EQ(second.members(1).quantity().units(), d("-1").raw());
    EXPECT_FALSE(result.portfolio_targets(2).has_next_bar());
    EXPECT_EQ(result.SerializeAsString(), backtest::run(spec).SerializeAsString());
    EXPECT_EQ(protocol::decode_backtest_result(result).at("portfolio_targets").size(), 3U);
    auto future = spec;
    for (int c = 0; c < future.paper().contracts_size(); ++c) {
        auto* data = future.mutable_paper()->mutable_contracts(c)->mutable_dataset();
        auto* bar = data->mutable_bars(data->bars_size() - 1);
        for (auto* price :
            {bar->mutable_open(), bar->mutable_high(), bar->mutable_low(), bar->mutable_close()})
            price->set_units(d(c ? "900" : "1100").raw());
        data->mutable_days(data->days_size() - 1)
            ->mutable_settlement_price()
            ->set_units(bar->close().units());
        data->set_revision(protocol::bar_dataset_revision(*data));
    }
    future.set_dataset_revision(protocol::dataset_revision(future.paper()));
    const auto changed = backtest::run(future);
    EXPECT_EQ(changed.portfolio_targets(0).SerializeAsString(), first.SerializeAsString());
    EXPECT_EQ(changed.portfolio_targets(1).SerializeAsString(), second.SerializeAsString());
    auto wrong = result;
    wrong.mutable_portfolio_targets()->RemoveLast();
    EXPECT_THROW(protocol::validate_backtest_result(spec, wrong), std::invalid_argument);
    wrong = result;
    wrong.mutable_portfolio_targets(0)->mutable_equity()->set_units(d("99980").raw());
    EXPECT_THROW(protocol::validate_backtest_result(spec, wrong), std::invalid_argument);
    wrong = result;
    wrong.mutable_portfolio_targets(0)->mutable_members(1)->mutable_quantity()->set_units(
        d("-3").raw());
    EXPECT_THROW(protocol::validate_backtest_result(spec, wrong), std::invalid_argument);
}

TEST(Backtest, FixedWeightFundingRefusalKeepsDeclaredTargetAndDoesNotScaleItToAvailableCash)
{
    auto spec = fixed_weight_input();
    for (auto& c : *spec.mutable_paper()->mutable_contracts())
        c.mutable_cost_schedule()
            ->mutable_versions(0)
            ->mutable_values()
            ->mutable_margin_per_lot()
            ->set_units(d("25000").raw());
    const auto result = backtest::run(spec);
    ASSERT_EQ(result.portfolio_targets_size(), 3);
    EXPECT_EQ(result.portfolio_targets(0).members(0).quantity().units(), d("4").raw());
    ASSERT_FALSE(result.target_rejections().empty());
    EXPECT_EQ(result.target_rejections(0).target().units(), d("4").raw());
    EXPECT_EQ(
        result.target_rejections(0).reason(),
        backtest::proto::BacktestTargetRejection::INSUFFICIENT_FUNDS);
    EXPECT_EQ(result.target_rejections(0).source_index(), 4U);
    for (const auto& fill : result.account().fills())
        EXPECT_EQ(fill.symbol(), "hc2610");
    EXPECT_EQ(result.account().fees().units(), d("8").raw());
    EXPECT_EQ(result.account().equity().units(), d("99992").raw());
    EXPECT_NO_THROW(protocol::validate_backtest_result(spec, result));
}

TEST(Backtest, FixedWeightSourceRejectsWrongMembersPartialTimelinesAndImplicitComparisons)
{
    const auto original = fixed_weight_input();
    auto bad = original;
    bad.mutable_strategies(0)->mutable_fixed_weights()->mutable_members(1)->set_symbol("other");
    EXPECT_THROW(protocol::decode_backtest(bad), std::invalid_argument);
    bad = original;
    bad.mutable_paper()->mutable_contracts(1)->mutable_dataset()->mutable_bars(0)->set_timestamp_ns(
        original.paper().contracts(1).dataset().bars(0).timestamp_ns() + 1);
    bad.mutable_paper()->mutable_contracts(1)->mutable_dataset()->set_revision(
        protocol::bar_dataset_revision(bad.paper().contracts(1).dataset()));
    bad.set_dataset_revision(protocol::dataset_revision(bad.paper()));
    EXPECT_THROW(protocol::decode_backtest(bad), std::invalid_argument);
    bad = original;
    *bad.add_strategies() = testing_support::moving_average(1, 2);
    EXPECT_THROW(protocol::decode_backtest(bad), std::invalid_argument);
}

TEST_F(
    TaskProcess,
    FixedWeightPortfolioUsesRealDataAdmissionAndKeepsOriginalTargetsAcrossRestartAndPreparationRecovery)
{
    (void)test::seed_history(warehouse.path, {100, 101}, "fixed-weight-catalogue");
    const std::vector<std::string>
        days{"2026-09-21", "2026-09-22", "2026-09-23", "2026-09-24", "2026-09-25", "2026-09-28"};
    std::vector<test::DailyRow> rows;
    for (const auto& day : days)
        rows.push_back({day, 1000, 100, test::DailyTrade{1000, 1001, 999, 1000, 1000}});
    const std::array<Json, 2> tables{
        test::seed_daily_table(warehouse.path, "SHFE/rb/2026-10", rows),
        test::seed_daily_table(warehouse.path, "SHFE/hc/2026-10", rows)
    };
    start_data();
    const auto prototype = fixed_weight_input();
    research::proto::ResearchRequest submit;
    submit.mutable_submit()->set_id("fixed-weight-portfolio");
    auto& request = *submit.mutable_submit()->mutable_backtest();
    *request.mutable_deposit() = prototype.paper().deposit();
    *request.mutable_risk() = prototype.paper().risk();
    *request.add_strategies() = prototype.strategies(0);
    for (int c = 0; c < 2; ++c) {
        auto* contract = request.add_contracts();
        *contract->mutable_data()->mutable_daily() = protocol::encode_daily_table(tables[c]);
        contract->mutable_data()->set_contract(c ? "SHFE/hc/2026-10" : "SHFE/rb/2026-10");
        contract->mutable_data()->set_begin_day(days.front());
        contract->mutable_data()->set_end_day(days.back());
        *contract->mutable_cost_schedule() = prototype.paper().contracts(c).cost_schedule();
    }
    const auto admitted = call(submit, {}, 10s).task();
    const auto completed = [&](const char* id) {
        const auto status = testing_support::wait_task(
            [&](auto query) {
                return call(query);
            },
            id,
            20s);
        if (status.state() != task_wire::SUCCEEDED)
            throw std::runtime_error(status.error());
        research::proto::ResearchRequest read;
        read.mutable_result()->set_id(id);
        return call(read, {}, 10s);
    };
    const auto result = completed("fixed-weight-portfolio");
    ASSERT_EQ(result.backtest().portfolio_targets_size(), 3);
    ASSERT_EQ(result.backtest().portfolio_fills_size(), result.backtest().account().fills_size());
    EXPECT_EQ(result.backtest().portfolio_targets(0).members(0).quantity().units(), d("4").raw());
    EXPECT_EQ(result.backtest().portfolio_targets(0).members(1).quantity().units(), d("-2").raw());
    EXPECT_EQ(result.backtest().account().fees().units(), d("20").raw());
    EXPECT_EQ(result.result_task().program_artifact(), fixture_program_artifact);
    EXPECT_FALSE(result.result_task().risk_artifact().empty());
    EXPECT_NO_THROW(
        protocol::validate_backtest_result(result.result_task().input(), result.backtest()));
    const auto replay = call(submit, {}, 10s).task();
    EXPECT_EQ(replay.id(), admitted.id());
    EXPECT_EQ(replay.attempt(), 1U);
    EXPECT_EQ(replay.result_digest(), result.result_task().result_digest());
    auto changed = submit;
    changed.mutable_submit()
        ->mutable_backtest()
        ->mutable_strategies(0)
        ->mutable_fixed_weights()
        ->mutable_gross_weight_limit()
        ->set_units(d("0.9").raw());
    EXPECT_THROW(call(changed, {}, 10s), std::runtime_error);
    service.reset();
    data_service.reset();
    start();
    const auto restored = completed("fixed-weight-portfolio");
    EXPECT_EQ(restored.backtest().SerializeAsString(), result.backtest().SerializeAsString());
    EXPECT_EQ(restored.result_task().SerializeAsString(), result.result_task().SerializeAsString());
    service.reset();
    {
        tasks::Store store(asterion::research::task_domain(), directory.path, {"task"});
        auto preparation =
            task_submission(store, "fixed-weight-prepared", result.result_task().input());
        asterion::tasks::set_program_artifact(preparation, fixture_program_artifact);
        store.admit_submission(preparation);
        preparation.prepare_files();
        EXPECT_TRUE(store.prepared_submission_record("fixed-weight-prepared")
                .execution()
                .backtest()
                .strategies(0)
                .has_fixed_weights());
        EXPECT_TRUE(store.abandon_submission(preparation));
    }
    start_data();
    start();
    research::proto::ResearchRequest recover;
    recover.mutable_recover_submission()->set_id("fixed-weight-prepared");
    EXPECT_EQ(call(recover, {}, 10s).task().kind(), task_wire::BACKTEST);
    const auto recovered = completed("fixed-weight-prepared");
    EXPECT_EQ(
        recovered.result_task().input().SerializeAsString(),
        result.result_task().input().SerializeAsString());
    EXPECT_EQ(recovered.backtest().SerializeAsString(), result.backtest().SerializeAsString());
    research::proto::ResearchRequest list;
    list.mutable_list()->set_limit(10);
    EXPECT_EQ(call(list).tasks().tasks_size(), 2);
    research::proto::ResearchRequest freeze;
    auto& mutation = *freeze.mutable_strategy()->mutable_mutation();
    mutation.set_id("freeze-portfolio-candidate");
    auto& fixed = *mutation.mutable_freeze();
    fixed.set_id("portfolio-candidate");
    fixed.set_name("Synthetic fixed portfolio");
    fixed.set_purpose("Explicit original-condition portfolio research");
    fixed.set_limitations(
        "Retrospective synthetic reference, no independent validation or live permission");
    auto& source = *fixed.mutable_source();
    source.mutable_owner()->set_node_id("fixture-node");
    source.mutable_owner()->set_service_id("task");
    source.mutable_owner()->set_domain("research");
    source.set_kind("backtest_result");
    source.set_id("fixed-weight-portfolio");
    source.set_version(result.result_task().result_digest());
    const auto candidate = call(freeze).strategy().package();
    ASSERT_TRUE(candidate.definition().has_portfolio());
    ASSERT_EQ(candidate.definition().portfolio().contracts_size(), 2);
    EXPECT_FALSE(candidate.definition().has_contract());
    EXPECT_EQ(
        candidate.definition().portfolio().strategy().SerializeAsString(),
        prototype.strategies(0).SerializeAsString());
    EXPECT_EQ(call(freeze).strategy().package().SerializeAsString(), candidate.SerializeAsString());
    EXPECT_EQ(call(list).tasks().tasks_size(), 2);
    research::proto::ResearchRequest deliver;
    auto& creation = *deliver.mutable_strategy()->mutable_mutation();
    creation.set_id("deliver-portfolio");
    creation.mutable_deliver()->set_id("portfolio-delivery");
    creation.mutable_deliver()->set_target_task_id("portfolio-explicit-use");
    *creation.mutable_deliver()->mutable_package() = protocol::strategy_package_object(candidate);
    const auto draft = call(deliver).strategy().delivery();
    research::proto::ResearchRequest delivery_status;
    delivery_status.mutable_strategy()->mutable_delivery()->set_id(draft.id());
    EXPECT_FALSE(call(delivery_status).strategy().target_accepted());
    EXPECT_EQ(call(list).tasks().tasks_size(), 2);
    research::proto::ResearchRequest accept;
    auto& use = *accept.mutable_submit();
    use.set_id(draft.target_task_id());
    *use.mutable_strategy_use()->mutable_package() = draft.package();
    *use.mutable_strategy_use()->mutable_delivery() = protocol::strategy_delivery_object(draft);
    auto& reuse = *use.mutable_backtest_reuse();
    reuse.set_choice(backtest::proto::BacktestReuse::FIXED);
    reuse.set_source_id(draft.reference_result().id());
    reuse.set_source_result_digest(draft.reference_result().version());
    reuse.set_source_input_sha256(draft.source_input_sha256());
    auto wrong = accept;
    wrong.mutable_submit()->mutable_backtest_reuse()->mutable_fee_multiplier()->set_units(
        d("2").raw());
    EXPECT_THROW(call(wrong), std::runtime_error);
    EXPECT_NO_THROW(call(accept));
    const auto used = completed(draft.target_task_id().c_str());
    EXPECT_EQ(
        used.result_task().strategy_use().SerializeAsString(),
        use.strategy_use().SerializeAsString());
    EXPECT_EQ(
        used.result_task().input().SerializeAsString(),
        result.result_task().input().SerializeAsString());
    EXPECT_EQ(used.backtest().SerializeAsString(), result.backtest().SerializeAsString());
    EXPECT_EQ(call(accept).task().attempt(), 1U);
    EXPECT_TRUE(call(delivery_status).strategy().target_accepted());
    EXPECT_EQ(call(list).tasks().tasks_size(), 3);
    auto explicit_delivery = deliver;
    explicit_delivery.mutable_strategy()->mutable_mutation()->set_id(
        "deliver-portfolio-new-conditions");
    auto& new_delivery =
        *explicit_delivery.mutable_strategy()->mutable_mutation()->mutable_deliver();
    new_delivery.set_id("portfolio-new-conditions-delivery");
    new_delivery.set_target_task_id("portfolio-new-conditions");
    const auto explicit_draft = call(explicit_delivery).strategy().delivery();
    research::proto::ResearchRequest explicit_status;
    explicit_status.mutable_strategy()->mutable_delivery()->set_id(explicit_draft.id());
    EXPECT_FALSE(call(explicit_status).strategy().target_accepted());
    // The isolated Data writer is stopped before seeding a new immutable snapshot.
    data_service.reset();
    std::vector<test::DailyRow> revised_rows;
    for (const auto& day : days)
        revised_rows.push_back({day, 1000, 101, test::DailyTrade{1000, 1001, 999, 1000, 1000}});
    const std::array<Json, 2> revised_tables{
        test::seed_daily_table(warehouse.path, "SHFE/rb/2026-10", revised_rows),
        test::seed_daily_table(warehouse.path, "SHFE/hc/2026-10", revised_rows)
    };
    start_data();
    auto explicit_submit = submit;
    auto& configured = *explicit_submit.mutable_submit();
    configured.set_id(explicit_draft.target_task_id());
    *configured.mutable_strategy_use()->mutable_package() = explicit_draft.package();
    *configured.mutable_strategy_use()->mutable_delivery() =
        protocol::strategy_delivery_object(explicit_draft);
    auto& conditions = *configured.mutable_backtest();
    for (int c = 0; c < conditions.contracts_size(); ++c) {
        auto* member = conditions.mutable_contracts(c);
        *member->mutable_data()->mutable_daily() = protocol::encode_daily_table(revised_tables[c]);
        for (auto& version : *member->mutable_cost_schedule()->mutable_versions())
            for (auto* fee :
                {version.mutable_values()->mutable_open_fee(),
                    version.mutable_values()->mutable_close_today_fee(),
                    version.mutable_values()->mutable_close_yesterday_fee()})
                fee->set_units(fee->units() * 2);
    }
    auto wrong_policy = explicit_submit;
    wrong_policy.mutable_submit()
        ->mutable_backtest()
        ->mutable_strategies(0)
        ->mutable_fixed_weights()
        ->mutable_gross_weight_limit()
        ->set_units(d("0.9").raw());
    EXPECT_THROW(call(wrong_policy), std::runtime_error);
    EXPECT_EQ(call(list).tasks().tasks_size(), 3);
    EXPECT_NO_THROW(call(explicit_submit));
    const auto explicit_used = completed(explicit_draft.target_task_id().c_str());
    EXPECT_EQ(explicit_used.backtest().account().fees().units(), d("40").raw());
    EXPECT_EQ(
        explicit_used.result_task().strategy_use().request_sha256(),
        sha256_bytes(conditions.SerializeAsString()));
    EXPECT_NE(
        explicit_used.result_task().input().SerializeAsString(),
        result.result_task().input().SerializeAsString());
    for (int c = 0; c < 2; ++c) {
        const auto& chosen =
            explicit_used.result_task().input().paper().contracts(c).dataset().daily();
        EXPECT_EQ(chosen.snapshot(), revised_tables[c].at("snapshot").get<std::string>());
        EXPECT_NE(
            chosen.snapshot(),
            result.result_task().input().paper().contracts(c).dataset().daily().snapshot());
    }
    EXPECT_TRUE(call(explicit_status).strategy().target_accepted());
    EXPECT_EQ(call(explicit_submit).task().attempt(), 1U);
    auto changed_conditions = explicit_submit;
    changed_conditions.mutable_submit()->mutable_backtest()->mutable_deposit()->set_units(
        d("200000").raw());
    EXPECT_THROW(call(changed_conditions), std::runtime_error);
    EXPECT_EQ(
        completed("fixed-weight-portfolio").backtest().account().fees().units(),
        d("20").raw());
    EXPECT_EQ(call(freeze).strategy().package().SerializeAsString(), candidate.SerializeAsString());
    EXPECT_EQ(call(list).tasks().tasks_size(), 4);
    service.reset();
    data_service.reset();
    start();
    const auto offline_use = completed(draft.target_task_id().c_str());
    EXPECT_EQ(
        offline_use.result_task().SerializeAsString(),
        used.result_task().SerializeAsString());
    EXPECT_EQ(offline_use.backtest().SerializeAsString(), used.backtest().SerializeAsString());
    EXPECT_EQ(offline_use.service_id(), used.service_id());
    EXPECT_EQ(call(freeze).strategy().package().SerializeAsString(), candidate.SerializeAsString());
    EXPECT_TRUE(call(delivery_status).strategy().target_accepted());
    EXPECT_EQ(call(accept).task().attempt(), 1U);
    const auto offline_explicit = completed(explicit_draft.target_task_id().c_str());
    EXPECT_EQ(
        offline_explicit.result_task().SerializeAsString(),
        explicit_used.result_task().SerializeAsString());
    EXPECT_EQ(
        offline_explicit.backtest().SerializeAsString(),
        explicit_used.backtest().SerializeAsString());
    EXPECT_TRUE(call(explicit_status).strategy().target_accepted());
    EXPECT_EQ(call(explicit_submit).task().attempt(), 1U);
    auto pending_command = deliver;
    pending_command.mutable_strategy()->mutable_mutation()->set_id(
        "deliver-portfolio-pending-recovery");
    auto& pending_request =
        *pending_command.mutable_strategy()->mutable_mutation()->mutable_deliver();
    pending_request.set_id("portfolio-pending-delivery");
    pending_request.set_target_task_id("portfolio-pending-unadmitted");
    const auto pending_draft = call(pending_command).strategy().delivery();
    research::proto::ResearchRequest pending_status;
    pending_status.mutable_strategy()->mutable_delivery()->set_id(pending_draft.id());
    EXPECT_FALSE(call(pending_status).strategy().target_accepted());
    EXPECT_EQ(call(list).tasks().tasks_size(), 4);
    service.reset();
    TaskDirectory archive;
    const auto backup = archive.path / "portfolio-candidate-backup";
    const auto restore = archive.path / "portfolio-candidate-restored";
    {
        auto exported = research::cold_backup(directory.path, candidate.owner());
        std::vector<backup_files::Snapshot> snapshots;
        snapshots.push_back(std::move(exported.snapshot));
        backup_files::Cut cut(std::move(snapshots));
        const auto plan = cut.preview("portfolio-candidate-independent");
        cut.save("portfolio-candidate-independent", backup, sha256_bytes(plan.dump()));
    }
    backup_files::restore_files(backup, restore);
    std::filesystem::rename(directory.path, archive.path / "preserved-original-research");
    std::filesystem::rename(warehouse.path, archive.path / "preserved-original-data");
    const auto restored_root = restore / "owners/0";
    {
        const auto inspected = research::cold_backup(restored_root, candidate.owner());
        EXPECT_TRUE(
            std::ranges::any_of(inspected.snapshot.dependencies, [](const auto& dependency) {
                return dependency.object.owner().domain() == "data" && !dependency.included;
            })); // Research-only restore must not invent external Data closure.
    }

    // Stop this owned process before the isolated archive directory is removed,
    // including on an assertion failure or exception.
    struct StopRestored
    {
        std::unique_ptr<ChildProcess>& process;

        ~StopRestored()
        {
            process.reset();
        }
    } stop_restored{service};

    launch(true, restored_root);
    wait_for_initialization();
    research::proto::ResearchRequest read_candidate;
    read_candidate.mutable_strategy()->mutable_get()->set_id(candidate.id());
    EXPECT_EQ(
        call(read_candidate).strategy().package().SerializeAsString(),
        candidate.SerializeAsString());
    EXPECT_EQ(call(freeze).strategy().package().SerializeAsString(), candidate.SerializeAsString());
    EXPECT_EQ(
        call(pending_command).strategy().delivery().SerializeAsString(),
        pending_draft.SerializeAsString());
    EXPECT_FALSE(call(pending_status).strategy().target_accepted());
    EXPECT_TRUE(call(delivery_status).strategy().target_accepted());
    EXPECT_TRUE(call(explicit_status).strategy().target_accepted());
    const auto from_restored = completed(explicit_draft.target_task_id().c_str());
    EXPECT_EQ(
        from_restored.result_task().SerializeAsString(),
        explicit_used.result_task().SerializeAsString());
    EXPECT_EQ(
        from_restored.backtest().SerializeAsString(),
        explicit_used.backtest().SerializeAsString());
    EXPECT_EQ(call(explicit_submit).task().attempt(), 1U);
    EXPECT_EQ(call(list).tasks().tasks_size(), 4);
    research::proto::ResearchRequest missing;
    missing.mutable_get()->set_id(pending_draft.target_task_id());
    EXPECT_THROW(call(missing), std::runtime_error);
}
