#include "research_execution.hpp"
#include "research_statistics.hpp"
#include "research_evaluation.hpp"
#include "research_sample.hpp"
#include "research_logic.hpp"
#include "research_models.hpp"
#include "backtest/engine.hpp"
#include "backtest/model_strategy.hpp"
#include "factor/factor_engine.hpp"
#include "risk_module.hpp"
#include "coverage.hpp"
#include "availability.hpp"
#include <asterion/protocol/notice_review.hpp>
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
            std::optional<backtest::ModelReplayData> model;
            if (attempt.task().input().has_model_replay())
                model = research::resolve_model_replay_data(
                    attempt.task().input(),
                    directory,
                    loaded_program,
                    stop);
            *result.mutable_result() = backtest::run(
                attempt.task().input(),
                stop,
                progress,
                &module,
                model ? &*model : nullptr);
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
        case task::proto::MODEL_TRAINING:
            if (loaded_program.empty() || loaded_program != attempt.task().program_artifact())
                throw Error(
                    ErrorCode::conflict,
                    "calculation requires its fixed execution program");
            *result.mutable_model_training() =
                research::train_model(attempt.task().model_training(), directory, stop, progress);
            break;
        case task::proto::MODEL_TIME_VALIDATION:
            if (loaded_program.empty() || loaded_program != attempt.task().program_artifact())
                throw Error(
                    ErrorCode::conflict,
                    "calculation requires its fixed execution program");
            *result.mutable_model_time() = research::validate_model_time(
                attempt.task().model_time(),
                directory,
                stop,
                progress);
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
        case task::proto::EVENT_SET_REVIEW: {
            const auto& input = attempt.task().event_set_review();
            if (loaded_program.empty() || loaded_program != input.program_artifact())
                throw Error(
                    ErrorCode::conflict,
                    "event set review requires its fixed execution program");
            auto& output = *result.mutable_event_set_review();
            *output.mutable_owner() = input.owner();
            *output.mutable_source() = input.request().source();
            *output.mutable_content() = input.content().receipt().content();
            output.set_program_artifact(input.program_artifact());
            const auto& payload = input.content().payload();
            output.set_as_of_ms(payload.request().query().as_of_ms());
            output.set_use_mode(payload.request().query().use_mode());
            output.set_use_scope(input.request().use_scope());
            output.set_assembled_at_ms(input.publication().published().available_at_ms());
            *output.mutable_exclusions() = payload.exclusions();
            unsigned completed = 0;
            for (const auto& member : input.members()) {
                if (stop.stop_requested())
                    throw Error(ErrorCode::cancelled, "task cancelled");
                task::proto::TaskAttempt single;
                single.mutable_task()->set_kind(task::proto::NOTICE_REVIEW);
                *single.mutable_task()->mutable_notice_review() = member;
                *single.mutable_execution() = protocol::task_execution(
                    single.task(),
                    sha256_bytes(member.SerializeAsString()));
                const auto report = execute(
                    std::move(single),
                    directory,
                    stop,
                    [](unsigned, unsigned) {},
                    loaded_program);
                *output.add_members() = report.notice_review();
                progress(++completed, input.members_size());
            }
            if (stop.stop_requested())
                throw Error(ErrorCode::cancelled, "task cancelled");
            protocol::validate_event_set_review_result(input, output);
            break;
        }
        case task::proto::NOTICE_REVIEW: {
            const auto& input = attempt.task().notice_review();
            if (loaded_program.empty() || loaded_program != input.program_artifact())
                throw Error(
                    ErrorCode::conflict,
                    "notice review requires its fixed execution program");
            const auto& payload = input.content().payload();
            const auto& original = payload.input().material().evidence();
            const auto& publication = input.publication().publication();
            NoticeAvailability times;
            if (original.has_source_published_at_ms())
                times.source_published_at_ms = original.source_published_at_ms();
            times.acquired_at_ms = original.acquired_at_ms();
            times.processed_at_ms = payload.processed_at_ms();
            times.verified_at_ms = payload.verification().verified_at_ms();
            times.stored_at_ms = publication.storage().stored_at_ms();
            times.available_at_ms = publication.available_at_ms();
            times.as_of_ms = input.request().as_of_ms();
            times.declared_strict_history =
                payload.input().candidate().use_mode() == intelligence::proto::STRICT_HISTORY;
            const auto reviewed = review_notice_availability(times, stop);
            auto& output = *result.mutable_notice_review();
            *output.mutable_owner() = input.owner();
            *output.mutable_source() = input.request().source();
            *output.mutable_content() = input.content().receipt().content();
            output.set_program_artifact(input.program_artifact());
            output.set_as_of_ms(times.as_of_ms);
            output.set_use_scope(input.request().use_scope());
            output.set_contract(payload.input().candidate().contract());
            output.set_effective_day(payload.assessment().effective_day());
            *output.mutable_margin_ratio() = payload.assessment().margin_ratio();
            output.set_planned(payload.assessment().planned_marker_present());
            if (times.source_published_at_ms)
                output.set_source_published_at_ms(*times.source_published_at_ms);
            output.set_acquired_at_ms(times.acquired_at_ms);
            output.set_processed_at_ms(times.processed_at_ms);
            output.set_verified_at_ms(times.verified_at_ms);
            output.set_stored_at_ms(times.stored_at_ms);
            output.set_available_at_ms(times.available_at_ms);
            output.set_published_by_cutoff(reviewed.published_by_cutoff);
            output.set_strict_history_eligible(reviewed.strict_history_eligible);
            for (const auto& limitation : reviewed.limitations)
                output.add_limitations(limitation);
            if (stop.stop_requested())
                throw Error(ErrorCode::cancelled, "task cancelled");
            progress(1, 1);
            break;
        }
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
