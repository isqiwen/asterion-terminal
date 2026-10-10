#pragma once
#include <asterion/domain/daily_bars.hpp>
#include <asterion/domain/market.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/v1/data.pb.h>
#include <span>
namespace asterion::protocol {
data::v1::DailyBar encode_daily_bar(const HistoricalDailyBar&);
HistoricalDailyBar daily_bar(const data::v1::DailyBar&);
Json decode_daily_page(const data::v1::DailyPage&);
data::v1::HistoryUpdateQuery encode_history_update_query(const Json&);
Json decode_history_update_query(const data::v1::HistoryUpdateQuery&);
Json decode_history_usage(const data::v1::HistoryUsage&);
Json decode_history_update_plan(const data::v1::HistoryUpdatePlan&);
Json decode_minute_page(const data::v1::MinutePage&);

// Evidence is fixed with task inputs, independently of bar content identity.
void validate_history_evidence(const data::v1::HistoryVersionEvidence&);
Json decode_history_evidence(const data::v1::HistoryVersionEvidence&);
data::v1::HistoryVersionEvidence encode_history_evidence(const Json&);

// Task datasets run in memory inside one task.
inline constexpr int max_dataset_sources = 32;
inline constexpr std::size_t max_dataset_bars = 200000;
Instrument instrument(const v1::Contract&);
v1::Bar encode_bar(const MarketBar&);
MarketBar market_bar(const v1::Bar&);
// Content identity: SHA-256 of contract, interval, bars and days.
std::string bar_dataset_revision(const data::v1::BarDataset&);
// Validates structure, order, contract units, day coverage and revision.
void validate_bar_dataset(const data::v1::BarDataset&);
std::vector<MarketBar> dataset_bars(const data::v1::BarDataset&);
std::vector<DaySettlement> dataset_days(const data::v1::BarDataset&);
// Both views validate the entire dataset; metadata omits bars and settlements.
Json decode_bar_dataset(const data::v1::BarDataset&, DatasetView view = DatasetView::full);
// What a dataset covers, instead of its bars: count, first and last bar, interval and sources.
Json decode_bar_dataset_range(const data::v1::BarDataset&);
data::v1::BarDataset encode_bar_dataset(const Json&);
data::v1::BarDatasetRequest encode_bar_dataset_request(const Json&);
Json decode_bar_dataset_request(const data::v1::BarDatasetRequest&);
// A dominant series over `contracts`, which its rolls name by index: later
// months of one product with the same terms, each from a later trading day,
// with positive factors that end in 1.
void validate_dominant_schedule(const data::v1::DominantSchedule& schedule,
                                std::span<const v1::Contract* const> contracts);
// The roll in force on a trading day; the first roll before the series begins.
const data::v1::DominantRoll& dominant_roll(const data::v1::DominantSchedule& schedule,
                                            const std::string& trading_day);
// A month's raw price at the level of the series' latest month, back on the
// price grid. Signals and factors read it; orders and fills never do.
Decimal dominant_price(Decimal raw, Decimal factor, Decimal increment);
Json decode_dominant_schedule(const data::v1::DominantSchedule& schedule);
std::string named_dataset_revision(const data::v1::NamedDataset&);
void validate_named_dataset(const data::v1::NamedDataset&);
Json decode_named_dataset(const data::v1::NamedDataset&);
} // namespace asterion::protocol
