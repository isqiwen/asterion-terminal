#pragma once
#include "records.hpp"
#include <filesystem>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace asterion::warehouse {

enum class FeeBasis
{
    unknown,
    rate,
    lot
};

struct FeeRule
{
    std::string effective_from;
    std::string effective_until;
    FeeBasis basis = FeeBasis::unknown;
};

struct DceFeeRules
{
    FeeBasis basis(std::string_view product, std::string_view day) const;

    std::string verified_through;
    std::map<std::string, std::vector<FeeRule>, std::less<>> products;
};

struct ExchangeRules
{
    std::vector<PriceTick> price_ticks;
    DceFeeRules dce_settlement_fees;
};

ExchangeRules load_exchange_rules(const std::filesystem::path& file);

// Checks public observations of `day` against verified intervals, throwing
// std::invalid_argument on an unlisted product or a changed price tick.
void verify_price_ticks(
    std::span<const PriceTick> verified,
    std::span<const PriceTick> observed,
    std::string_view day);

} // namespace asterion::warehouse
