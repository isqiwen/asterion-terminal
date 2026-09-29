#pragma once
#include <asterion/foundation/decimal.hpp>
#include <cstddef>
#include <optional>

namespace asterion::chart_indicators {
struct MacdValue {
  double diff, signal, histogram;
};
// Display-only MACD(12,26,9). Price EMAs start at the dataset's first close;
// the signal starts at zero. Suppress the first 33 records as warm-up.
// Never reset at a query boundary. Histogram uses 2 * (DIFF - DEA).
class Macd final {
public:
  std::optional<MacdValue> push(Decimal close) noexcept;

private:
  std::size_t count_ = 0;
  double fast_ = 0, slow_ = 0, signal_ = 0;
};
} // namespace asterion::chart_indicators
