#pragma once
#include <asterion/domain/daily_bars.hpp>
#include <asterion/domain/market.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/v1/data.pb.h>
#include <map>
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
// Market-wide table updates: the definition both services keep, and what the
// Terminal shows of the store.
void validate_table_update(const data::v1::TableUpdate&);
data::v1::TableUpdate encode_table_update(const Json&);
Json decode_table_update(const data::v1::TableUpdate&);
Json decode_table_update_result(const data::v1::TableUpdateResult&);
Json decode_table_coverage(const data::v1::TableCoverage&);
data::v1::TablePageQuery encode_table_page_query(const Json&);
Json decode_table_page(const data::v1::TablePage&);
// Series of the market-wide daily table. An instrument is a contract
// (SHFE/rb/2026-10) or a product (SHFE/rb): its delivery month is then empty.
struct TableInstrument {
  std::string venue, product, delivery_month;
};
// What one request may ask for: more than there are futures products.
inline constexpr int max_table_instruments = 100;
TableInstrument table_instrument(std::string_view text);
// Where daily facts are read: a snapshot of the table store and the sources
// of its daily table and calendar.
void validate_daily_table(const data::v1::DailyTable&);
data::v1::DailyTable encode_daily_table(const Json&);
Json decode_daily_table(const data::v1::DailyTable&);
void validate_table_series_request(const data::v1::TableSeriesRequest&);
data::v1::TableSeriesRequest encode_table_series_request(const Json&);
// Identity, ascending positive closes inside the range, and for a product a
// schedule that fits its months, each read only while it is dominant.
void validate_table_series(const data::v1::TableSeries&);
// The months of a series as contracts: exchange, product and delivery month.
std::vector<v1::Contract> table_series_months(const data::v1::TableSeries&);
// Content identity: SHA-256 of the instrument, its months' closes and schedule.
std::string table_series_revision(const data::v1::TableSeries&);
Json decode_table_series(const data::v1::TableSeries&);
data::v1::TableProductsQuery encode_table_products_query(const Json&);
Json decode_table_products(const data::v1::TableProducts&);

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
// Validates structure, order, contract units, that every bar falls on one of
// the days, and the revision.
inline constexpr unsigned bar_dataset_version = 4;
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
// A product whose month contracts the daily table names: exchange and product
// as history writes them, positive units, and a range with both days.
void validate_product_bars_request(const data::v1::ProductBarsRequest&);
data::v1::ProductBarsRequest encode_product_bars_request(const Json&);
Json decode_product_bars_request(const data::v1::ProductBarsRequest&);
// A dominant series over `contracts`, which its rolls name by index: later
// months of one product with the same terms, each from a later trading day,
// with positive factors that end in 1. Its term points, at most one a day
// from its first, compare positive settlements with a later delivery month.
void validate_dominant_schedule(const data::v1::DominantSchedule& schedule,
                                std::span<const v1::Contract* const> contracts);
// The roll in force on a trading day; the first roll before the series begins.
const data::v1::DominantRoll& dominant_roll(const data::v1::DominantSchedule& schedule,
                                            const std::string& trading_day);
// A month's raw price at the level of the series' latest month, back on the
// price grid. Signals and factors read it; orders and fills never do.
Decimal dominant_price(Decimal raw, Decimal factor, Decimal increment);
Json decode_dominant_schedule(const data::v1::DominantSchedule& schedule);
// How far `near_month` settled above the later month of a term point, as a
// year's worth: near over far less one, times twelve over the delivery
// months between them. Positive when the nearer month is the dearer.
Decimal term_carry(const data::v1::TermPoint& point, const std::string& near_month);
// The carry each trading day of a valid schedule begins with, where it has a
// term point; `contracts` are those its rolls name by index.
std::map<std::string, Decimal> term_carries(const data::v1::DominantSchedule& schedule,
                                            std::span<const v1::Contract* const> contracts);
Json decode_term_points(const data::v1::DominantSchedule& schedule);
std::string named_dataset_revision(const data::v1::NamedDataset&);
inline constexpr unsigned named_dataset_version = 3;
void validate_named_dataset(const data::v1::NamedDataset&);
Json decode_named_dataset(const data::v1::NamedDataset&);
} // namespace asterion::protocol
