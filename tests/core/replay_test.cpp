#include <gtest/gtest.h>
#include <asterion/futures/order_limits.hpp>
#include <asterion/futures/replay.hpp>
#include <asterion/futures/trading_days.hpp>

using namespace asterion;

namespace {

Decimal d(const char* text)
{
    return Decimal::parse(text);
}

const std::vector<std::string> days{"2026-01-05", "2026-01-06", "2026-01-07", "2026-01-08"};

// A contract whose day i closes at closes[i], trading 10 either side of it.
ContractBars
contract(const std::string& symbol, const std::vector<int>& closes, std::size_t from = 0)
{
    ContractBars item;
    item.terms.instrument = {{"SHFE", symbol}, "CNY", d("1"), d("1"), d("10")};
    item.terms.costs = {d("0"), d("3"), d("3"), d("3"), d("0.1"), d("0"), d("0"), d("0")};
    item.cost_schedule = {{days.front(), "test", item.terms.costs}};
    for (std::size_t i = 0; i < closes.size(); ++i) {
        const auto close = Decimal::parse(std::to_string(closes[i]));
        item.bars.push_back(
            {days[from + i],
                trading_day_close_ns(days[from + i]),
                close,
                close + d("10"),
                close - d("10"),
                close,
                d("1000")});
    }
    return item;
}

std::vector<DaySettlement> settled(const ContractBars& item)
{
    std::vector<DaySettlement> result;
    for (const auto& bar : item.bars)
        result.push_back({bar.trading_day, bar.close});
    return result;
}

OrderLimitsConfig limits(const char* order = "10", const char* gross = "10")
{
    return {d(order), d(gross), 10, d(gross)};
}

// What a replay reported, in order.
struct Seen final : Replay::Observer
{
    std::vector<std::string> events;
    std::vector<RiskDecision> decisions;
    std::vector<std::int64_t> settled_at;

    void advanced(
        std::size_t index,
        const std::optional<PaperExecution::Target>& intent,
        const RiskDecision& decision,
        std::size_t) override
    {
        events.push_back(
            "advanced " + std::to_string(index) + (intent ? " " + intent->order_id : ""));
        decisions.push_back(decision);
    }

    void marked(std::size_t index) override
    {
        events.push_back("marked " + std::to_string(index));
    }

    void settled(
        const PaperReplaySchedule::Day& day,
        std::int64_t at,
        const std::optional<PaperExecution::MarginRevaluation>&) override
    {
        events.push_back("settled " + day.trading_day);
        settled_at.push_back(at);
    }
};

// Replays every day, wanting `lots` of unit 0 after each timestamp.
void run(Replay& replay, Seen& seen, std::optional<Decimal> lots)
{
    while (!replay.finished()) {
        while (!replay.day_done()) {
            replay.next(seen);
            replay.want(0, lots);
            replay.decide();
        }
        replay.settle(seen);
    }
}

} // namespace

TEST(Replay, ATargetFillsOnTheNextBarAtItsOpenPlusSlippage)
{
    auto item = contract("rb2605", {100, 110, 120, 115});
    item.slippage_ticks = 1;
    Replay replay(d("1000000"), {item}, {settled(item)}, {}, limits());
    Seen seen;
    run(replay, seen, d("1"));
    const auto& account = replay.execution().account();
    ASSERT_EQ(account.fills().size(), 1U);
    // Decided after the first bar (100); executes at the second bar's open (110) + 1 tick slippage.
    EXPECT_EQ(account.fills()[0].execution.order_id, "strategy.0");
    EXPECT_EQ(account.fills()[0].execution.price, d("111"));
    EXPECT_EQ(account.realized(), d("40"));
    EXPECT_EQ(
        seen.events,
        (std::vector<std::string>{
            "advanced 0",
            "marked 0",
            "settled 2026-01-05",
            "advanced 1 strategy.0",
            "marked 1",
            "settled 2026-01-06",
            "advanced 2 strategy.1",
            "marked 2",
            "settled 2026-01-07",
            "advanced 3 strategy.2",
            "marked 3",
            "settled 2026-01-08"
        }));
}

