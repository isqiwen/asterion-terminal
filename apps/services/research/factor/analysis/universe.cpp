#include "universe.hpp"
#include <stdexcept>

namespace asterion {

std::vector<UniverseReason>
liquidity_membership(std::span<const LiquidityObservation> facts, const LiquidityRule& rule)
{
    if (!rule.minimum_history || rule.minimum_history > 10000 || rule.minimum_volume < 0
        || rule.minimum_open_interest < 0)
        throw std::invalid_argument("invalid liquidity eligibility rule");
    std::vector<UniverseReason> result;
    result.reserve(facts.size());
    std::size_t history = 0;
    for (std::size_t i = 0; i < facts.size(); ++i) {
        auto reason = UniverseReason::eligible;
        if (!facts[i].observed)
            reason = UniverseReason::unobserved;
        else if (history < rule.minimum_history)
            reason = UniverseReason::insufficient_history;
        else {
            const auto& prior = facts[i - 1];
            if (!prior.volume_contracts)
                reason = UniverseReason::unknown_volume;
            else if (!prior.open_interest_contracts)
                reason = UniverseReason::unknown_open_interest;
            else if (*prior.volume_contracts < rule.minimum_volume)
                reason = UniverseReason::below_volume;
            else if (*prior.open_interest_contracts < rule.minimum_open_interest)
                reason = UniverseReason::below_open_interest;
        }
        result.push_back(reason);
        history = facts[i].observed ? history + 1 : 0;
    }
    return result;
}

} // namespace asterion
