#pragma once
#include <asterion/base/decimal.hpp>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>

namespace asterion {

enum class ExposureUnavailable
{
    none,
    gross_not_recorded,
    margin_not_recorded,
    non_positive_equity,
    zero_gross_notional
};

struct ExposureRatio
{
    std::optional<double> value;
    ExposureUnavailable unavailable = ExposureUnavailable::none;
};

struct ExposurePosition
{
    Decimal price, multiplier, net_quantity;
    std::optional<Decimal> gross_quantity, margin;
};

struct PositionExposure
{
    Decimal net_notional;
    std::optional<Decimal> gross_notional, long_quantity, short_quantity;
    ExposureRatio net_to_equity, gross_to_equity, margin_to_equity, covered_gross_share;
};

struct AccountExposure
{
    std::vector<PositionExposure> positions;
    Decimal covered_net_notional;
    std::optional<Decimal> covered_gross_notional;
    ExposureRatio net_to_equity, gross_to_equity, margin_to_equity;
};

// Caller supplies one currency and the original listed settlement contracts.
// Observed margin is an amount, never a rate. Available/frozen funds remain
// original ledger facts outside this calculation; they are not reconstructed.
AccountExposure account_exposure(
    Decimal equity,
    std::optional<Decimal> observed_margin,
    std::span<const ExposurePosition>,
    std::stop_token = {});

} // namespace asterion
