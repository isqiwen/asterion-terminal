#pragma once
#include <asterion/domain/daily_bars.hpp>
#include <asterion/domain/market.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/v1/data.pb.h>
namespace asterion::protocol {
data::v1::DailyBar encode_daily_bar(const HistoricalDailyBar&);
HistoricalDailyBar daily_bar(const data::v1::DailyBar&);
Json decode_daily_page(const data::v1::DailyPage&);
Json decode_minute_page(const data::v1::MinutePage&);

inline constexpr std::size_t max_dataset_bars = 20000;
Instrument instrument(const v1::Contract&);
v1::Bar encode_bar(const MarketBar&);
MarketBar market_bar(const v1::Bar&);
// Content identity: SHA-256 of contract, interval, bars and days.
std::string bar_dataset_revision(const data::v1::BarDataset&);
// Validates structure, order, contract units, day coverage and revision.
void validate_bar_dataset(const data::v1::BarDataset&);
std::vector<MarketBar> dataset_bars(const data::v1::BarDataset&);
std::vector<DaySettlement> dataset_days(const data::v1::BarDataset&);
Json decode_bar_dataset(const data::v1::BarDataset&);
data::v1::BarDataset encode_bar_dataset(const Json&);
data::v1::BarDatasetRequest encode_bar_dataset_request(const Json&);
Json decode_bar_dataset_request(const data::v1::BarDatasetRequest&);
} // namespace asterion::protocol
