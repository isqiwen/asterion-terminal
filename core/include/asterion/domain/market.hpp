#pragma once

#include <asterion/foundation/decimal.hpp>

#include <cstdint>
#include <string>

namespace asterion {

enum class AssetClass { equity, futures, option, crypto, fx, bond, commodity };

// Venue + symbol identifies an actual tradable instrument, never a continuous alias.
struct InstrumentId {
  std::string venue;
  std::string symbol;
  auto operator<=>(const InstrumentId&) const = default;
  void validate() const;
};

struct Instrument {
  InstrumentId id;
  AssetClass asset_class;
  std::string quote_currency;
  Decimal price_increment;
  Decimal quantity_increment;
  Decimal multiplier;
  void validate() const;
};

struct TradeTick {
  InstrumentId instrument;
  std::int64_t timestamp_ns;
  Decimal price;
  Decimal quantity;
  void validate(const Instrument& spec) const;
};

} // namespace asterion
