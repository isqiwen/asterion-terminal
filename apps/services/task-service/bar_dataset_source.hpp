#pragma once
#include <asterion/protocol/data.hpp>
#include <asterion/v1/research.pb.h>
#include <vector>
namespace asterion::tasks {
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
} // namespace asterion::tasks
