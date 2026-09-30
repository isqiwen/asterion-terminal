#pragma once
#include <asterion/protocol/data.hpp>
#include <asterion/v1/research.pb.h>
#include <optional>
namespace asterion::tasks {
// Completed downloads captured under the store lock; resolve() reads their
// files outside it. The settlement source is a daily download of the same
// contract whose trading days and settlement prices settle every bar.
struct BarDatasetSources {
  data::v1::BarDatasetRequest request;
  research::v1::Task source;
  std::optional<data::v1::MinuteDownloadResult> minutes;
  std::optional<data::v1::DailyDownloadResult> daily;
  research::v1::Task settlement;
  data::v1::DailyDownloadResult settlement_result;
};
// Minute bars carry their exchange trading day from the data source and need
// bar-end labels. Daily bars are labelled 15:00 local on their trading day.
// Fails rather than drops bars without a settlement, and when the range exceeds
// max_dataset_bars. Settlement days without minute bars are listed as uncovered.
data::v1::BarDataset resolve_bar_dataset(const BarDatasetSources&);
} // namespace asterion::tasks
