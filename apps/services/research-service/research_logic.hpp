#pragma once
#include <asterion/research_logic.pb.h>
#include <functional>
#include <stop_token>

namespace asterion::tasks {

research::proto::LogicTrialResult compute_logic_trial(
    const research::proto::LogicTrialInput&,
    std::stop_token = {},
    const std::function<void(unsigned, unsigned)>& progress = {});

} // namespace asterion::tasks
