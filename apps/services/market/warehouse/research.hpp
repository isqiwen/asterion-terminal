#pragma once
#include "store.hpp"
#include <asterion/market.pb.h>

namespace asterion::market_data::warehouse {

// The one dataset research reads, provider "research", composed from the
// sources in the data directory: Tushare's contracts, calendars and daily
// bars; the exchanges' settlement parameters; each day's price ticks from the
// recorded CTP snapshot, or else the historical sources; and each contract-day's minutes
// with their observation segments from Tushare, or from the recorder when
// Tushare has none. Every partition comes whole from one source. Unchanged
// sources publish no new version. `research` is the one Store of that dataset.
Version compose_research(const std::filesystem::path& data, Store& research);

// The contract-days whose minutes Tushare and the recorder both hold,
// compared minute by minute on open, high, low, close and volume. A recorded
// minute marked partial is counted, not compared.
::market::MinuteComparison compare_minutes(const std::filesystem::path& data);
::market::MinuteDifferences
minute_differences(const std::filesystem::path& data, const ::market::GetMinuteDifferences&);

} // namespace asterion::market_data::warehouse
