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

std::shared_ptr<const RiskPort> limits(const char* lots = "10")
{
    auto value = std::make_shared<OrderLimits>(OrderLimitsConfig{d(lots), d(lots), 10});
    value->start();
    return value;
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

TEST(Replay, ATargetFillsOnTheNextBarAtTheDecidedClose)
{
    const auto item = contract("rb2605", {100, 110, 120, 115});
    Replay replay(d("1000000"), {item}, {settled(item)}, {}, limits());
    Seen seen;
    run(replay, seen, d("1"));
    const auto& account = replay.execution().account();
    ASSERT_EQ(account.fills().size(), 1U);
    // Decided at the first close of 100; the second bar's low of 100 reaches it.
    EXPECT_EQ(account.fills()[0].execution.order_id, "strategy.0");
    EXPECT_EQ(account.fills()[0].execution.price, d("100"));
    EXPECT_EQ(account.realized(), d("150"));
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
    // The second bar never comes down to 100: the order placed for it expires.
    const auto item = contract("rb2605", {100, 130, 140});
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

TEST(Replay, RiskRejectionsAreReportedAndPlaceNothing)
{
    const auto item = contract("rb2605", {100, 110, 120, 115});
    Replay replay(d("1000000"), {item}, {settled(item)}, {}, limits("2"));
    Seen seen;
    run(replay, seen, d("5"));
    EXPECT_TRUE(replay.execution().account().orders().empty());
    ASSERT_EQ(seen.decisions.size(), 4U);
    EXPECT_TRUE(seen.decisions[0].allowed());
    EXPECT_EQ(seen.decisions[1].reason, RiskReason::order_quantity);
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

TEST(Replay, ADominantSeriesRollsAtTheOpenAndSeesAdjustedPrices)
{
    // 2605 is dominant on the first two days, 2610 from the third; 2610 joins a
    // day early and 2605 stays for its bar on the roll day.
    const auto left = contract("rb2605", {100, 110, 120});
    const auto right = contract("rb2610", {125, 136, 148}, 1);
    DominantSeries series{{{days[0], 0, d("1.1")}, {days[2], 1, d("1")}}};
    Replay replay(d("1000000"), {left, right}, {settled(left), settled(right)}, {series}, limits());
    ASSERT_EQ(replay.units(), 1U);
    EXPECT_EQ(replay.unit(0), 0U);
    EXPECT_EQ(replay.unit(1), 0U);
    EXPECT_EQ(replay.series(1), std::optional<std::size_t>{0});
    Seen seen;
    std::vector<std::pair<std::string, Decimal>> leading;
    while (!replay.finished()) {
        while (!replay.day_done()) {
            for (const auto& bar : replay.next(seen))
                if (bar.leading)
                    leading.emplace_back(bar.seen.trading_day, bar.seen.close);
            replay.want(0, d("1"));
            replay.decide();
        }
        replay.settle(seen);
    }
    // The left month's closes at the right month's level, rounded to the grid.
    EXPECT_EQ(leading.front(), std::make_pair(days[0], d("110")));
    EXPECT_EQ(leading.back(), std::make_pair(days[3], d("148")));
    const auto& account = replay.execution().account();
    std::vector<std::string> fills;
    for (const auto& fill : account.fills())
        fills.push_back(fill.execution.order_id + "@" + fill.execution.price.str());
    // Bought 2605 at 100; on the roll day 2605 closes at its open of 120 and
    // 2610 takes the series' lot at its open of 136 once 2605 is closed.
    ASSERT_GE(fills.size(), 2U);
    EXPECT_EQ(fills[0], "strategy.0@100");
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
