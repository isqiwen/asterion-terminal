#pragma once
#include <asterion/protocol/backtest.hpp>
#include <asterion/v1/task.pb.h>
#include <chrono>
namespace asterion::protocol {
// Result file verification has a separate budget from request admission and worker
// liveness. The worker continues to renew its lease while awaiting completion.
inline constexpr auto task_verification_timeout = std::chrono::minutes(5);
// Preparing a bounded immutable input is separate from the small progress RPC.
// Its lease starts only after input preparation has finished.
inline constexpr auto task_input_timeout = std::chrono::seconds(30);
Json decode_task(const task::v1::Task& task);
Json decode_task_result(const task::v1::TaskResponse& response, const std::string& id);
} // namespace asterion::protocol
