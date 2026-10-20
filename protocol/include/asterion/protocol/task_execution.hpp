#pragma once
#include <asterion/task.pb.h>

namespace asterion::protocol {

// Called on the file worker after verifying immutable input bytes.
task::proto::TaskExecution
task_execution(const task::proto::Task&, const std::string& input_sha256);
// The request a resolved dataset is read again by: its versions and daily
// table over the trading days it speaks of, reported boundary gaps included.
// Data answers it with the same dataset, byte for byte.
data::proto::BarDatasetRequest bar_dataset_selection(const data::proto::BarDataset&);

} // namespace asterion::protocol
