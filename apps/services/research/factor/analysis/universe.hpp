#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace asterion {

// Aligned fixed daily observations. A missing close is a break in observed
// history, not proof of delisting. Lots stay raw across dominant-month rolls.
struct LiquidityObservation
{
    bool observed;
    std::optional<std::int64_t> volume_contracts, open_interest_contracts;
};

struct LiquidityRule
{
    std::size_t minimum_history;
    std::int64_t minimum_volume, minimum_open_interest;
};
enum class UniverseReason
{
    eligible,
    unobserved,
    insufficient_history,
    unknown_volume,
    unknown_open_interest,
    below_volume,
    below_open_interest
};
// Qualification is independent of features and future returns. At index i
// only closes before i count as history, and only i-1 supplies liquidity.
// Thresholds are inclusive. Unknown facts never become zero. Warm-up starts
// at the fixed input's first observation; no history outside it is assumed.
std::vector<UniverseReason>
liquidity_membership(std::span<const LiquidityObservation>, const LiquidityRule&);

} // namespace asterion
