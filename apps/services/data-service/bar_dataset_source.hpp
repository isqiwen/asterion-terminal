#pragma once
#include <asterion/protocol/data.hpp>
#include <asterion/v1/backtest.pb.h>
#include <vector>
namespace asterion::data {
// Immutable archive records captured under the store lock. File verification
// runs outside it; task history is not needed to resolve these versions.
struct BarDatasetSources {
  data::v1::BarDatasetRequest request;
  std::vector<data::v1::HistoryRecord> sources;
  std::vector<data::v1::HistoryRecord> settlements;
};
// Minute bars carry their exchange trading day from the data source and need
// bar-end labels. Daily bars are labelled 15:00 local on their trading day.
// Fails rather than drops bars without a settlement, and when the range exceeds
// max_dataset_bars. Settlement days without minute bars are listed as uncovered.
data::v1::BarDataset resolve_bar_dataset(const BarDatasetSources&);
// Month contracts of one product as its dominant series. The dominant month
// of a trading day is the month with the largest open interest on the
// previous trading day, among the current month and later ones: it never
// moves back to an earlier month, and a change takes effect on the next
// trading day. The series therefore begins on the second common trading day.
struct DominantSeries {
  // The inputs that are dominant at some point, in delivery-month order.
  std::vector<std::size_t> months;
  // Bars of each of those months while it is dominant, plus the roll day and
  // the trading day after it, on which the position it still holds is closed.
  std::vector<data::v1::BarDataset> datasets;
  // contract indexes refer to `months`.
  data::v1::DominantSchedule schedule;
};
// Fails rather than guesses: when no month has data on a day, when the
// outgoing month has no bars on the roll day to close on, or when the two
// months lack a common settlement on the day before a roll (the adjustment
// ratio).
DominantSeries resolve_dominant_series(const std::vector<BarDatasetSources>& months);
} // namespace asterion::data
