#pragma once
#include "store.hpp"

namespace asterion::market_data::warehouse {

// The one dataset research reads, provider "research", composed from the
// sources in the data directory: Tushare's contracts, calendars and daily
// bars; the exchanges' settlement parameters; and each contract-day's minutes
// with their observation segments from Tushare, or from the recorder when
// Tushare has none. Every partition comes whole from one source. Unchanged
// sources publish no new version.
Version compose_research(const std::filesystem::path& data);

} // namespace asterion::market_data::warehouse
