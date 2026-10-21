#pragma once
#include <asterion/task.pb.h>
#include <functional>
#include <filesystem>
#include <stop_token>

namespace asterion::tasks {

using TaskProgress = std::function<void(std::size_t, std::size_t)>;
void resolve_task_input(task::proto::TaskAttempt&, std::stop_token);
task::proto::TaskFinish
download(const task::proto::TaskAttempt&, const std::string&, std::stop_token, const TaskProgress&);
task::proto::TaskFinish execute(
    task::proto::TaskAttempt,
    const std::filesystem::path&,
    const std::string&,
    std::stop_token,
    const TaskProgress&);

} // namespace asterion::tasks
