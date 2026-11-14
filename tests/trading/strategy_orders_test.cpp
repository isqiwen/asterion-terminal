#include "strategy_orders.hpp"
#include <gtest/gtest.h>

using namespace asterion;
using namespace asterion::trading;

namespace {

Decimal lots(int value)
{
    return Decimal::parse(std::to_string(value));
}

FuturesContract contract(const char* venue = "SHFE", const char* product = "rb")
{
    const auto symbol = std::string(product) + (std::string_view(venue) == "CZCE" ? "610" : "2610");
    FuturesContract result{
        {{venue, symbol}, "CNY", lots(1), lots(1), lots(10)},
        product,
        "2026-10"
    };
    result.validate();
    return result;
}

class StrategyOrders : public ::testing::Test
{
protected:
    void SetUp() override
    {
        account.phase = BrokerPhase::ready;
        account.positions_reconciled = true;
    }

    StrategyOrderPlan plan(int quantity, bool unconfirmed = false)
    {
        const PositionTarget target{allowed[0].instrument.id, lots(quantity)};
        return plan_strategy_orders({&target, 1}, allowed, account, unconfirmed);
    }

    void hold(Side side, int today, int yesterday, std::size_t index = 0)
    {
        account.positions.push_back(
            {allowed[index].instrument.id, side, lots(today), lots(yesterday)});
    }

    BrokerSnapshot account;
    std::vector<FuturesContract> allowed{contract(), contract("SHFE", "cu")};
};

} // namespace

TEST_F(StrategyOrders, OpensOnlyTheMissingLongOrShortLots)
{
    for (auto side : {Side::buy, Side::sell}) {
        account.positions.clear();
        hold(side, 1, 2);
        const auto result = plan(side == Side::buy ? 5 : -5);
        ASSERT_EQ(result.state, StrategyPlanState::opening);
        ASSERT_EQ(result.orders.size(), 1);
        EXPECT_EQ(result.orders[0].instrument, allowed[0].instrument.id);
        EXPECT_EQ(result.orders[0].side, side);
        EXPECT_EQ(result.orders[0].offset, Offset::open);
        EXPECT_EQ(result.orders[0].quantity, lots(2));
    }
}

TEST_F(StrategyOrders, SplitsShanghaiAndEnergyClosesByYesterdayThenToday)
{
    for (const auto* venue : {"SHFE", "INE"}) {
        allowed[0] = contract(venue);
        for (auto side : {Side::buy, Side::sell}) {
            account.positions.clear();
            hold(side, 3, 2);
            const auto result = plan(side == Side::buy ? 1 : -1);
            ASSERT_EQ(result.state, StrategyPlanState::reducing);
            ASSERT_EQ(result.orders.size(), 2);
            EXPECT_EQ(result.orders[0].side, side == Side::buy ? Side::sell : Side::buy);
            EXPECT_EQ(result.orders[1].side, result.orders[0].side);
            EXPECT_EQ(result.orders[0].offset, Offset::close_yesterday);
            EXPECT_EQ(result.orders[0].quantity, lots(2));
            EXPECT_EQ(result.orders[1].offset, Offset::close_today);
            EXPECT_EQ(result.orders[1].quantity, lots(2));
        }
    }
}

TEST_F(StrategyOrders, DoesNotCloseTodayWhenYesterdayCoversTheReduction)
{
    hold(Side::buy, 3, 4);
    const auto result = plan(5);
    ASSERT_EQ(result.orders.size(), 1);
    EXPECT_EQ(result.orders[0].offset, Offset::close_yesterday);
    EXPECT_EQ(result.orders[0].quantity, lots(2));
}

TEST_F(StrategyOrders, OtherExchangesUseOneGenericClose)
{
    for (const auto* venue : {"DCE", "CZCE", "CFFEX", "GFEX"}) {
        allowed[0] = contract(venue);
        account.positions.clear();
        hold(Side::sell, 2, 3);
        const auto result = plan(0);
        ASSERT_EQ(result.orders.size(), 1);
        EXPECT_EQ(result.orders[0].side, Side::buy);
        EXPECT_EQ(result.orders[0].offset, Offset::close);
        EXPECT_EQ(result.orders[0].quantity, lots(5));
    }
}

