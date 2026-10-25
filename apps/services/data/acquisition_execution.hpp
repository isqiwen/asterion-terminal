#pragma once
#include <asterion/task.pb.h>
#include <functional>
#include <stop_token>

namespace asterion::data {

using TaskProgress = std::function<void(std::size_t, std::size_t)>;
task::proto::TaskFinish
download(const task::proto::TaskAttempt&, const std::string&, std::stop_token, const TaskProgress&);

} // namespace asterion::data
