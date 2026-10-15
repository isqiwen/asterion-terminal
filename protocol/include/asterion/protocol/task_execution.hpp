#pragma once
#include <asterion/v1/task.pb.h>

namespace asterion::protocol {

// Called on the file worker after verifying immutable input bytes.
task::v1::TaskExecution task_execution(const task::v1::Task&, const std::string& input_sha256);
// The request a resolved dataset is read again by: its versions and daily
// table over the trading days it speaks of, reported boundary gaps included.
// Data answers it with the same dataset, byte for byte.
data::v1::BarDatasetRequest bar_dataset_selection(const data::v1::BarDataset&);

} // namespace asterion::protocol