TEST_F(StrategyOrders, ReversalWaitsForTerminalOrdersAndReconciledPositionsBeforeOpening)
{
    hold(Side::buy, 2, 0);
    auto result = plan(-3);
    ASSERT_EQ(result.orders.size(), 1);
    EXPECT_EQ(result.state, StrategyPlanState::reducing);
    EXPECT_EQ(result.orders[0].offset, Offset::close_today);
    EXPECT_EQ(result.orders[0].quantity, lots(2));

    BrokerOrder close;
    close.instrument = allowed[0].instrument.id;
    close.side = Side::sell;
    close.offset = Offset::close_today;
    close.quantity = lots(2);
    close.filled = lots(1);
    close.status = BrokerOrderStatus::partially_filled;
    account.orders.push_back(close);
    account.positions[0].today = lots(1);
    EXPECT_EQ(plan(-3).state, StrategyPlanState::waiting_for_orders);
    EXPECT_TRUE(plan(-3).orders.empty());

    account.orders[0].status = BrokerOrderStatus::filled;
    account.orders[0].filled = lots(2);
    account.positions_reconciled = false;
    EXPECT_EQ(plan(-3).state, StrategyPlanState::waiting_for_account);
    account.positions.clear();
    account.positions_reconciled = true;
    result = plan(-3);
    ASSERT_EQ(result.orders.size(), 1);
    EXPECT_EQ(result.state, StrategyPlanState::opening);
    EXPECT_EQ(result.orders[0].side, Side::sell);
    EXPECT_EQ(result.orders[0].offset, Offset::open);
    EXPECT_EQ(result.orders[0].quantity, lots(3));
}

TEST_F(StrategyOrders, PartiallyFilledCancellationUsesTheRemainingActualHolding)
{
    hold(Side::buy, 1, 0);
    BrokerOrder close;
    close.quantity = lots(2);
    close.filled = lots(1);
    close.status = BrokerOrderStatus::cancelled;
    account.orders.push_back(close);
    const auto result = plan(0);
    ASSERT_EQ(result.orders.size(), 1);
    EXPECT_EQ(result.state, StrategyPlanState::reducing);
    EXPECT_EQ(result.orders[0].quantity, lots(1));
}

TEST_F(StrategyOrders, MatchingNetDoesNotHideHedgedExposure)
{
    hold(Side::buy, 3, 2);
    hold(Side::sell, 0, 3);
    const auto result = plan(2);
    ASSERT_EQ(result.state, StrategyPlanState::reducing);
    Decimal buys, sells;
    for (const auto& order : result.orders) {
        EXPECT_NE(order.offset, Offset::open);
        auto& quantity = order.side == Side::buy ? buys : sells;
        quantity = quantity + order.quantity;
    }
    EXPECT_EQ(buys, lots(3));
    EXPECT_EQ(sells, lots(3));
}

TEST_F(StrategyOrders, AllReductionsPrecedeAnyOpeningAcrossContracts)
{
    hold(Side::buy, 2, 0, 1);
    const std::vector<PositionTarget> targets{
        {allowed[0].instrument.id, lots(4)},
        {allowed[1].instrument.id, lots(0)}
    };
    auto result = plan_strategy_orders(targets, allowed, account, false);
    ASSERT_EQ(result.orders.size(), 1);
    EXPECT_EQ(result.state, StrategyPlanState::reducing);
    EXPECT_EQ(result.orders[0].instrument, allowed[1].instrument.id);
    account.positions.clear();
    result = plan_strategy_orders(targets, allowed, account, false);
    ASSERT_EQ(result.orders.size(), 1);
    EXPECT_EQ(result.state, StrategyPlanState::opening);
    EXPECT_EQ(result.orders[0].instrument, allowed[0].instrument.id);
}

