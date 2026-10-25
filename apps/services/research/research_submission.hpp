#pragma once
#include "task_store.hpp"

namespace asterion::tasks {

// Research task submissions: each validates its own definition and ownership
// before the shared task store prepares the fixed input.
Store::Submission
task_submission(const Store& store, const std::string& id, backtest::proto::BacktestInput input);
Store::Submission
task_submission(const Store& store, const std::string& id, factor::proto::FactorInput input);
Store::Submission task_submission(
    const Store& store,
    const std::string& id,
    research::proto::AccountReviewInput input);
Store::Submission
task_submission(const Store& store, const std::string& id, research::proto::SampleInput input);
Store::Submission
task_submission(const Store& store, const std::string& id, research::proto::LogicTrialInput input);
Store::Submission
task_submission(const Store& store, const std::string& id, research::proto::EvaluationInput input);
Store::Submission
task_submission(const Store& store, const std::string& id, research::proto::StatisticsInput input);

} // namespace asterion::tasks
