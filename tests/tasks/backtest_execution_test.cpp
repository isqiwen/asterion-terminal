#include "task_execution_support.hpp"

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
