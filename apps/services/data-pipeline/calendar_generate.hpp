#pragma once
#include "session_calendar.hpp"
#include <optional>
#include <string>
#include <string_view>
namespace asterion::data_pipeline {
// Renders a settlement calendar CSV (format: docs/market-data.md) from
// "trading_day,settlement_price,settlement_source" rows and a product's session
// template. The trading days and settlement prices are the caller's evidence;
// the template only supplies session times, recorded in schedule_source.
std::string generate_calendar_csv(const sessions::SessionCatalog& catalog, const std::string& venue,
                                  const std::string& product, std::string_view settlements,
                                  const std::optional<std::string>& previous = std::nullopt);
} // namespace asterion::data_pipeline
