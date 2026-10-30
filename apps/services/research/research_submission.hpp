#pragma once
#include "research_tasks.hpp"

namespace asterion::tasks {

// Research task submissions: each validates its own definition and ownership
// before the shared task store prepares the fixed input.
Store::Submission
task_submission(const Store& store, const std::string& id, backtest::proto::BacktestInput input);
Store::Submission
task_submission(const Store& store, const std::string& id, factor::proto::FactorInput input);
Store::Submission
task_submission(const Store& store, const std::string& id, research::proto::SampleInput input);
Store::Submission
task_submission(const Store& store, const std::string& id, research::proto::LogicTrialInput input);
Store::Submission
task_submission(const Store& store, const std::string& id, research::proto::EvaluationInput input);
Store::Submission
task_submission(const Store& store, const std::string& id, research::proto::StatisticsInput input);

// Admitted identity a calculation carries beside its definition, set before admission.
void set_program_artifact(Store::Submission&, std::string artifact);
void set_strategy_use(
    const Store&,
    Store::Submission&,
    research::proto::StrategyUse,
    const research::proto::StrategyDefinition&);

} // namespace asterion::tasks
