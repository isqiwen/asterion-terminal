#pragma once
#include <asterion/task.pb.h>
#include <stop_token>

namespace asterion::protocol {

// Execution-thread operation; the control thread continues lease/cancel handling.
void resolve_task_input(task::proto::TaskAttempt&, std::stop_token);

} // namespace asterion::protocol