TEST_F(StrategyOrders, EqualOrOmittedTargetsLeaveHoldingsUnchanged)
{
    hold(Side::buy, 1, 2);
    hold(Side::sell, 5, 0, 1);
    EXPECT_EQ(plan(3).state, StrategyPlanState::settled);
    EXPECT_TRUE(plan(3).orders.empty());
    EXPECT_TRUE(plan_strategy_orders({}, allowed, account, false).orders.empty());
}

TEST_F(StrategyOrders, WorkingOrdersOnAnyContractAndUnknownOutcomesBlockPlanning)
{
    BrokerOrder order;
    order.instrument = allowed[1].instrument.id;
    account.orders.push_back(order);
    for (auto status :
        {BrokerOrderStatus::submitted,
            BrokerOrderStatus::accepted,
            BrokerOrderStatus::partially_filled}) {
        account.orders[0].status = status;
        EXPECT_EQ(plan(1).state, StrategyPlanState::opening);
        ASSERT_EQ(plan(1).orders.size(), 1U);
    }
    account.orders[0].instrument = allowed[0].instrument.id;
    for (auto status :
        {BrokerOrderStatus::submitted,
            BrokerOrderStatus::accepted,
            BrokerOrderStatus::partially_filled}) {
        account.orders[0].status = status;
        EXPECT_EQ(plan(1).state, StrategyPlanState::waiting_for_orders);
        EXPECT_TRUE(plan(1).orders.empty());
    }
    for (auto status :
        {BrokerOrderStatus::filled, BrokerOrderStatus::cancelled, BrokerOrderStatus::rejected}) {
        account.orders[0].status = status;
        EXPECT_EQ(plan(1).state, StrategyPlanState::opening);
        EXPECT_EQ(plan(1, true).state, StrategyPlanState::waiting_for_orders);
        EXPECT_TRUE(plan(1, true).orders.empty());
    }
    account.positions_reconciled = false;
    account.unreconciled_instruments = {allowed[0].instrument.id};
    const std::vector<PositionTarget> multi{
        {allowed[0].instrument.id, lots(1)},
        {allowed[1].instrument.id, lots(2)}
    };
    const auto next = plan_strategy_orders(multi, allowed, account, false);
    ASSERT_EQ(next.state, StrategyPlanState::opening);
    ASSERT_EQ(next.orders.size(), 1U);
    EXPECT_EQ(next.orders[0].instrument, allowed[1].instrument.id);
    EXPECT_EQ(next.orders[0].quantity, lots(2));
}

TEST_F(StrategyOrders, UnreadyOrUnreconciledAccountsNeverImplyAnEmptyPosition)
{
    hold(Side::sell, 2, 3);
    for (auto phase :
        {BrokerPhase::disconnected,
            BrokerPhase::connecting,
            BrokerPhase::authenticating,
            BrokerPhase::logging_in,
            BrokerPhase::confirming,
            BrokerPhase::synchronizing,
            BrokerPhase::error}) {
        account.phase = phase;
        EXPECT_EQ(plan(0).state, StrategyPlanState::waiting_for_account);
        EXPECT_TRUE(plan(0).orders.empty());
    }
    account.phase = BrokerPhase::ready;
    account.positions_reconciled = false;
    EXPECT_EQ(plan(0).state, StrategyPlanState::waiting_for_account);
}

TEST_F(StrategyOrders, RefusesDuplicateUnknownAndOffGridTargets)
{
    std::vector<PositionTarget> targets{{allowed[0].instrument.id, lots(1)}};
    targets.push_back(targets[0]);
    EXPECT_THROW(plan_strategy_orders(targets, allowed, account, false), std::invalid_argument);
    targets.resize(1);
    targets[0].instrument.symbol = "rb9999";
    EXPECT_THROW(plan_strategy_orders(targets, allowed, account, false), std::invalid_argument);
    targets[0] = {allowed[0].instrument.id, Decimal::parse("0.5")};
    EXPECT_THROW(plan_strategy_orders(targets, allowed, account, false), std::invalid_argument);
    allowed[0].instrument.quantity_increment = lots(2);
    EXPECT_THROW(plan(-3), std::invalid_argument);
    EXPECT_EQ(plan(-4).orders[0].quantity, lots(4));
}
