#include <asterion/futures/portfolio_targets.hpp>
#include <set>
#include <stdexcept>

namespace asterion {
namespace {

Decimal magnitude(Decimal value)
{
    return value < Decimal{} ? Decimal{} - value : value;
}

} // namespace

PortfolioTargets fixed_weight_targets(
    Decimal equity,
    std::span<const WeightedContract> members,
    Decimal gross_weight_limit,
    Decimal net_weight_limit,
    std::stop_token stop)
{
    const auto cancelled = [&] {
        if (stop.stop_requested())
            throw std::runtime_error("portfolio target calculation cancelled");
    };
    cancelled();
    if (equity <= Decimal{} || gross_weight_limit < Decimal{} || net_weight_limit < Decimal{}
        || members.size() < 2 || members.size() > 20)
        throw std::invalid_argument(
            "fixed-weight targets require positive equity and explicit nonnegative budgets for 2 to 20 contracts");
    PortfolioTargets result;
    result.equity = equity;
    result.gross_limit = gross_weight_limit;
    result.net_limit = net_weight_limit;
    std::set<std::pair<std::string, std::string>> identities;
    for (const auto& member : members) {
        cancelled();
        if (member.instrument.venue.empty() || member.instrument.symbol.empty()
            || !identities.emplace(member.instrument.venue, member.instrument.symbol).second)
            throw std::invalid_argument(
                "fixed-weight targets require unique actual contract identities");
        if (member.price <= Decimal{} || member.multiplier <= Decimal{})
            throw std::invalid_argument(
                "fixed-weight sizing is not applicable to nonpositive prices or multipliers");
        result.gross_weight = result.gross_weight + magnitude(member.weight);
        result.net_weight = result.net_weight + member.weight;
    }
    if (result.gross_weight > gross_weight_limit || magnitude(result.net_weight) > net_weight_limit)
        throw std::invalid_argument(
            "fixed-weight targets exceed the declared gross or net nominal budget");
    result.members.reserve(members.size());
    for (const auto& member : members) {
        cancelled();
        const auto requested = equity * member.weight;
        const auto per_lot = member.price * member.multiplier;
        // Both operands have the same exact scale. Integer division chooses
        // whole lots directly, without first rounding a fractional Decimal lot.
        const auto quantity = Decimal::parse(std::to_string(requested.raw() / per_lot.raw()));
        const auto allocated = quantity * per_lot;
        result.members.push_back({member, requested, quantity, allocated, requested - allocated});
        result.requested_gross_notional = result.requested_gross_notional + magnitude(requested);
        result.requested_net_notional = result.requested_net_notional + requested;
        result.allocated_gross_notional = result.allocated_gross_notional + magnitude(allocated);
        result.allocated_net_notional = result.allocated_net_notional + allocated;
    }
    result.unallocated_nominal_budget =
        equity * gross_weight_limit - result.requested_gross_notional;
    return result;
}

} // namespace asterion
