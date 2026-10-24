#include "research_batch.hpp"
#include <asterion/protocol/research_batch.hpp>
#include <asterion/protocol/backtest.hpp>
#include <asterion/foundation/decimal.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <stdexcept>

namespace asterion::research {

proto::BacktestBatchDefinition capture_backtest_batch(
    const resource::proto::Owner& owner,
    const proto::BacktestBatchDraft& draft,
    const task::proto::TaskResponse& verified,
    const std::string& input_digest,
    const std::string& current_program,
    const std::string& current_risk,
    std::int64_t recorded_at_ms)
{
    protocol::validate_backtest_batch_draft(draft);
    const auto& task = verified.result_task();
    if (draft.source().owner().SerializeAsString() != owner.SerializeAsString()
        || !verified.has_result_task() || !verified.has_backtest()
        || task.kind() != task::proto::BACKTEST || task.state() != task::proto::SUCCEEDED
        || task.has_retirement() || !task.has_input() || task.id() != draft.source().id()
        || task.result_digest() != draft.source().version()
        || current_program != task.program_artifact() || current_risk != task.risk_artifact()
        || input_digest != sha256_bytes(task.input().SerializeAsString()))
        throw std::invalid_argument(
            "backtest batch requires its verified original source and implementations");
    protocol::validate_backtest_result(task.input(), verified.backtest());
    for (const auto& member : draft.members())
        (void)protocol::backtest_batch_member_input(task.input(), member);
    proto::BacktestBatchDefinition result;
    *result.mutable_request() = draft;
    *result.mutable_owner() = owner;
    result.set_source_input_sha256(input_digest);
    result.set_program_artifact(task.program_artifact());
    result.set_risk_artifact(task.risk_artifact());
    result.set_work_units_per_run(protocol::backtest_work_units(task.input()));
    result.set_recorded_at_ms(recorded_at_ms);
    *result.mutable_data_owner() = task.input().data_owner();
    result.set_digest(protocol::backtest_batch_digest(result));
    protocol::validate_backtest_batch_definition(result);
    return result;
}

proto::BatchAssessmentTask capture_batch_assessment_task(
    const task::proto::Task& task,
    const std::string& input_digest,
    const task::proto::TaskResponse* verified)
{
    if (task.kind() != task::proto::BACKTEST)
        throw std::invalid_argument("batch assessment requires an original backtest task");
    proto::BatchAssessmentTask result;
    result.set_id(task.id());
    result.set_state(task.state());
    result.set_attempt(task.attempt());
    result.set_retired(task.has_retirement());
    result.set_error(task.error());
    result.set_input_sha256(input_digest);
    result.set_program_artifact(task.program_artifact());
    result.set_risk_artifact(task.risk_artifact());
    if (task.state() != task::proto::SUCCEEDED) {
        if (verified)
            throw std::invalid_argument(
                "batch assessment cannot attach account facts to an incomplete task");
        return result;
    }
    if (!verified || !verified->has_result_task() || !verified->has_backtest())
        throw std::invalid_argument("batch assessment requires a verified published account");
    const auto& actual = verified->result_task();
    if (actual.id() != task.id() || actual.state() != task.state()
        || actual.attempt() != task.attempt() || actual.result_digest() != task.result_digest()
        || actual.program_artifact() != task.program_artifact()
        || actual.risk_artifact() != task.risk_artifact() || !actual.has_input()
        || input_digest != sha256_bytes(actual.input().SerializeAsString()))
        throw std::invalid_argument(
            "batch assessment verified account identity differs from observed task");
    protocol::validate_backtest_result(actual.input(), verified->backtest());
    const auto& source = verified->backtest();
    if (source.settlements().empty())
        throw std::invalid_argument("batch assessment account has no original settlements");
    const auto& currency = actual.input().paper().contracts(0).dataset().contract().currency();
    for (const auto& contract : actual.input().paper().contracts())
        if (contract.dataset().contract().currency() != currency)
            throw std::invalid_argument("batch assessment requires one original account currency");
    result.set_result_sha256(actual.result_digest());
    auto& account = *result.mutable_account();
    account.set_currency(currency);
    account.set_first_day(source.settlements(0).trading_day());
    account.set_last_day(source.settlements(source.settlements_size() - 1).trading_day());
    account.set_settlement_days(source.settlements_size());
    *account.mutable_deposit() = actual.input().paper().deposit();
    *account.mutable_closing_equity() = source.account().equity();
    *account.mutable_fees() = source.account().fees();
    account.mutable_net_profit()->set_units(
        (Decimal::from_raw(source.account().equity().units())
            - Decimal::from_raw(actual.input().paper().deposit().units()))
            .raw());
    return result;
}

} // namespace asterion::research
