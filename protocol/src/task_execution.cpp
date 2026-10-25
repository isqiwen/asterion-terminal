#include <asterion/protocol/backtest.hpp>
#include <asterion/protocol/task_execution.hpp>
#include <asterion/protocol/research_project.hpp>
#include <asterion/protocol/research_strategy.hpp>
#include <asterion/protocol/research_variable.hpp>
#include <asterion/protocol/research_statistics.hpp>
#include <asterion/protocol/research_evaluation.hpp>
#include <asterion/protocol/research_sample.hpp>
#include <asterion/protocol/research_logic.hpp>
#include <asterion/protocol/research_model.hpp>
#include <asterion/foundation/id.hpp>
#include <asterion/protocol/data.hpp>
#include <asterion/protocol/account_review.hpp>
#include <stdexcept>
#include <set>

namespace asterion::protocol {

data::proto::BarDatasetRequest bar_dataset_selection(const data::proto::BarDataset& dataset)
{
    data::proto::BarDatasetRequest result;
    result.set_contract(history_identity(dataset.contract()).key());
    *result.mutable_source_dataset_ids() = dataset.source_dataset_ids();
    *result.mutable_daily() = dataset.daily();
    // Its days are every day of the range it settled on, with bars or without.
    result.set_begin_day(dataset.days(0).trading_day());
    result.set_end_day(dataset.days(dataset.days_size() - 1).trading_day());
    return result;
}

task::proto::TaskExecution
task_execution(const task::proto::Task& task, const std::string& input_sha256)
{
    task::proto::TaskExecution result;
    result.set_input_sha256(input_sha256);
    result.set_program_artifact(task.program_artifact());
    if (task.has_research_plan()) {
        validate_project_binding(task.research_plan());
        *result.mutable_research_plan() = task.research_plan();
    }
    if (task.has_strategy_use()) {
        validate_strategy_use(task.strategy_use());
        if (!task.has_input())
            throw std::invalid_argument("invalid fixed strategy use owner or task kind");
        *result.mutable_strategy_use() = task.strategy_use();
    }
    if (task.has_input()) {
        const auto& input = task.input();
        if (input.has_data_owner())
            *result.mutable_data_owner() = input.data_owner();
        if (input.has_model_replay())
            *result.mutable_model_replay() = input.model_replay();
        auto* parameters = result.mutable_backtest();
        if (input.has_performance_assumptions())
            *parameters->mutable_performance_assumptions() = input.performance_assumptions();
        *parameters->mutable_deposit() = input.paper().deposit();
        *parameters->mutable_risk() = input.paper().risk();
        *parameters->mutable_strategies() = input.strategies();
        *parameters->mutable_settlement_margins() = input.settlement_margins();
        if (input.has_historical_margins())
            *result.mutable_historical_margins() = input.historical_margins();
        parameters->set_holdout_from(input.holdout_day());
        if (input.has_walk_forward())
            *parameters->mutable_walk_forward() = input.walk_forward();
        for (const auto& contract : input.paper().contracts()) {
            auto* item = parameters->add_contracts();
            *item->mutable_data() = bar_dataset_selection(contract.dataset());
            *item->mutable_cost_schedule() = contract.cost_schedule();
            item->set_slippage_ticks(contract.slippage_ticks());
        }
        *result.mutable_schedules() = input.series();
    } else if (task.has_factor()) {
        const auto& input = task.factor();
        if (input.has_data_owner())
            *result.mutable_data_owner() = input.data_owner();
        auto* parameters = result.mutable_factor();
        for (const auto& series : input.series()) {
            if (series.has_bars())
                *parameters->add_series()->mutable_bars() = bar_dataset_selection(series.bars());
            else if (series.has_table()) {
                // Instruments read under one snapshot and range are asked for together.
                const auto& table = series.table();
                const bool same = parameters->series_size()
                    && parameters->series(parameters->series_size() - 1).has_tables() &&
                    [&](const data::proto::TableSeriesRequest& last) {
                        return last.snapshot() == table.snapshot()
                            && last.daily_source() == table.daily_source()
                            && last.calendar_source() == table.calendar_source()
                            && last.begin_day() == table.begin_day()
                            && last.end_day() == table.end_day();
                    }(parameters->series(parameters->series_size() - 1).tables());
                auto* request = same
                    ? parameters->mutable_series(parameters->series_size() - 1)->mutable_tables()
                    : parameters->add_series()->mutable_tables();
                request->set_snapshot(table.snapshot());
                request->set_daily_source(table.daily_source());
                request->set_calendar_source(table.calendar_source());
                request->set_begin_day(table.begin_day());
                request->set_end_day(table.end_day());
                request->add_instruments(table.instrument());
            } else {
                // The months as they were fixed, and their schedule beside them.
                auto* months = parameters->add_series()->mutable_dominant();
                for (const auto& month : series.dominant().months())
                    *months->add_months() = bar_dataset_selection(month);
                *result.add_schedules() = series.dominant().schedule();
            }
        }
        if (input.has_liquidity())
            *parameters->mutable_liquidity() = input.liquidity();
        if (input.has_variable_definition()) {
            validate_variable_definition(input.variable_definition());
            *result.mutable_variable_definition() = input.variable_definition();
            *parameters->mutable_variable_definition() =
                variable_object(input.variable_definition());
            if (task.has_variable_definition())
                verify_variable_reference(task.variable_definition(), input.variable_definition());
        } else if (task.has_variable_definition())
            throw std::invalid_argument("research variable definition missing from fixed input");
        parameters->set_factor(input.factor());
        parameters->set_retain_observations(input.retain_observations());
        *parameters->mutable_lookbacks() = input.lookbacks();
        parameters->set_horizon(input.horizon());
        if (input.has_full_sample())
            parameters->set_full_sample(input.full_sample());
        else if (input.has_holdout_start())
            parameters->set_holdout_start(input.holdout_start());
        else
            *parameters->mutable_walk_forward() = input.walk_forward();
    } else if (task.has_sample()) {
        validate_sample_input(task.sample());
        *result.mutable_data_owner() = task.sample().data_owner();
        *result.mutable_sample() = sample_execution(task.sample());
    } else if (task.has_logic_trial()) {
        validate_logic_trial_input(task.logic_trial());
        *result.mutable_logic_trial() = task.logic_trial();
    } else if (task.has_model_training()) {
        validate_model_training_input(task.model_training());
        const auto& plan = task.model_training().plan();
        if (!task.has_model_plan()
            || task.model_plan().SerializeAsString() != model_plan_object(plan).SerializeAsString()
            || task.program_artifact() != plan.program_artifact())
            throw std::invalid_argument("model training differs from its fixed plan or program");
        *result.mutable_model_training() = task.model_training();
    } else if (task.has_model_time()) {
        validate_model_time_input(task.model_time());
        const auto& plan = task.model_time().plan();
        if (!task.has_model_time_plan()
            || task.model_time_plan().SerializeAsString()
                != model_time_plan_object(plan).SerializeAsString()
            || task.program_artifact() != plan.recipe().program_artifact())
            throw std::invalid_argument("model training differs from its fixed plan or program");
        *result.mutable_model_time() = task.model_time();
    } else if (task.has_evaluation()) {
        validate_evaluation_input(task.evaluation());
        *result.mutable_evaluation() = task.evaluation();
    } else if (task.has_statistics()) {
        validate_statistics_input(task.statistics());
        *result.mutable_statistics() = task.statistics();
    } else if (task.has_account_review()) {
        *result.mutable_account_review() = account_review_execution(task.account_review());
    } else {
        throw std::invalid_argument("missing calculation input");
    }
    return result;
}

TaskDataReferences task_data_references(const task::proto::TaskExecution& execution)
{
    if (!execution.program_artifact().empty()
        && ((!execution.has_backtest() && !execution.has_factor() && !execution.has_statistics()
                && !execution.has_sample() && !execution.has_logic_trial()
                && !execution.has_model_training() && !execution.has_model_time()
                && !execution.has_evaluation())
            || execution.program_artifact().size() != 64
            || execution.program_artifact().find_first_not_of("0123456789abcdef")
                != std::string::npos))
        throw std::invalid_argument("invalid fixed calculation program identity");
    if (execution.has_research_plan())
        validate_project_binding(execution.research_plan());
    if (execution.has_strategy_use()) {
        validate_strategy_use(execution.strategy_use());
        if (!execution.has_backtest())
            throw std::invalid_argument("invalid fixed strategy use owner or task kind");
    }
    if (execution.has_model_replay()) {
        if (!execution.has_backtest() || !execution.backtest().strategies().empty())
            throw std::invalid_argument("model replay requires an exclusive backtest definition");
        validate_model_replay_definition(execution.model_replay());
        if (execution.program_artifact()
            != execution.model_replay().training().plan().recipe().program_artifact())
            throw std::invalid_argument("model replay requires its original calculation program");
    }
    if (execution.has_variable_definition()) {
        if (!execution.has_factor() || !execution.factor().has_variable_definition())
            throw std::invalid_argument("invalid fixed research variable execution");
        verify_variable_reference(
            execution.factor().variable_definition(),
            execution.variable_definition());
    } else if (execution.has_factor() && execution.factor().has_variable_definition())
        throw std::invalid_argument("research variable definition missing from fixed execution");
    std::set<std::string> minutes, snapshots;
    const auto bars = [&](const data::proto::BarDatasetRequest& value) {
        minutes.insert(value.source_dataset_ids().begin(), value.source_dataset_ids().end());
        snapshots.insert(value.daily().snapshot());
    };
    if (execution.has_backtest()) {
        if (execution.backtest().has_performance_assumptions())
            validate_performance_assumptions(execution.backtest().performance_assumptions());
        for (const auto& contract : execution.backtest().contracts())
            bars(contract.data());
        if (execution.has_historical_margins())
            snapshots.insert(execution.historical_margins().snapshot());
    } else if (execution.has_factor()) {
        for (const auto& series : execution.factor().series()) {
            if (series.has_bars())
                bars(series.bars());
            else if (series.has_tables())
                snapshots.insert(series.tables().snapshot());
            else if (series.has_dominant())
                for (const auto& month : series.dominant().months())
                    bars(month);
        }
    } else if (execution.has_sample()) {
        validate_message(execution.sample());
        validate_sample_request(execution.sample().request());
        const auto& owner = execution.sample().owner();
        validate_id(owner.node_id());
        validate_id(owner.service_id());
        validate_id(execution.data_owner().node_id());
        validate_id(execution.data_owner().service_id());
        if (owner.domain() != "research" || owner.node_id() != execution.data_owner().node_id()
            || !execution.schedules().empty() || execution.has_historical_margins())
            throw std::invalid_argument("sample selection differs from its fixed Data evidence");
        bars(execution.sample().request().selection());
    } else if (execution.has_logic_trial()) {
        validate_logic_trial_input(execution.logic_trial());
        if (execution.has_data_owner() || !execution.schedules().empty()
            || execution.has_historical_margins() || execution.has_research_plan()
            || execution.has_strategy_use() || execution.has_variable_definition())
            throw std::invalid_argument("logic trial execution contains unrelated parameters");
    } else if (execution.has_model_training()) {
        validate_model_training_input(execution.model_training());
        if (execution.program_artifact() != execution.model_training().plan().program_artifact()
            || execution.has_data_owner() || !execution.schedules().empty()
            || execution.has_historical_margins() || execution.has_research_plan()
            || execution.has_strategy_use() || execution.has_variable_definition())
            throw std::invalid_argument("model training execution contains unrelated parameters");
    } else if (execution.has_model_time()) {
        validate_model_time_input(execution.model_time());
        if (execution.program_artifact()
                != execution.model_time().plan().recipe().program_artifact()
            || execution.has_data_owner() || !execution.schedules().empty()
            || execution.has_historical_margins() || execution.has_research_plan()
            || execution.has_strategy_use() || execution.has_variable_definition())
            throw std::invalid_argument("model training execution contains unrelated parameters");
    } else if (execution.has_evaluation()) {
        validate_evaluation_input(execution.evaluation());
        if (execution.program_artifact().empty() || execution.has_data_owner()
            || !execution.schedules().empty() || execution.has_historical_margins()
            || execution.has_research_plan() || execution.has_strategy_use()
            || execution.has_variable_definition())
            throw std::invalid_argument(
                "evaluation execution contains unrelated or missing parameters");
    } else if (execution.has_statistics()) {
        validate_statistics_input(execution.statistics());
        if (execution.has_data_owner() || !execution.schedules().empty()
            || execution.has_historical_margins())
            throw std::invalid_argument("statistics execution contains unrelated parameters");
    } else if (execution.has_account_review()) {
        validate_account_review_execution(execution.account_review());
    } else
        throw std::invalid_argument("task references require fixed calculation parameters");
    const auto validate = [](const auto& values) {
        for (const auto& value : values)
            if (value.size() != 64
                || value.find_first_not_of("0123456789abcdef") != std::string::npos)
                throw std::invalid_argument("invalid fixed task data identity");
    };
    validate(minutes);
    validate(snapshots);
    return {{minutes.begin(), minutes.end()}, {snapshots.begin(), snapshots.end()}};
}

} // namespace asterion::protocol
