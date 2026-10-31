#include "static_stress.hpp"
#include <asterion/base/error.hpp>
#include <algorithm>
#include <stdexcept>

namespace asterion {

StaticStress static_stress(
    Decimal balance,
    Decimal equity,
    Decimal frozen,
    const ExposurePosition& position,
    Decimal scenario_price,
    Decimal scenario_margin_rate,
    std::stop_token stop)
{
    if (stop.stop_requested())
        throw Error(ErrorCode::cancelled, "static stress cancelled");
    const auto magnitude = position.net_quantity < Decimal{} ? Decimal{} - position.net_quantity
                                                             : position.net_quantity;
    if (!position.gross_quantity || *position.gross_quantity < magnitude
        || position.multiplier <= Decimal{} || frozen < Decimal{}
        || scenario_margin_rate < Decimal{})
        throw std::invalid_argument(
            "static stress requires complete original lots and valid explicit assumptions");
    StaticStress result;
    result.mark_change =
        position.net_quantity * (scenario_price - position.price) * position.multiplier;
    result.equity = equity + result.mark_change;
    const auto absolute_price =
        scenario_price < Decimal{} ? Decimal{} - scenario_price : scenario_price;
    result.margin =
        *position.gross_quantity * absolute_price * position.multiplier * scenario_margin_rate;
    result.available = std::min(balance, result.equity) - result.margin - frozen;
    result.gap = result.available < Decimal{} ? Decimal{} - result.available : Decimal{};
    return result;
}

} // namespace asterion
