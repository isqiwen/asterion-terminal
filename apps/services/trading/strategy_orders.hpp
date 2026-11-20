#pragma once
#include "broker_execution.hpp"
#include <asterion/futures/futures.hpp>
#include <span>

namespace asterion::trading {

struct PositionTarget
{
    InstrumentId instrument;
    Decimal quantity; // Signed whole lots; omitted contracts are unchanged.
};

struct StrategyOrder
{
    InstrumentId instrument;
    Side side;
    Offset offset;
    Decimal quantity;
};

enum class StrategyPlanState
{
    settled,
    waiting_for_account,
    waiting_for_orders,
    reducing,
    opening
};

struct StrategyOrderPlan
{
    StrategyPlanState state = StrategyPlanState::settled;
    std::vector<StrategyOrder> orders;
};

// Pure planning from the account owner's current facts and validated policy.
// Any working or unconfirmed order blocks planning, including partial fills.
// Reductions across all targets precede opens; replan from reconciled positions
// after each batch. Nothing here reserves risk, chooses a price or permits a send.
StrategyOrderPlan plan_strategy_orders(
    std::span<const PositionTarget> targets,
    std::span<const FuturesContract> allowed_contracts,
    const BrokerSnapshot& account,
    bool has_unconfirmed_orders);

} // namespace asterion::trading
