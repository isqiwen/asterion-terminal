#pragma once
#include <asterion/v1/task.pb.h>
#include <stop_token>
namespace asterion::protocol {
// Execution-thread operation; the control thread continues lease/cancel handling.
void resolve_task_input(task::v1::TaskAttempt&, std::stop_token);
} // namespace asterion::protocol
