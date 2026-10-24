#pragma once
#include <asterion/domain/trading_days.hpp>
#include <asterion/domain/history_identity.hpp>
#include <asterion/domain/market.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/data.pb.h>
#include <asterion/data_service.pb.h>
#include <map>
#include <span>

namespace asterion::protocol {

Json decode_historical_margins(const data::proto::HistoricalMargins&);

data::proto::HistoryUpdateQuery encode_history_update_query(const Json&);
Json decode_history_update_query(const data::proto::HistoryUpdateQuery&);
Json decode_history_usage(const data::proto::HistoryUsage&);
Json decode_history_update_plan(const data::proto::HistoryUpdatePlan&);
Json decode_minute_page(const data::proto::MinutePage&);
void validate_minute_range_query(const data::proto::MinuteRangeQuery&);
data::proto::MinuteRangeQuery encode_minute_range_query(const Json&);
Json decode_minute_range_evidence(const data::proto::MinuteRangeEvidence&);
void validate_minute_evidence_query(const data::proto::MinuteEvidenceQuery&);
data::proto::MinuteEvidenceQuery encode_minute_evidence_query(const Json&);
Json decode_minute_evidence(const data::proto::MinuteEvidence&);
// Market-wide table updates: the definition both services keep, and what the
// Terminal shows of the store.
data::proto::TableRefreshQuery encode_table_refresh_query(const Json&);
Json decode_table_refresh_query(const data::proto::TableRefreshQuery&);
Json decode_table_refresh_plan(const data::proto::TableRefreshPlan&);
void validate_table_update(const data::proto::TableUpdate&);
data::proto::TableUpdate encode_table_update(const Json&);
Json decode_table_update(const data::proto::TableUpdate&);
// Verify completion evidence against the fixed target and execution identity, offline.
void validate_table_update_result(
    const data::proto::TableUpdate&,
    const data::proto::DownloadIdentity&,
    const data::proto::TableUpdateResult&);
Json decode_table_update_result(const data::proto::TableUpdateResult&);
Json decode_table_coverage(const data::proto::TableCoverage&);
data::proto::TablePageQuery encode_table_page_query(const Json&);
Json decode_table_page(const data::proto::TablePage&);

// Series of the market-wide daily table. An instrument is a contract
// (SHFE/rb/2026-10) or a product (SHFE/rb): its delivery month is then empty.
struct TableInstrument
{
    std::string venue, product, delivery_month;
};

// What one request may ask for: more than there are futures products.
inline constexpr int max_table_instruments = 100;
TableInstrument table_instrument(std::string_view text);
// Where daily facts and contract units are read: a snapshot of the table
// store and the sources of its daily table, calendar and contract list.
void validate_daily_table(const data::proto::DailyTable&);
data::proto::DailyTable encode_daily_table(const Json&);
Json decode_daily_table(const data::proto::DailyTable&);
void validate_table_series_request(const data::proto::TableSeriesRequest&);
data::proto::TableSeriesRequest encode_table_series_request(const Json&);
// Identity, ascending positive closes inside the range, and for a product a
// schedule that fits its months, each read only while it is dominant.
void validate_table_series(const data::proto::TableSeries&);
// The months of a series as contracts: exchange, product and delivery month.
std::vector<proto::Contract> table_series_months(const data::proto::TableSeries&);
// Content identity: SHA-256 of the instrument, its months' closes and schedule.
std::string table_series_revision(const data::proto::TableSeries&);
Json decode_table_series(const data::proto::TableSeries&);
data::proto::TableProductsQuery encode_table_products_query(const Json&);
Json decode_table_products(const data::proto::TableProducts&);
void validate_table_listed_contracts_query(const data::proto::TableListedContractsQuery&);
data::proto::TableListedContractsQuery encode_table_listed_contracts_query(const Json&);
Json decode_table_listed_contracts(const data::proto::TableListedContracts&);
// A chart of one contract of the daily table: its candles of a period, a page
// of them at a time.
void validate_table_daily_query(const data::proto::TableDailyQuery&);
data::proto::TableDailyQuery encode_table_daily_query(const Json&);
Json decode_table_daily(const data::proto::TableDailyPage&);

// Evidence is fixed with task inputs, independently of bar content identity.
void validate_history_evidence(const data::proto::HistoryVersionEvidence&);
Json decode_history_evidence(const data::proto::HistoryVersionEvidence&);
data::proto::HistoryVersionEvidence encode_history_evidence(const Json&);

// Task datasets run in memory inside one task.
inline constexpr int max_dataset_sources = 32;
inline constexpr std::size_t max_dataset_bars = 200000;
Instrument instrument(const proto::Contract&);
// The contract as history names it: SHFE/rb/2026-10.
HistoryIdentity history_identity(const proto::Contract&);
proto::Bar encode_bar(const MarketBar&);
MarketBar market_bar(const proto::Bar&);
// Content identity: SHA-256 of contract, interval, bars, days and price
// increment changes.
std::string bar_dataset_revision(const data::proto::BarDataset&);
// Validates structure, order, contract units, that every bar is on the price
// increment of its day and falls on one of the days, and the revision.
void validate_bar_dataset(const data::proto::BarDataset&);
// Exact UTC presentation of an original observation, preserving all nine nanos.
std::string format_utc_nanoseconds(std::int64_t);
std::vector<MarketBar> dataset_bars(const data::proto::BarDataset&);
// The days its price increment changes after the first bar's, ascending.
std::vector<PriceIncrementChange> dataset_price_increment_changes(const data::proto::BarDataset&);
std::vector<DaySettlement> dataset_days(const data::proto::BarDataset&);
// Both views validate the entire dataset; metadata omits bars and settlements.
Json decode_bar_dataset(const data::proto::BarDataset&, DatasetView view = DatasetView::full);
// What a dataset covers, instead of its bars: count, first and last bar, interval and sources.
Json decode_bar_dataset_range(const data::proto::BarDataset&);
data::proto::BarDataset encode_bar_dataset(const Json&);
data::proto::BarDatasetRequest encode_bar_dataset_request(const Json&);
Json decode_bar_dataset_request(const data::proto::BarDatasetRequest&);
// A product whose month contracts the daily table names: exchange and product
// as history writes them, and a range with both days.
void validate_product_bars_request(const data::proto::ProductBarsRequest&);
data::proto::ProductBarsRequest encode_product_bars_request(const Json&);
Json decode_product_bars_request(const data::proto::ProductBarsRequest&);
// A dominant series over `contracts`, which its rolls name by index: later
// months of one product, each from a later trading day, with positive
// factors that end in 1. Its term points, at most one a day
// from its first, compare positive settlements with a later delivery month.
void validate_dominant_schedule(
    const data::proto::DominantSchedule& schedule,
    std::span<const proto::Contract* const> contracts);
// The roll in force on a trading day; the first roll before the series begins.
const data::proto::DominantRoll&
dominant_roll(const data::proto::DominantSchedule& schedule, const std::string& trading_day);
// A month's raw price at the level of the series' latest month, back on the
// price grid. Signals and factors read it; orders and fills never do.
Decimal dominant_price(Decimal raw, Decimal factor, Decimal increment);
Json decode_dominant_schedule(const data::proto::DominantSchedule& schedule);
// How far `near_month` settled above the later month of a term point, as a
// year's worth: near over far less one, times twelve over the delivery
// months between them. Positive when the nearer month is the dearer.
Decimal term_carry(const data::proto::TermPoint& point, const std::string& near_month);
// The carry each trading day of a valid schedule begins with, where it has a
// term point; `contracts` are those its rolls name by index.
std::map<std::string, Decimal> term_carries(
    const data::proto::DominantSchedule& schedule,
    std::span<const proto::Contract* const> contracts);
Json decode_term_points(const data::proto::DominantSchedule& schedule);
std::string named_dataset_revision(const data::proto::NamedDataset&);
void validate_named_dataset(const data::proto::NamedDataset&);
Json decode_named_dataset(const data::proto::NamedDataset&);

} // namespace asterion::protocol
