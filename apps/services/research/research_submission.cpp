#include "research_submission.hpp"
#include "risk_module.hpp"
#include <asterion/foundation/error.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/protocol/account_review.hpp>
#include <asterion/protocol/research_evaluation.hpp>
#include <asterion/protocol/research_logic.hpp>
#include <asterion/protocol/research_statistics.hpp>

namespace asterion::tasks {
namespace wire = task::proto;

Store::Submission
task_submission(const Store& store, const std::string& id, backtest::proto::BacktestInput input)
{
    wire::Task task;
    task.set_id(id);
    *task.mutable_input() = std::move(input);
    task.set_risk_artifact(risk_providers::Module::selected().artifact());
    return store.submission(std::move(task));
}

Store::Submission
task_submission(const Store& store, const std::string& id, factor::proto::FactorInput input)
{
    wire::Task task;
    task.set_id(id);
    *task.mutable_factor() = std::move(input);
    return store.submission(std::move(task));
}

Store::Submission task_submission(
    const Store& store,
    const std::string& id,
    research::proto::AccountReviewInput input)
{
    protocol::validate_account_review_input(input);
    if (input.owner().service_id() != store.instance())
        throw Error(ErrorCode::conflict, "account review belongs to another Research owner");
    wire::Task task;
    task.set_id(id);
    *task.mutable_account_review() = std::move(input);
    auto submission = store.submission(std::move(task));
    submission.set_reuse_digest(
        sha256_bytes(submission.task().account_review().SerializeAsString()));
    return submission;
}

Store::Submission
task_submission(const Store& store, const std::string& id, research::proto::SampleInput input)
{
    if (input.owner().service_id() != store.instance())
        throw Error(ErrorCode::conflict, "invalid research sample owner");
    wire::Task task;
    task.set_id(id);
    *task.mutable_sample() = std::move(input);
    return store.submission(std::move(task));
}

Store::Submission
task_submission(const Store& store, const std::string& id, research::proto::LogicTrialInput input)
{
    protocol::validate_logic_trial_input(input);
    if (input.owner().service_id() != store.instance())
        throw Error(ErrorCode::conflict, "invalid research logic trial owner");
    wire::Task task;
    task.set_id(id);
    *task.mutable_logic_trial() = std::move(input);
    return store.submission(std::move(task));
}

Store::Submission
task_submission(const Store& store, const std::string& id, research::proto::EvaluationInput input)
{
    protocol::validate_evaluation_input(input);
    if (input.request().source().owner().service_id() != store.instance())
        throw Error(ErrorCode::conflict, "evaluation source must belong to this Research service");
    wire::Task task;
    task.set_id(id);
    *task.mutable_evaluation() = std::move(input);
    auto submission = store.submission(std::move(task));
    submission.set_reuse_digest(sha256_bytes(submission.task().evaluation().SerializeAsString()));
    wire::TaskDerivation origin;
    origin.set_source_id(submission.task().evaluation().request().source().id());
    origin.set_source_result_digest(submission.task().evaluation().request().source().version());
    origin.set_source_instance(store.instance());
    origin.set_data_choice("fixed");
    submission.set_derivation(std::move(origin));
    return submission;
}

Store::Submission
task_submission(const Store& store, const std::string& id, research::proto::StatisticsInput input)
{
    protocol::validate_statistics_input(input);
    if (input.owner().service_id() != store.instance())
        throw Error(ErrorCode::conflict, "statistics source must belong to this Research service");
    wire::Task task;
    task.set_id(id);
    *task.mutable_statistics() = std::move(input);
    auto submission = store.submission(std::move(task));
    submission.set_reuse_digest(sha256_bytes(submission.task().statistics().SerializeAsString()));
    wire::TaskDerivation origin;
    origin.set_source_id(submission.task().statistics().source().result().id());
    origin.set_source_result_digest(submission.task().statistics().source().result().version());
    origin.set_source_instance(store.instance());
    origin.set_data_choice("fixed");
    submission.set_derivation(std::move(origin));
    return submission;
}

} // namespace asterion::tasks