TEST(Replay, WhatABarDoesNotFillExpiresAfterIt)
{
    // The second bar has zero volume: the order placed for it cannot fill and expires.
    auto item = contract("rb2605", {100, 130, 140});
    item.bars[1].volume = d("0");
    Replay replay(d("1000000"), {item}, {settled(item)}, {}, limits());
    Seen seen;
    replay.next(seen);
    replay.want(0, d("1"));
    replay.decide();
    replay.settle(seen);
    replay.next(seen);
    // Nothing is wanted any more: the standing target is not placed again.
    replay.want(0, std::nullopt);
    replay.decide();
    replay.settle(seen);
    replay.next(seen);
    replay.settle(seen);
    const auto& account = replay.execution().account();
    EXPECT_TRUE(account.fills().empty());
    ASSERT_EQ(account.orders().size(), 1U);
    EXPECT_EQ(account.orders()[0].order.state(), OrderState::cancelled);
    EXPECT_TRUE(replay.finished());
}

TEST(Replay, NothingIsPlacedWithoutADecision)
{
    const auto item = contract("rb2605", {100, 110, 120, 115});
    Replay replay(d("1000000"), {item}, {settled(item)}, {}, limits());
    Seen seen;
    while (!replay.finished()) {
        while (!replay.day_done()) {
            replay.next(seen);
            replay.want(0, d("1"));
        }
        replay.settle(seen);
    }
    EXPECT_TRUE(replay.execution().account().orders().empty());
}

TEST(Replay, ATargetAboveTheOrderLimitFillsInLegsAndReversesInOneBar)
{
    const auto item = contract("rb2605", {100, 110, 120, 115});
    Replay replay(d("1000000"), {item}, {settled(item)}, {}, limits("5", "20"));
    Seen seen;
    // Day 0: ask for +20 lots (4 legs of 5).
    replay.next(seen);
    replay.want(0, d("20"));
    replay.decide();
    replay.settle(seen);
    // Day 1: all 4 legs fill in the same bar; then reverse to -5 lots.
    replay.next(seen);
    const auto& account = replay.execution().account();
    ASSERT_EQ(account.fills().size(), 4U);
    for (std::size_t i = 0; i < 4; ++i) {
        EXPECT_EQ(account.fills()[i].execution.quantity, d("5"));
        EXPECT_EQ(account.fills()[i].execution.price, item.bars[1].open);
    }
    replay.want(0, d("-5"));
    replay.decide();
    replay.settle(seen);
    // Day 2: closes +20 in 4 legs of 5 and opens -5 in 1 leg within the same bar.
    replay.next(seen);
    ASSERT_EQ(account.fills().size(), 9U);
    ASSERT_EQ(account.positions().size(), 1U);
    EXPECT_EQ(account.positions()[0].side, Side::sell);
    EXPECT_EQ(account.positions()[0].quantity, d("5"));
}

TEST(Replay, WithdrawingATargetCancelsItsPendingLegAndKeepsItsPosition)
{
    auto item = contract("rb2605", {100, 100, 100, 100});
    item.bars[1].volume = d("10"); // participation cap = 1 lot on bar 1
    Replay replay(d("1000000"), {item}, {settled(item)}, {}, limits("1"));
    Seen seen;
    replay.next(seen);
    replay.want(0, d("3"));
    replay.decide();
    replay.settle(seen);
    replay.next(seen);
    replay.decide();
    replay.want(0, std::nullopt);
    replay.settle(seen);
    replay.next(seen);
    const auto& account = replay.execution().account();
    ASSERT_EQ(account.fills().size(), 1U);
    EXPECT_EQ(account.fills()[0].execution.quantity, d("1"));
    ASSERT_EQ(account.positions().size(), 1U);
    replay.want(0, d("0"));
    replay.decide();
    replay.settle(seen);
    replay.next(seen);
    replay.settle(seen);
    EXPECT_EQ(account.fills().size(), 2U);
    EXPECT_TRUE(account.positions().empty());
}

TEST(Replay, RiskRejectionsAreReportedAndPlaceNothing)
{
    const auto item = contract("rb2605", {100, 110, 120, 115});
    Replay replay(d("1000000"), {item}, {settled(item)}, {}, limits("5", "2"));
    Seen seen;
    run(replay, seen, d("5"));
    EXPECT_TRUE(replay.execution().account().orders().empty());
    ASSERT_EQ(seen.decisions.size(), 4U);
    EXPECT_TRUE(seen.decisions[0].allowed());
    EXPECT_EQ(seen.decisions[1].reason, RiskReason::gross_quantity);
}

