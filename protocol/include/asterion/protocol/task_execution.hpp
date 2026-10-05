#pragma once
#include <asterion/v1/task.pb.h>
namespace asterion::protocol {
// Called on the file worker after verifying immutable input bytes.
task::v1::TaskExecution task_execution(const task::v1::Task&, const std::string& input_sha256);
} // namespace asterion::protocol
