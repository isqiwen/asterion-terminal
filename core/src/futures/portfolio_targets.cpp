#include <asterion/futures/account.hpp>
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
    std::stop_token stop,
    Decimal max_margin_ratio,
    Decimal min_rebalance_weight)
{
    const auto cancelled = [&] {
        if (stop.stop_requested())
            throw std::runtime_error("portfolio target calculation cancelled");
    };
    cancelled();
    if (equity <= Decimal{} || gross_weight_limit < Decimal{} || net_weight_limit < Decimal{}
        || max_margin_ratio < Decimal{} || min_rebalance_weight < Decimal{} || members.empty()
        || members.size() > max_portfolio_contracts)
        throw std::invalid_argument(
            "fixed-weight targets require positive equity and explicit nonnegative budgets for 1 to 64 contracts");
    PortfolioTargets result;
    result.equity = equity;
    result.gross_limit = gross_weight_limit;
    result.net_limit = net_weight_limit;
    std::set<std::pair<std::string, std::string>> identities;
    Decimal implied_margin_ratio{};
    const auto one = Decimal::parse("1");
    for (const auto& member : members) {
        cancelled();
        if (member.instrument.venue.empty() || member.instrument.symbol.empty()
            || !identities.emplace(member.instrument.venue, member.instrument.symbol).second)
            throw std::invalid_argument(
                "fixed-weight targets require unique actual contract identities");
        if (member.price <= Decimal{} || member.multiplier <= Decimal{})
            throw std::invalid_argument(
                "fixed-weight sizing is not applicable to nonpositive prices or multipliers");
        if (member.margin_rate < Decimal{} || !member.current_quantity.multiple_of(one))
            throw std::invalid_argument(
                "fixed-weight sizing requires nonnegative margin rates and whole current lots");
        result.gross_weight = result.gross_weight + magnitude(member.weight);
        result.net_weight = result.net_weight + member.weight;
        implied_margin_ratio = implied_margin_ratio
            + multiply(magnitude(member.weight), member.margin_rate, Rounding::half_up);
    }
    if (result.gross_weight > gross_weight_limit || magnitude(result.net_weight) > net_weight_limit)
        throw std::invalid_argument(
            "fixed-weight targets exceed the declared gross or net nominal budget");
    Decimal scale = one;
    if (max_margin_ratio > Decimal{} && implied_margin_ratio > max_margin_ratio)
        scale = divide(max_margin_ratio, implied_margin_ratio, Rounding::toward_zero);
    const auto drift_floor = multiply(equity, min_rebalance_weight, Rounding::toward_zero);
    result.members.reserve(members.size());
    for (const auto& member : members) {
        cancelled();
        const auto scaled_weight =
            scale == one ? member.weight : multiply(member.weight, scale, Rounding::toward_zero);
        const auto requested = multiply(equity, scaled_weight, Rounding::toward_zero);
        const auto per_lot = member.price * member.multiplier;
        // Both operands have the same exact scale. Integer division chooses
        // whole lots directly, without first rounding a fractional Decimal lot.
        auto quantity = Decimal::from_integer(requested.raw() / per_lot.raw());
        if (min_rebalance_weight > Decimal{} && member.weight != Decimal{}
            && quantity != member.current_quantity) {
            const bool sign_flip = member.current_quantity != Decimal{}
                && ((member.current_quantity > Decimal{}) != (member.weight > Decimal{}));
            if (!sign_flip) {
                const auto current_notional = member.current_quantity * per_lot;
                if (magnitude(requested - current_notional) < drift_floor)
                    quantity = member.current_quantity;
            }
        }
        const auto allocated = quantity * per_lot;
        result.members.push_back({member, requested, quantity, allocated, requested - allocated});
        result.requested_gross_notional = result.requested_gross_notional + magnitude(requested);
        result.requested_net_notional = result.requested_net_notional + requested;
    }
    if (max_margin_ratio > Decimal{}) {
        const auto cent = Decimal::parse("0.01");
        const auto margin_cap = multiply(equity, max_margin_ratio, Rounding::toward_zero);
        auto lot_margin = [&](const WeightedTarget& item) {
            return quantize(
                multiply(
                    magnitude(item.allocated_notional),
                    item.contract.margin_rate,
                    Rounding::half_up),
                cent,
                Rounding::half_up);
        };
        while (true) {
            Decimal total_margin{};
            std::size_t largest = result.members.size();
            Decimal best_margin{};
            for (std::size_t i = 0; i < result.members.size(); ++i) {
                const auto m = lot_margin(result.members[i]);
                total_margin = total_margin + m;
                if (m > best_margin && result.members[i].quantity != Decimal{}) {
                    best_margin = m;
                    largest = i;
                }
            }
            if (total_margin <= margin_cap || largest == result.members.size())
                break;
            auto& target = result.members[largest];
            target.quantity =
                target.quantity > Decimal{} ? target.quantity - one : target.quantity + one;
            target.allocated_notional =
                target.quantity * (target.contract.price * target.contract.multiplier);
            target.nominal_remainder = target.requested_notional - target.allocated_notional;
        }
    }
    for (const auto& item : result.members) {
        result.allocated_gross_notional =
            result.allocated_gross_notional + magnitude(item.allocated_notional);
        result.allocated_net_notional = result.allocated_net_notional + item.allocated_notional;
    }
    result.unallocated_nominal_budget = multiply(equity, gross_weight_limit, Rounding::toward_zero)
        - result.requested_gross_notional;
    return result;
}

} // namespace asterion
