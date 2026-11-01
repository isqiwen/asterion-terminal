#include "strategy_orders.hpp"
#include <asterion/futures/position_target.hpp>
#include <algorithm>
#include <set>
#include <stdexcept>

namespace asterion::trading {
namespace {

bool working(const BrokerOrder& order)
{
    return order.status == BrokerOrderStatus::submitted
        || order.status == BrokerOrderStatus::accepted
        || order.status == BrokerOrderStatus::partially_filled;
}

} // namespace

StrategyOrderPlan plan_strategy_orders(
    std::span<const PositionTarget> targets,
    std::span<const FuturesContract> allowed_contracts,
    const BrokerSnapshot& account,
    bool has_unconfirmed_orders)
{
    std::set<InstrumentId> identities;
    for (const auto& target : targets) {
        const auto contract = std::ranges::find_if(allowed_contracts, [&](const auto& allowed) {
            return allowed.instrument.id == target.instrument;
        });
        if (!identities.insert(target.instrument).second || contract == allowed_contracts.end())
            throw std::invalid_argument("strategy targets require unique allowed contracts");
        if (!target.quantity.multiple_of(contract->instrument.quantity_increment))
            throw std::invalid_argument(
                "strategy target quantity must follow the contract lot increment");
    }
    if (targets.empty())
        return {};
    if (account.phase != BrokerPhase::ready || !account.positions_reconciled)
        return {StrategyPlanState::waiting_for_account, {}};
    if (has_unconfirmed_orders || std::ranges::any_of(account.orders, working))
        return {StrategyPlanState::waiting_for_orders, {}};

    std::vector<StrategyOrder> reductions, openings;
    for (const auto& target : targets) {
        HeldPosition long_position{Side::buy, {}, {}}, short_position{Side::sell, {}, {}};
        for (const auto& position : account.positions) {
            if (position.instrument != target.instrument)
                continue;
            auto& holding = position.side == Side::buy ? long_position : short_position;
            holding.today = holding.today + position.today;
            holding.yesterday = holding.yesterday + position.yesterday;
        }
        // Keep the desired side only. Using the shared per-side conversion
        // also reduces hedged holdings without cancelling them out as net lots.
        for (const auto& held : {long_position, short_position}) {
            const auto desired = held.side == Side::buy ? std::max(target.quantity, Decimal{})
                                                        : std::min(target.quantity, Decimal{});
            for (const auto& order :
                target_orders(desired, held, close_policy(target.instrument.venue))) {
                auto& batch = order.offset == Offset::open ? openings : reductions;
                batch.push_back({target.instrument, order.side, order.offset, order.quantity});
            }
        }
    }
    if (!reductions.empty())
        return {StrategyPlanState::reducing, std::move(reductions)};
    if (!openings.empty())
        return {StrategyPlanState::opening, std::move(openings)};
    return {};
}

} // namespace asterion::trading
