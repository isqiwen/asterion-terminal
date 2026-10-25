#pragma once
#include <asterion/research_model.pb.h>
#include <asterion/task.pb.h>
#include <asterion/backtest.pb.h>
#include <vector>
#include "research_execution.hpp"
#include <asterion/foundation/time.hpp>

namespace asterion::backtest {

struct ModelReplayData;

} // namespace asterion::backtest

namespace asterion::research {

backtest::ModelReplayData resolve_model_replay_data(
    const backtest::proto::BacktestInput&,
    const std::filesystem::path& root,
    const std::string& loaded_program,
    std::stop_token = {});
// Fixed plan and source evidence have been checked by the owning caller.
std::vector<std::uint32_t> model_time_training_indices(
    const proto::ModelTimePlan&,
    const factor::proto::FactorResult&,
    unsigned fold);

proto::ModelDecisionTrialResult compute_model_decision_trial(
    const proto::ModelDecisionTrialInput&,
    const std::filesystem::path& root,
    std::stop_token = {},
    const tasks::TaskProgress& = {},
    const Clock& = SystemClock{});

proto::ModelTimePlan capture_model_time_plan(
    const resource::proto::Owner&,
    const proto::ModelTimeSave&,
    const proto::ModelPlan&,
    const task::proto::TaskResponse&,
    std::int64_t recorded_at_ms,
    std::stop_token = {});
proto::ModelTimeResult validate_model_time(
    const proto::ModelTimeInput&,
    const std::filesystem::path& root,
    std::stop_token = {},
    const tasks::TaskProgress& = {},
    const Clock& = SystemClock{});

proto::ModelPlan capture_model_plan(
    const resource::proto::Owner&,
    const proto::ModelDraft&,
    const std::string& plan_id,
    const task::proto::TaskResponse&,
    const std::string& input_digest,
    std::uint64_t input_bytes,
    const std::string& current_program,
    std::int64_t recorded_at_ms);
std::vector<std::uint32_t> model_training_indices(
    const proto::ModelPlan&,
    const factor::proto::FactorInput&,
    const factor::proto::FactorResult&);

proto::ModelMappingProjection project_model_mapping(
    const proto::ModelMapping&,
    const task::proto::TaskResponse&,
    std::uint32_t begin,
    std::uint32_t limit,
    const Clock& = SystemClock{});

proto::ModelTrainingResult train_model(
    const proto::ModelTrainingInput&,
    const std::filesystem::path& root,
    std::stop_token = {},
    const tasks::TaskProgress& = {},
    const Clock& = SystemClock{});

} // namespace asterion::research
