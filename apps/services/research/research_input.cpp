#include "research_execution.hpp"
#include <asterion/foundation/error.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/protocol/backtest.hpp>
#include <asterion/protocol/data_client.hpp>
#include <asterion/protocol/data.hpp>
#include <asterion/protocol/factor.hpp>
#include <asterion/protocol/research_variable.hpp>
#include <asterion/protocol/research_statistics.hpp>
#include <asterion/protocol/research_evaluation.hpp>
#include <asterion/protocol/research_sample.hpp>
#include <asterion/protocol/research_model.hpp>
#include <asterion/protocol/task_execution.hpp>
#include <asterion/protocol/account_review.hpp>

namespace asterion::tasks {

using namespace asterion::protocol;

void resolve_task_input(
    task::proto::TaskAttempt& attempt,
    std::stop_token stop,
    std::chrono::steady_clock::time_point deadline)
{
    auto& task = *attempt.mutable_task();
    if (task.kind() == task::proto::MINUTE_DOWNLOAD || task.kind() == task::proto::TABLE_UPDATE) {
        if (attempt.has_execution())
            throw std::invalid_argument("unexpected calculation definition");
        return;
    }
    const auto& execution = attempt.execution();
    if (task.research_plan().SerializeAsString() != execution.research_plan().SerializeAsString())
        throw std::invalid_argument("research plan differs from its fixed execution");
    if (task.has_variable_definition() != execution.has_variable_definition())
        throw std::invalid_argument("research variable differs from its fixed execution");
    if (execution.has_variable_definition())
        verify_variable_reference(task.variable_definition(), execution.variable_definition());
    if (task.program_artifact() != execution.program_artifact())
        throw std::invalid_argument("calculation program differs from its fixed execution");
    if (task.kind() == task::proto::SAMPLE) {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "task cancelled");
        if (!execution.has_sample())
            throw std::invalid_argument("sample selection differs from its fixed Data evidence");
        (void)task_data_references(execution);
        if (!task.has_sample()) {
            DataClient data(attempt.data_endpoint(), attempt.data_instance());
            data::proto::DataRequest request;
            *request.mutable_bar_dataset() = execution.sample().request().selection();
            const auto left = std::chrono::ceil<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (left <= std::chrono::milliseconds::zero())
                throw Error(ErrorCode::unavailable, "fixed input reconstruction timed out");
            auto response =
                data.call(std::move(request), std::min(left, std::chrono::milliseconds(120000)));
            if (stop.stop_requested())
                throw Error(ErrorCode::cancelled, "task cancelled");
            auto& input = *task.mutable_sample();
            *input.mutable_owner() = execution.sample().owner();
            *input.mutable_data_owner() = execution.data_owner();
            *input.mutable_request() = execution.sample().request();
            *input.mutable_dataset() = std::move(*response.mutable_bar_dataset());
        }
        if (sample_execution(task.sample()).SerializeAsString()
                != execution.sample().SerializeAsString()
            || task.sample().data_owner().SerializeAsString()
                != execution.data_owner().SerializeAsString()
            || sha256_bytes(task.sample().SerializeAsString()) != execution.input_sha256())
            throw std::invalid_argument("sample selection differs from its fixed Data evidence");
        return; // Normal execution uses the owned body; only missing preparation reads fixed Data.
    }
    if (task.kind() == task::proto::LOGIC_TRIAL) {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "task cancelled");
        if (!execution.has_logic_trial()
            || (task.has_logic_trial()
                && task.logic_trial().SerializeAsString()
                    != execution.logic_trial().SerializeAsString())
            || sha256_bytes(execution.logic_trial().SerializeAsString())
                != execution.input_sha256())
            throw std::invalid_argument("logic trial differs from its fixed execution");
        (void)task_data_references(execution);
        *task.mutable_logic_trial() = execution.logic_trial();
        return; // Fictional owned observations never resolve or read a Data service.
    }
    if (task.kind() == task::proto::MODEL_TRAINING) {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "task cancelled");
        if (!execution.has_model_training()
            || (task.has_model_training()
                && task.model_training().SerializeAsString()
                    != execution.model_training().SerializeAsString())
            || sha256_bytes(execution.model_training().SerializeAsString())
                != execution.input_sha256()
            || task.model_plan().SerializeAsString()
                != model_plan_object(execution.model_training().plan()).SerializeAsString())
            throw std::invalid_argument("model training differs from its fixed execution");
        (void)task_data_references(execution);
        *task.mutable_model_training() = execution.model_training();
        return; // Restore only the owned descriptor; never reconstruct original factor evidence.
    }
    if (task.kind() == task::proto::MODEL_TIME_VALIDATION) {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "task cancelled");
        if (!execution.has_model_time()
            || (task.has_model_time()
                && task.model_time().SerializeAsString()
                    != execution.model_time().SerializeAsString())
            || sha256_bytes(execution.model_time().SerializeAsString()) != execution.input_sha256()
            || task.model_time_plan().SerializeAsString()
                != model_time_plan_object(execution.model_time().plan()).SerializeAsString())
            throw std::invalid_argument("model training differs from its fixed execution");
        (void)task_data_references(execution);
        *task.mutable_model_time() = execution.model_time();
        return; // Restore only the owned descriptor; never reconstruct original factor evidence.
    }
    if (task.kind() == task::proto::PERFORMANCE_EVALUATION) {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "task cancelled");
        if (!execution.has_evaluation()
            || (task.has_evaluation()
                && task.evaluation().SerializeAsString()
                    != execution.evaluation().SerializeAsString())
            || sha256_bytes(execution.evaluation().SerializeAsString()) != execution.input_sha256())
            throw std::invalid_argument("invalid calculation execution definition");
        (void)task_data_references(execution);
        *task.mutable_evaluation() = execution.evaluation();
        return; // Own fixed account reference; never refetch Data or rerun the source.
    }
    if (task.kind() == task::proto::STATISTICS) {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "task cancelled");
        if (!execution.has_statistics() || execution.has_data_owner()
            || (task.has_statistics()
                && task.statistics().SerializeAsString()
                    != execution.statistics().SerializeAsString())
            || sha256_bytes(execution.statistics().SerializeAsString()) != execution.input_sha256())
            throw std::invalid_argument("invalid calculation execution definition");
        validate_statistics_input(execution.statistics());
        *task.mutable_statistics() = execution.statistics();
        return; // Fixed owned source descriptor; never discover or refetch Data.
    }
    if (task.kind() == task::proto::ACCOUNT_REVIEW) {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "task cancelled");
        if (!task.has_account_review() || !execution.has_account_review()
            || execution.has_data_owner()
            || sha256_bytes(task.account_review().SerializeAsString()) != execution.input_sha256()
            || account_review_execution(task.account_review()).SerializeAsString()
                != execution.account_review().SerializeAsString())
            throw std::invalid_argument("account review owned input differs from fixed execution");
        return; // Complete owned input; never construct a Data client or refetch Trading.
    }
    if (task.definition_case() != task::proto::Task::DEFINITION_NOT_SET
        || execution.input_sha256().size() != 64
        || (task.kind() == task::proto::BACKTEST ? !execution.has_backtest()
                                                 : !execution.has_factor()))
        throw std::invalid_argument("invalid calculation execution definition");
    DataClient data(attempt.data_endpoint(), attempt.data_instance());
    const auto check_stop = [&] {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "task cancelled");
    };
    const auto read = [&](data::proto::DataRequest request) {
        check_stop();
        const auto left = std::chrono::ceil<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (left <= std::chrono::milliseconds::zero())
            throw Error(ErrorCode::unavailable, "fixed input reconstruction timed out");
        auto result =
            data.call(std::move(request), std::min(left, std::chrono::milliseconds(120000)));
        check_stop();
        return result;
    };
    const auto bars = [&](const data::proto::BarDatasetRequest& selection) {
        check_stop();
        data::proto::DataRequest request;
        *request.mutable_bar_dataset() = selection;
        auto reply = read(std::move(request));
        check_stop();
        return std::move(*reply.mutable_bar_dataset());
    };
    std::string digest;
    if (execution.has_backtest()) {
        const auto& parameters = execution.backtest();
        if (!parameters.series().empty())
            throw std::invalid_argument("calculation requires fixed dominant schedules");
        auto* input = task.mutable_input();
        if (execution.has_data_owner())
            *input->mutable_data_owner() = execution.data_owner();
        *input->mutable_series() = execution.schedules();
        auto* paper = input->mutable_paper();
        *paper->mutable_deposit() = parameters.deposit();
        *paper->mutable_risk() = parameters.risk();
        std::size_t count = 0;
        for (const auto& contract : parameters.contracts()) {
            auto dataset = bars(contract.data());
            count += static_cast<std::size_t>(dataset.bars_size());
            if (count > max_dataset_bars)
                throw std::invalid_argument("dataset exceeds 200000 bars; narrow the date range");
            auto* item = paper->add_contracts();
            *item->mutable_dataset() = std::move(dataset);
            *item->mutable_cost_schedule() = contract.cost_schedule();
            item->set_slippage_ticks(contract.slippage_ticks());
        }
        *input->mutable_settlement_margins() = parameters.settlement_margins();
        if (execution.has_historical_margins())
            *input->mutable_historical_margins() = execution.historical_margins();
        set_backtest_strategies(*input, parameters);
        if (execution.has_model_replay())
            *input->mutable_model_replay() = execution.model_replay();
        input->set_dataset_revision(dataset_revision(*paper));
        digest = sha256_bytes(input->SerializeAsString());
    } else {
        const auto& parameters = execution.factor();
        auto* input = task.mutable_factor();
        *input = factor_input(parameters);
        if (execution.has_variable_definition()) {
            verify_variable_reference(
                parameters.variable_definition(),
                execution.variable_definition());
            *input->mutable_variable_definition() = execution.variable_definition();
        }
        if (execution.has_data_owner())
            *input->mutable_data_owner() = execution.data_owner();
        int schedule = 0;
        for (const auto& source : parameters.series()) {
            check_stop();
            if (source.has_dominant()) {
                // The fixed months under their fixed schedule: nothing is selected again.
                if (schedule == execution.schedules_size())
                    throw std::invalid_argument("calculation requires fixed dominant schedules");
                factor::proto::FactorSeries series;
                for (const auto& month : source.dominant().months())
                    *series.mutable_dominant()->add_months() = bars(month);
                *series.mutable_dominant()->mutable_schedule() = execution.schedules(schedule++);
                add_factor_series(*input, std::move(series));
                continue;
            }
            auto reply = read(factor_series_query(source));
            check_stop();
            for (auto& series : factor_series(source, std::move(reply)))
                add_factor_series(*input, std::move(series));
        }
        input->set_dataset_revision(factor_revision(input->series()));
        digest = sha256_bytes(input->SerializeAsString());
    }
    if (digest != execution.input_sha256())
        throw std::invalid_argument("calculation input does not match fixed task evidence");
    check_stop();
}

} // namespace asterion::tasks
