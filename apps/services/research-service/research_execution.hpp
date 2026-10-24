#pragma once
#include <asterion/task.pb.h>
#include <functional>
#include <filesystem>
#include <stop_token>
#include <chrono>

namespace asterion::tasks {

using TaskProgress = std::function<void(std::size_t, std::size_t)>;
void resolve_task_input(
    task::proto::TaskAttempt&,
    std::stop_token,
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max());
task::proto::TaskFinish execute(
    task::proto::TaskAttempt,
    const std::filesystem::path&,
    std::stop_token,
    const TaskProgress&,
    const std::string& loaded_program = {});

} // namespace asterion::tasks
