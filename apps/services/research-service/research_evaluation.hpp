#pragma once
#include "research_execution.hpp"
#include <asterion/research_evaluation.pb.h>

namespace asterion::tasks {

research::proto::EvaluationInput capture_evaluation_input(
    const resource::proto::Owner&,
    const research::proto::EvaluationRequest&,
    const task::proto::TaskResponse&,
    const std::string& input_digest,
    std::uint64_t input_bytes);
research::proto::EvaluationResult compute_evaluation(
    const research::proto::EvaluationInput&,
    const std::filesystem::path&,
    std::stop_token = {},
    const TaskProgress& = {});

} // namespace asterion::tasks
