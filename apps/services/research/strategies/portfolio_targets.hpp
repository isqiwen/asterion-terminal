#pragma once
#include <asterion/domain/market.hpp>
#include <span>
#include <stop_token>
#include <vector>

namespace asterion {

struct WeightedContract
{
    InstrumentId instrument;
    Decimal weight, price, multiplier;
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

// Completed-boundary inputs only. Not an account, margin estimate or trade.
// Whole-lot targets truncate toward zero; no normalization or implicit rounding.
PortfolioTargets fixed_weight_targets(
    Decimal equity,
    std::span<const WeightedContract> members,
    Decimal gross_weight_limit,
    Decimal net_weight_limit,
    std::stop_token stop = {});

} // namespace asterion