TEST(Replay, ADayWithoutBarsIsSettledAtItsClose)
{
    // The contract has no bar on the third day but settles on it.
    auto item = contract("rb2605", {100, 110, 120, 115});
    auto days_settled = settled(item);
    item.bars.erase(item.bars.begin() + 2);
    Replay replay(d("1000000"), {item}, {days_settled}, {}, limits());
    Seen seen;
    run(replay, seen, d("1"));
    ASSERT_EQ(seen.settled_at.size(), 4U);
    EXPECT_EQ(seen.settled_at[1], trading_day_close_ns(days[1]));
    EXPECT_EQ(seen.settled_at[2], trading_day_close_ns(days[2]));
}

TEST(Replay, ADominantSeriesRollsAtTheOpenAndShowsEachMonthsOwnPrices)
{
    // 2605 is dominant on the first two days, 2610 from the third; 2610 joins a
    // day early (with its own cost schedule starting on days[1]) and 2605 stays
    // for its bar on the roll day.
    const auto left = contract("rb2605", {100, 110, 120});
    auto right = contract("rb2610", {125, 136, 148}, 1);
    right.cost_schedule = {{days[1], "test", right.terms.costs}};
    DominantSeries series{{{days[0], 0}, {days[2], 1}}};
    Replay replay(d("1000000"), {left, right}, {settled(left), settled(right)}, {series}, limits());
    ASSERT_EQ(replay.units(), 1U);
    EXPECT_EQ(replay.unit(0), 0U);
    EXPECT_EQ(replay.unit(1), 0U);
    EXPECT_EQ(replay.series(1), std::optional<std::size_t>{0});
    Seen seen;
    std::vector<std::pair<std::string, Decimal>> leading;
    bool decided = false;
    while (!replay.finished()) {
        while (!replay.day_done()) {
            for (const auto& bar : replay.next(seen))
                if (bar.leading)
                    leading.emplace_back(bar.bar.trading_day, bar.bar.close);
            if (!decided) {
                replay.want(0, d("1"));
                replay.decide();
                decided = true;
            }
        }
        replay.settle(seen);
    }
    // Adjusting earlier months is the strategy context's, not the replay's.
    EXPECT_EQ(leading.front(), std::make_pair(days[0], d("100")));
    EXPECT_EQ(leading.back(), std::make_pair(days[3], d("148")));
    const auto& account = replay.execution().account();
    std::vector<std::string> fills;
    for (const auto& fill : account.fills())
        fills.push_back(fill.execution.order_id + "@" + fill.execution.price.str());
    // Bought 2605 at 110 (open of days[1]); on the roll day 2605 closes at its
    // open of 120 and 2610 takes the series' lot at its open of 136 even without
    // a new strategy decision on the roll day.
    ASSERT_GE(fills.size(), 2U);
    EXPECT_EQ(fills[0], "strategy.0@110");
    std::vector<std::string> rolls;
    for (const auto& fill : fills)
        if (fill.starts_with("roll."))
            rolls.push_back(fill.substr(fill.find('@')));
    EXPECT_EQ(rolls, (std::vector<std::string>{"@120", "@136"}));
    for (const auto& lot : account.positions())
        EXPECT_EQ(lot.instrument.symbol, "rb2610");
}

TEST(Replay, ADayIsReplayedAndSettledInOrder)
{
    const auto item = contract("rb2605", {100, 110});
    Replay replay(d("1000000"), {item}, {settled(item)}, {}, limits());
    Seen seen;
    EXPECT_THROW(replay.settle(seen), std::logic_error);
    replay.next(seen);
    EXPECT_THROW(replay.next(seen), std::logic_error);
    replay.settle(seen);
    std::stop_source stop;
    stop.request_stop();
    EXPECT_THROW(replay.next(seen, stop.get_token()), std::runtime_error);
}

