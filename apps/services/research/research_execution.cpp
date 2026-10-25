#include "research_execution.hpp"
#include "research_statistics.hpp"
#include "research_evaluation.hpp"
#include "research_sample.hpp"
#include "research_logic.hpp"
#include "backtest/engine.hpp"
#include "factor/factor_engine.hpp"
#include "risk_module.hpp"
#include "coverage.hpp"
#include <asterion/protocol/task_execution.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/protocol/account_review.hpp>
#include <asterion/foundation/error.hpp>

namespace asterion::tasks {

task::proto::TaskFinish execute(
    task::proto::TaskAttempt attempt,
    const std::filesystem::path& directory,
    std::stop_token stop,
    const TaskProgress& progress,
    const std::string& loaded_program)
{
    if ((attempt.task().kind() == task::proto::BACKTEST
            || attempt.task().kind() == task::proto::FACTOR)
        && !attempt.task().program_artifact().empty()
        && attempt.task().program_artifact() != loaded_program)
        throw Error(ErrorCode::conflict, "calculation requires its fixed execution program");
    resolve_task_input(attempt, stop);
    task::proto::TaskFinish result;
    switch (attempt.task().kind()) {
        case task::proto::BACKTEST: {
            const auto module = risk_providers::Module::pinned(
                directory / attempt.task().id(),
                attempt.task().risk_artifact());
            *result.mutable_result() =
                backtest::run(attempt.task().input(), stop, progress, &module);
            break;
        }
        case task::proto::FACTOR:
            *result.mutable_factor() = factor::run(attempt.task().factor(), stop, progress);
            break;
        case task::proto::LOGIC_TRIAL:
            if (loaded_program.empty() || loaded_program != attempt.task().program_artifact())
                throw Error(
                    ErrorCode::conflict,
                    "calculation requires its fixed execution program");
            *result.mutable_logic_trial() =
                compute_logic_trial(attempt.task().logic_trial(), stop, progress);
            break;
        case task::proto::SAMPLE:
            if (loaded_program.empty() || loaded_program != attempt.task().program_artifact())
                throw Error(
                    ErrorCode::conflict,
                    "calculation requires its fixed execution program");
            *result.mutable_sample() = capture_sample(attempt.task().sample(), stop, progress);
            break;
        case task::proto::PERFORMANCE_EVALUATION:
            if (loaded_program.empty() || loaded_program != attempt.task().program_artifact())
                throw Error(
                    ErrorCode::conflict,
                    "calculation requires its fixed execution program");
            *result.mutable_evaluation() =
                compute_evaluation(attempt.task().evaluation(), directory, stop, progress);
            break;
        case task::proto::STATISTICS:
            if (loaded_program.empty() || loaded_program != attempt.task().program_artifact())
                throw Error(
                    ErrorCode::conflict,
                    "calculation requires its fixed execution program");
            *result.mutable_statistics() =
                compute_statistics(attempt.task().statistics(), directory, stop, progress);
            break;
        case task::proto::ACCOUNT_REVIEW: {
            const auto& input = attempt.task().account_review();
            if (loaded_program.empty() || loaded_program != input.program_artifact())
                throw Error(
                    ErrorCode::conflict,
                    "account review requires its fixed execution program");
            const auto& facts = input.evidence().capture().facts();
            AccountObservationCoverage coverage;
            coverage.position_records = static_cast<unsigned>(facts.positions_size());
            coverage.order_records = static_cast<unsigned>(facts.orders_size());
            coverage.trade_records = static_cast<unsigned>(facts.trades_size());
            coverage.unconfirmed_orders = static_cast<unsigned>(facts.unconfirmed_size());
            coverage.broker_ready = facts.phase() == "ready";
            coverage.positions_reconciled = facts.positions_reconciled();
            coverage.funds_present = facts.has_funds();
            coverage.recovery_required = facts.recovery_required();
            const auto reviewed = review_account_observation(coverage, stop);
            auto& output = *result.mutable_account_review();
            *output.mutable_owner() = input.owner();
            *output.mutable_source() = input.evidence().operation().reference();
            output.set_account_id(facts.account_id());
            output.set_environment(facts.environment());
            output.set_program_artifact(input.program_artifact());
            output.set_conclusion(research::proto::AccountReviewResult::EVIDENCE_INSUFFICIENT);
            output.set_position_records(coverage.position_records);
            output.set_order_records(coverage.order_records);
            output.set_trade_records(coverage.trade_records);
            output.set_unconfirmed_orders(coverage.unconfirmed_orders);
            output.set_broker_ready(coverage.broker_ready);
            output.set_positions_reconciled(coverage.positions_reconciled);
            output.set_funds_present(coverage.funds_present);
            output.set_recovery_required(coverage.recovery_required);
            for (const auto& limitation : reviewed.limitations)
                output.add_limitations(limitation);
            if (stop.stop_requested())
                throw Error(ErrorCode::cancelled, "task cancelled");
            progress(1, 1);
            break;
        }
        default:
            throw std::invalid_argument("unsupported task kind");
    }
    result.set_id(attempt.task().id());
    result.set_token(attempt.token());
    return result;
}

} // namespace asterion::tasks
