#pragma once
#include <asterion/futures/market.hpp>
#include <span>
#include <stop_token>
#include <vector>

namespace asterion {

struct WeightedContract
{
    InstrumentId instrument;
    Decimal weight, price, multiplier;
    Decimal margin_rate = {};
    Decimal current_quantity = {};
};

struct WeightedTarget
{
    WeightedContract contract;
    Decimal requested_notional, quantity, allocated_notional, nominal_remainder;
};

struct PortfolioTargets
{
    Decimal equity, gross_weight, net_weight, gross_limit, net_limit;
    Decimal requested_gross_notional, requested_net_notional;
    Decimal allocated_gross_notional, allocated_net_notional, unallocated_nominal_budget;
    std::vector<WeightedTarget> members;
};

// Completed-boundary inputs only. Whole-lot targets truncate toward zero;
// optional margin ceiling scales weights uniformly down to fit, and optional
// min_rebalance_weight suppresses integer-lot rounding churn below the drift floor.
PortfolioTargets fixed_weight_targets(
    Decimal equity,
    std::span<const WeightedContract> members,
    Decimal gross_weight_limit,
    Decimal net_weight_limit,
    std::stop_token stop = {},
    Decimal max_margin_ratio = {},
    Decimal min_rebalance_weight = {});

} // namespace asterion