TEST(Replay, IntradayFloatingGainDoesNotInflateMarginOrCancelOpenOrders)
{
    // Deposit 1,000; 1 lot at 900 with multiplier 10 and 10% margin takes 900 margin,
    // leaving 100 available before settlement. A rally to 1,200 intraday leaves
    // margin at 900 (basis price) until daily settlement credits the +3,000 P&L.
    auto item = contract("rb2605", {900, 900, 1200});
    item.terms.costs = {d("0"), d("0"), d("0"), d("0"), d("0.1"), d("0"), d("0"), d("0")};
    item.cost_schedule = {{days.front(), "test", item.terms.costs}};
    Replay replay(d("1000"), {item}, {settled(item)}, {}, limits());
    Seen seen;
    replay.next(seen);
    replay.want(0, d("1"));
    replay.decide();
    replay.settle(seen);
    replay.next(seen);
    const auto& account = replay.execution().account();
    ASSERT_EQ(account.positions().size(), 1U);
    EXPECT_EQ(account.margin(), d("900"));
    EXPECT_EQ(account.available(), d("100"));
    replay.settle(seen);
    // Day 2: price jumps to 1200 intraday; before settlement, margin stays 900 and available stays 100.
    replay.next(seen);
    EXPECT_EQ(account.margin(), d("900"));
    EXPECT_EQ(account.available(), d("100"));
    replay.settle(seen);
    // After settlement at 1200: balance is 4000, margin revalues to 1200, available is 2800.
    EXPECT_EQ(account.balance(), d("4000"));
    EXPECT_EQ(account.margin(), d("1200"));
    EXPECT_EQ(account.available(), d("2800"));
}

TEST(Replay, AsymmetricMarginsAndLazyCloseTodayFeeValidation)
{
    auto item = contract("rb2605", {100, 100, 100});
    const auto settlements = settled(item);
    item.terms.costs =
        {d("0"), d("3"), d("0"), d("3"), d("0.1"), d("0"), d("0"), d("0"), d("0.15"), false};
    item.cost_schedule = {{days.front(), "test", item.terms.costs}};
    // Add a second intraday bar on Day 1 to test closing today's position when close-today fee is missing.
    item.bars[1].timestamp_ns -= 60'000'000'000LL;
    item.bars.insert(
        item.bars.begin() + 2,
        {days[1],
            item.bars[1].timestamp_ns + 60'000'000'000LL,
            d("100"),
            d("110"),
            d("90"),
            d("100"),
            d("1000")});
    Replay replay(d("10000"), {item}, {settlements}, {}, limits());
    Seen seen;
    replay.next(seen);
    replay.want(0, d("-1"));
    replay.decide();
    replay.settle(seen);
    // Day 1 morning: short 1 lot at 100 (multiplier 10) uses short_margin_rate 0.15 -> margin 150.
    replay.next(seen);
    EXPECT_EQ(replay.execution().account().margin(), d("150"));
    // Attempting to close today's short on Day 1 afternoon fails because close-today fee is missing.
    replay.want(0, d("0"));
    replay.decide();
    EXPECT_THROW(replay.next(seen), std::invalid_argument);
}

TEST(Replay, APlainCloseCoveredByYesterdayLotsNeedsNoCloseTodayFee)
{
    // DCE closes yesterday's lots first: without close-today fees a close they cover still works.
    const Instrument spec{{"DCE", "m2609"}, "CNY", d("1"), d("1"), d("10")};
    const FuturesCosts
        costs{d("0"), d("3"), d("0"), d("3"), d("0.1"), d("0"), d("0"), d("0"), d("0.1"), false};
    FuturesAccount account(d("100000"), {{spec, costs}});
    account.mark(spec.id, d("100"));
    account.submit({"old", spec.id, Side::buy, d("2"), d("100")}, Offset::open);
    account.fill({"fill.old", "old", d("2"), d("100")});
    account.settle({d("100")});
    account.submit({"new", spec.id, Side::buy, d("1"), d("100")}, Offset::open);
    account.fill({"fill.new", "new", d("1"), d("100")});
    EXPECT_NO_THROW(
        account.submit({"close", spec.id, Side::sell, d("2"), d("100")}, Offset::close));
    EXPECT_NO_THROW(account.fill({"fill.close", "close", d("2"), d("100")}));
    // Only today's lot is left: closing it does need the missing fee.
    EXPECT_THROW(
        account.submit({"rest", spec.id, Side::sell, d("1"), d("100")}, Offset::close),
        std::invalid_argument);
}
