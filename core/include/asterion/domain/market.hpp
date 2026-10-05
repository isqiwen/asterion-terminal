#pragma once

#include <asterion/foundation/decimal.hpp>

#include <cstdint>
#include <string>

namespace asterion {

// Venue + symbol identifies an actual tradable instrument, never a continuous alias.
struct InstrumentId {
  std::string venue;
  std::string symbol;
  auto operator<=>(const InstrumentId&) const = default;
  void validate() const;
};

struct Instrument {
  InstrumentId id;
  std::string quote_currency;
  Decimal price_increment;
  Decimal quantity_increment;
  Decimal multiplier;
  void validate() const;
};

// One OHLCV period of a single contract from a historical data source.
// trading_day (YYYY-MM-DD) is the provider's or the dataset's derived trading
// date; timestamp_ns is the source bar label. Neither implies an intrabar path.
struct MarketBar {
  std::string trading_day;
  std::int64_t timestamp_ns = 0;
  Decimal open, high, low, close, volume;
  void validate(const Instrument& spec) const;
};

// Provider settlement price of one trading day.
struct DaySettlement {
  std::string trading_day;
  Decimal settlement_price;
};

} // namespace asterion
