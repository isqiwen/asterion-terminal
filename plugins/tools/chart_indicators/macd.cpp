#include "macd.hpp"
namespace asterion::chart_indicators {
std::optional<MacdValue> Macd::push(Decimal close) noexcept {
  const double value = static_cast<double>(close.raw()) / 100000000.0;
  if (count_++ == 0)
    fast_ = slow_ = value;
  else {
    fast_ += (value - fast_) * (2.0 / 13.0);
    slow_ += (value - slow_) * (2.0 / 27.0);
  }
  const double diff = fast_ - slow_;
  signal_ += (diff - signal_) * 0.2;
  if (count_ < 34)
    return std::nullopt;
  return MacdValue{diff, signal_, 2.0 * (diff - signal_)};
}
} // namespace asterion::chart_indicators
