#pragma once
#include "account_exposure.hpp"

namespace asterion {

struct StaticStress
{
    Decimal mark_change, equity, margin, available, gap;
};

// Fixed holdings, balance and frozen funds. This is the original paper
// account's min(balance,equity)-margin-frozen model, not a broker rule.
StaticStress static_stress(
    Decimal balance,
    Decimal equity,
    Decimal frozen,
    const ExposurePosition&,
    Decimal scenario_price,
    Decimal scenario_margin_rate,
    std::stop_token = {});

} // namespace asterion
