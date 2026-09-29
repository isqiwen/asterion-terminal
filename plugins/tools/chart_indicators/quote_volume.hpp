#pragma once
#include <cstdint>
#include <optional>
#include <string_view>

namespace asterion::chart_indicators {
// Cumulative feed counters are comparable only inside one known trading day.
inline std::optional<std::int64_t> quote_volume(std::string_view previous_day,
                                                std::int64_t previous, std::string_view day,
                                                std::int64_t current) noexcept {
  if (day.empty() || previous_day != day || previous < 0 || current < previous)
    return std::nullopt;
  return current - previous;
}
} // namespace asterion::chart_indicators
