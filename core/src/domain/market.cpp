#include <asterion/domain/market.hpp>

#include <algorithm>
#include <stdexcept>

namespace asterion {
namespace {
bool valid_identifier(const std::string& value) {
    return !value.empty() && value.size() <= 128 &&
        std::ranges::all_of(value, [](unsigned char c) {
            return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                   (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == '/';
        });
}
}
void InstrumentId::validate() const {
    if (!valid_identifier(venue) || !valid_identifier(symbol)) {
        throw std::invalid_argument("invalid instrument identity");
    }
}
void Instrument::validate() const {
    id.validate();
    switch (asset_class) {
    case AssetClass::equity: case AssetClass::futures: case AssetClass::option:
    case AssetClass::crypto: case AssetClass::fx: case AssetClass::bond: case AssetClass::commodity:
        break;
    default: throw std::invalid_argument("unsupported asset class");
    }
    if (!valid_identifier(quote_currency) || price_increment <= Decimal{} ||
        quantity_increment <= Decimal{} || multiplier <= Decimal{}) {
        throw std::invalid_argument("invalid instrument units");
    }
}
void TradeTick::validate(const Instrument& spec) const {
    spec.validate();
    if (instrument != spec.id || timestamp_ns < 0 || quantity <= Decimal{} ||
        !price.multiple_of(spec.price_increment) || !quantity.multiple_of(spec.quantity_increment)) {
        throw std::invalid_argument("trade does not match instrument contract");
    }
    // Zero or negative prices can be valid in some derivative markets.
}
} // namespace asterion
