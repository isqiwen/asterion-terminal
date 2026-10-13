#pragma once
#include "history_tables.hpp"
#include <asterion/protocol/data.hpp>
#include <asterion/v1/backtest.pb.h>
#include <vector>
namespace asterion::data {
// Immutable archive records captured under the store lock. File verification
// runs outside it; task history is not needed to resolve these versions.
struct BarDatasetSources {
  data::v1::BarDatasetRequest request;
  std::vector<data::v1::HistoryRecord> sources;
};
// Minute bars come from archive versions: they carry their exchange trading
// day from the data source and need bar-end labels. Without archive versions
// the bars are the contract's days in the market-wide daily table: a day is a
// bar where the table has its open, high, low, close and volume, labelled
// 15:00 local on the trading day.
//
// The dataset's days are every trading day the contract has a settlement
// price on in the daily table, as the request's snapshot holds it, from the
// first day asked for (or of the bars) to the last: a day it did not trade
// on, or whose bars are missing, is a day without bars, and a position held
// over it is settled. Fails rather than drops bars without a settlement,
// rather than reads around a trading day the table lacks, and when the range
// exceeds max_dataset_bars.
//
// The contract's multiplier is the request's and nothing else of a dataset
// says it is right: it is checked against the daily table's turnover of the
// days read, and refused when fewer than half of the days that have one
// agree with it.
data::v1::BarDataset resolve_bar_dataset(const history_files::TableStore&,
                                         const BarDatasetSources&);
// Month contracts of one product as its dominant series. The dominant month
// of a trading day is the month with the largest open interest on the
// previous trading day, among the current month and later ones: it never
// moves back to an earlier month, and a change takes effect on the next
// trading day. The series therefore begins on the second common trading day.
struct DominantSeries {
  // The inputs that are dominant at some point, in delivery-month order.
  std::vector<std::size_t> months;
  // Each of those months over the days it is dominant; the trading day
  // before it becomes dominant, for an account to have a price of it by then;
  // and, once it is left, the days to its first bar from the roll day on, at
  // whose open what it holds is closed, and to its next after that, for what
  // that bar's volume did not take.
  std::vector<data::v1::BarDataset> datasets;
  // contract indexes refer to `months`. Its term points compare each day's
  // dominant month with the later month that had the most open interest on
  // the trading day before, at that day's settlements.
  data::v1::DominantSchedule schedule;
};
// Which month is dominant is the daily table's to say: by each month's
// settlements and open interest from the first day any of them has bars to
// the last, on days it has bars and days it has none. A month is left on the
// last day the table has a settlement price of it, whatever it holds; one
// whose download ends before it does is not left early for that, and stays
// dominant on days without bars.
// Fails rather than guesses: when no month can be dominant on a day, when
// the month that is left has no bars from the roll day on to be closed on, or
// when the two months lack a common settlement on the day before a roll (the
// adjustment ratio).
DominantSeries resolve_dominant_series(const history_files::TableStore&,
                                       const std::vector<BarDatasetSources>& months);
// A product's daily bars from the daily table as its dominant series: the
// months are those that have bars in the range, read once together; `months`
// index them in delivery order. Each dataset is what resolve_bar_dataset
// reads of that month over the same days.
DominantSeries resolve_product_series(const history_files::TableStore&,
                                      const data::v1::ProductBarsRequest&);
} // namespace asterion::data
