#include "account_exposure.hpp"
#include <asterion/base/error.hpp>
#include <stdexcept>

namespace asterion {
namespace {

ExposureRatio
equity_ratio(std::optional<Decimal> amount, Decimal equity, ExposureUnavailable missing)
{
    if (!amount)
        return {{}, missing};
    if (equity <= Decimal{})
        return {{}, ExposureUnavailable::non_positive_equity};
    return {
        static_cast<double>(
            static_cast<long double>(amount->raw()) / static_cast<long double>(equity.raw())),
        ExposureUnavailable::none
    };
}

} // namespace

AccountExposure account_exposure(
    Decimal equity,
    std::optional<Decimal> observed_margin,
    std::span<const ExposurePosition> positions,
    std::stop_token stop)
{
    if (positions.empty())
        throw std::invalid_argument("exposure requires an original listed contract");
    if (observed_margin && *observed_margin < Decimal{})
        throw std::invalid_argument("exposure margin observation must be nonnegative");
    AccountExposure result;
    result.positions.reserve(positions.size());
    result.covered_gross_notional = Decimal{};
    const auto two = Decimal::parse("2");
    for (const auto& position : positions) {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "account exposure cancelled");
        if (position.multiplier <= Decimal{})
            throw std::invalid_argument(
                "exposure requires the original positive contract multiplier");
        if (position.margin && *position.margin < Decimal{})
            throw std::invalid_argument("exposure margin observation must be nonnegative");
        PositionExposure row;
        row.net_notional = position.net_quantity * position.price * position.multiplier;
        result.covered_net_notional = result.covered_net_notional + row.net_notional;
        if (position.gross_quantity) {
            const auto magnitude = position.net_quantity < Decimal{}
                ? Decimal{} - position.net_quantity
                : position.net_quantity;
            if (*position.gross_quantity < magnitude)
                throw std::invalid_argument(
                    "observed gross lots are smaller than absolute net lots");
            // Paired lots avoid an overflowing gross+net intermediate. Exact
            // division refuses invented half-quantum side quantities.
            const auto paired = divide(*position.gross_quantity - magnitude, two);
            row.long_quantity =
                paired + (position.net_quantity > Decimal{} ? position.net_quantity : Decimal{});
            row.short_quantity =
                paired + (position.net_quantity < Decimal{} ? magnitude : Decimal{});
            const auto price =
                position.price < Decimal{} ? Decimal{} - position.price : position.price;
            row.gross_notional = *position.gross_quantity * price * position.multiplier;
            if (result.covered_gross_notional)
                result.covered_gross_notional =
                    *result.covered_gross_notional + *row.gross_notional;
        } else
            result.covered_gross_notional.reset();
        row.net_to_equity = equity_ratio(row.net_notional, equity, ExposureUnavailable::none);
        row.gross_to_equity =
            equity_ratio(row.gross_notional, equity, ExposureUnavailable::gross_not_recorded);
        row.margin_to_equity =
            equity_ratio(position.margin, equity, ExposureUnavailable::margin_not_recorded);
        result.positions.push_back(std::move(row));
    }
    for (auto& position : result.positions) {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "account exposure cancelled");
        if (!result.covered_gross_notional)
            position.covered_gross_share = {{}, ExposureUnavailable::gross_not_recorded};
        else if (*result.covered_gross_notional == Decimal{})
            position.covered_gross_share = {{}, ExposureUnavailable::zero_gross_notional};
        else
            position.covered_gross_share = {
                static_cast<double>(
                    static_cast<long double>(position.gross_notional->raw())
                    / static_cast<long double>(result.covered_gross_notional->raw())),
                ExposureUnavailable::none
            };
    }
    result.net_to_equity =
        equity_ratio(result.covered_net_notional, equity, ExposureUnavailable::none);
    result.gross_to_equity = equity_ratio(
        result.covered_gross_notional,
        equity,
        ExposureUnavailable::gross_not_recorded);
    result.margin_to_equity =
        equity_ratio(observed_margin, equity, ExposureUnavailable::margin_not_recorded);
    return result;
}

} // namespace asterion
