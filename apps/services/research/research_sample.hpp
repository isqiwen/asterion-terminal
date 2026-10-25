#pragma once
#include "research_execution.hpp"
#include <asterion/research_sample.pb.h>

namespace asterion::tasks {

research::proto::SampleResult
capture_sample(const research::proto::SampleInput&, std::stop_token = {}, const TaskProgress& = {});

} // namespace asterion::tasks
