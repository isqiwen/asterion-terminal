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
// Each trading day's settlement price is the contract's in the daily table,
// as the request's snapshot holds it, read from the first day asked for (or
// of the bars) to the last. Fails rather than drops bars without a
// settlement, rather than reads around a trading day the table lacks, and
// when the range exceeds max_dataset_bars. Days the contract settled on
// without bars are listed as uncovered.
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
  // Bars of each of those months while it is dominant; the trading day before
  // it becomes dominant, whose last bar is the price it takes the series over
  // at; and, once it is left, the days the position it still holds is closed
  // on: the roll day and the trading day after it of minute bars, every day it
  // still trades of daily bars. A day without bars is not added.
  std::vector<data::v1::BarDataset> datasets;
  // contract indexes refer to `months`. Its term points compare each day's
  // dominant month with the later month that had the most open interest on
  // the trading day before, at that day's settlements.
  data::v1::DominantSchedule schedule;
};
// A month has data on the days it has bars; its open interest is the daily
// table's, on every day any of the months has bars.
// Fails rather than guesses: when no month has data on a day, when the
// outgoing month has no bars on the roll day to close on, or when the two
// months lack a common settlement on the day before a roll (the adjustment
// ratio).
DominantSeries resolve_dominant_series(const history_files::TableStore&,
                                       const std::vector<BarDatasetSources>& months);
// A product's daily bars from the daily table as its dominant series: the
// months are those that traded in the range, read once together; `months`
// index them in delivery order. Each dataset is what resolve_bar_dataset
// reads of that month over the same days. Unlike a month of minute bars, a
// month that is left keeps its bars to the last day it traded in the range.
DominantSeries resolve_product_series(const history_files::TableStore&,
                                      const data::v1::ProductBarsRequest&);
} // namespace asterion::data
