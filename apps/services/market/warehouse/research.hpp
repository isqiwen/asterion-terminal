#pragma once
#include "store.hpp"

namespace asterion::market_data::warehouse {

// The one dataset clients read, provider "research", composed from the sources
// in the data directory: Tushare's contracts, calendars, daily bars and minutes;
// the exchanges' settlement parameters; and each day's price ticks from the CTP
// snapshot, or else the historical sources. Every partition comes whole from one
// source. Unchanged sources publish no new version.
Version compose_research(const std::filesystem::path& data, Store& research);

} // namespace asterion::market_data::warehouse
