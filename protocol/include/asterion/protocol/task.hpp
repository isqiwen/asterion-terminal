#pragma once
#include <asterion/protocol/backtest.hpp>
#include <asterion/task.pb.h>
#include <chrono>
#include <string_view>

namespace asterion::protocol {

// Result file verification has a separate budget from request admission and worker
// liveness. The worker continues to renew its lease while awaiting completion.
inline constexpr auto task_verification_timeout = std::chrono::minutes(5);
// Preparing a bounded immutable input is separate from the small progress RPC.
// Its lease starts only after input preparation has finished.
inline constexpr auto task_input_timeout = std::chrono::seconds(30);
Json decode_task(const task::proto::Task& task);
Json decode_task_retirement(
    const task::proto::TaskRetirement&,
    const std::string& id,
    const std::string& service,
    std::string_view domain);
Json decode_task_page(const task::proto::TaskList&, unsigned before);
Json decode_task_result(const task::proto::TaskResponse& response, const std::string& id);

} // namespace asterion::protocol
