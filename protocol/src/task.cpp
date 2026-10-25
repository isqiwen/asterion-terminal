#include <asterion/protocol/task.hpp>
#include <asterion/protocol/research_project.hpp>
#include <asterion/protocol/research_strategy.hpp>
#include <asterion/protocol/research_variable.hpp>
#include <asterion/protocol/research_statistics.hpp>
#include <asterion/protocol/research_evaluation.hpp>
#include <asterion/protocol/research_sample.hpp>
#include <asterion/protocol/research_logic.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/protocol/factor.hpp>
#include <asterion/protocol/account_review.hpp>
#include <asterion/protocol/material.hpp>
#include <asterion/protocol/data.hpp>
#include <asterion/domain/trading_days.hpp>
#include <stdexcept>
#include <map>

namespace asterion::protocol {

Json decode_task_retirement(
    const task::proto::TaskRetirement& value,
    const std::string& id,
    const std::string& service,
    std::string_view domain)
{
    namespace wire = task::proto;
    const char* expected_kind = nullptr;
    if (domain == "research") {
        if (value.task().kind() == wire::BACKTEST)
            expected_kind = "backtest";
        if (value.task().kind() == wire::FACTOR)
            expected_kind = "factor";
        if (value.task().kind() == wire::ACCOUNT_REVIEW)
            expected_kind = "account_review";
        if (value.task().kind() == wire::STATISTICS)
            expected_kind = "statistics";
        if (value.task().kind() == wire::PERFORMANCE_EVALUATION)
            expected_kind = "performance_evaluation";
        if (value.task().kind() == wire::SAMPLE)
            expected_kind = "sample";
        if (value.task().kind() == wire::LOGIC_TRIAL)
            expected_kind = "logic_trial";
    } else if (domain == "data") {
        if (value.task().kind() == wire::MINUTE_DOWNLOAD)
            expected_kind = "minutes";
        if (value.task().kind() == wire::TABLE_UPDATE)
            expected_kind = "table_update";
        if (value.task().kind() == wire::MATERIAL_ACQUISITION)
            expected_kind = "material_acquisition_task";
    }
    const auto& object = value.object();
    const auto state = value.state();
    if (object.id() != id || value.task().id() != object.id()
        || object.owner().service_id() != service || object.owner().domain() != domain
        || object.version().size() != 64
        || object.version().find_first_not_of("0123456789abcdef") != std::string::npos
        || value.consumers_size() > 257 || !expected_kind || object.kind() != expected_kind
        || state < wire::TaskRetirement::AVAILABLE || state > wire::TaskRetirement::RETIRED
        || (state == wire::TaskRetirement::AVAILABLE
            && (value.active() || value.admission_busy() || value.consumers_size()))
        || (state == wire::TaskRetirement::RETIRED
            && value.task().retirement().SerializeAsString() != object.SerializeAsString()))
        throw Error(ErrorCode::unavailable, "invalid task retirement response");
    validate_id(object.owner().node_id());
    Json consumers = Json::array();
    for (const auto& id : value.consumers()) {
        validate_id(id);
        consumers.push_back(id);
    }
    return Json{
        {"id", object.id()},
        {"node", object.owner().node_id()},
        {"service", object.owner().service_id()},
        {"version", object.version()},
        {"state",
            state == wire::TaskRetirement::AVAILABLE     ? "available"
                : state == wire::TaskRetirement::BLOCKED ? "blocked"
                                                         : "retired"},
        {"active", value.active()},
        {"admission_busy", value.admission_busy()},
        {"consumers", std::move(consumers)},
        {"task", protocol::decode_task(value.task())},
        {"source_release_confirmed", value.source_release_confirmed()},
        {"release_error",
            value.has_release_error()
                ? Json{{"code", value.release_error().code()}, {"message", value.release_error().message()}}
                : Json(nullptr)}
    };
}

Json decode_task_page(const task::proto::TaskList& list, unsigned before)
{
    const auto& usage = list.capacity();
    if (!list.has_capacity() || !usage.active_limit() || usage.active_used() > usage.active_limit()
        || usage.active_used() > usage.retained_tasks() || usage.active_reserved() > 1
        || usage.active_reserved() > usage.active_limit() - usage.active_used())
        throw std::invalid_argument("invalid task capacity");
    Json storage{
        {"retained_tasks", usage.retained_tasks()},
        {"active_used", usage.active_used()},
        {"active_reserved", usage.active_reserved()},
        {"uncommitted", usage.uncommitted()},
        {"active_limit", usage.active_limit()}
    };
    if (list.tasks_size() > 200 || list.active_tasks_size() != static_cast<int>(usage.active_used())
        || (list.next_before_sequence()
            && (list.tasks().empty()
                || list.next_before_sequence() != list.tasks(0).submission_sequence())))
        throw std::invalid_argument("invalid task page response");
    std::map<unsigned, Json> ordered;
    unsigned previous = 0;
    for (const auto& task : list.tasks()) {
        if (task.submission_sequence() <= previous
            || (before && task.submission_sequence() >= before))
            throw std::invalid_argument("invalid task page response");
        previous = task.submission_sequence();
        ordered.emplace(previous, decode_task(task));
    }
    for (const auto& task : list.active_tasks()) {
        if (task.state() != task::proto::QUEUED && task.state() != task::proto::RUNNING
            && task.state() != task::proto::CANCEL_REQUESTED
            && task.state() != task::proto::PUBLISHING)
            throw std::invalid_argument("invalid task page response");
        auto value = decode_task(task);
        const auto [found, added] = ordered.emplace(task.submission_sequence(), value);
        if (!added && found->second != value)
            throw std::invalid_argument("invalid task page response");
    }
    Json values = Json::array();
    for (auto& [sequence, value] : ordered)
        values.push_back(std::move(value));
    return Json{
        {"capacity", std::move(storage)},
        {"before_sequence", before},
        {"next_before_sequence", list.next_before_sequence()},
        {"failed_count", list.failed_count()},
        {"interrupted_count", list.interrupted_count()},
        {"tasks", std::move(values)}
    };
}

Json decode_task(const task::proto::Task& task)
{
    validate_message(task);
    if (!task.submission_sequence() || task.submitted_at_ms() <= 0
        || task.submitted_at_ms() > 9223372036854LL || task.updated_at_ms() <= 0
        || task.updated_at_ms() > 9223372036854LL)
        throw std::invalid_argument("invalid task chronology");
    std::string state;
    switch (task.state()) {
        case task::proto::QUEUED:
            state = "queued";
            break;
        case task::proto::RUNNING:
            state = "running";
            break;
        case task::proto::CANCEL_REQUESTED:
            state = "cancel_requested";
            break;
        case task::proto::PUBLISHING:
            state = "publishing";
            break;
        case task::proto::SUCCEEDED:
            state = "succeeded";
            break;
        case task::proto::FAILED:
            state = "failed";
            break;
        case task::proto::CANCELLED:
            state = "cancelled";
            break;
        case task::proto::INTERRUPTED:
            state = "interrupted";
            break;
        default:
            throw std::invalid_argument("invalid task state");
    }
    if (task.kind() != task::proto::BACKTEST && task.kind() != task::proto::FACTOR
        && task.kind() != task::proto::MINUTE_DOWNLOAD && task.kind() != task::proto::TABLE_UPDATE
        && task.kind() != task::proto::ACCOUNT_REVIEW && task.kind() != task::proto::STATISTICS
        && task.kind() != task::proto::SAMPLE && task.kind() != task::proto::LOGIC_TRIAL
        && task.kind() != task::proto::MATERIAL_ACQUISITION
        && task.kind() != task::proto::PERFORMANCE_EVALUATION)
        throw std::invalid_argument("invalid task kind");
    Json output = {
        {"kind",
            task.kind() == task::proto::MINUTE_DOWNLOAD              ? "minute_download"
                : task.kind() == task::proto::TABLE_UPDATE           ? "table_update"
                : task.kind() == task::proto::FACTOR                 ? "factor"
                : task.kind() == task::proto::ACCOUNT_REVIEW         ? "account_review"
                : task.kind() == task::proto::STATISTICS             ? "statistics"
                : task.kind() == task::proto::PERFORMANCE_EVALUATION ? "performance_evaluation"
                : task.kind() == task::proto::SAMPLE                 ? "sample"
                : task.kind() == task::proto::LOGIC_TRIAL            ? "logic_trial"
                : task.kind() == task::proto::MATERIAL_ACQUISITION   ? "material_acquisition"
                                                                     : "backtest"},
        {"id", task.id()},
        {"metadata",
            {{"revision", task.metadata().revision()},
                {"name", task.metadata().name()},
                {"note", task.metadata().note()},
                {"source_id", task.metadata().derivation().source_id()},
                {"source_instance", task.metadata().derivation().source_instance()},
                {"source_result_digest", task.metadata().derivation().source_result_digest()},
                {"data_choice", task.metadata().derivation().data_choice()},
                {"snapshot", task.metadata().derivation().snapshot()}}},

        {"provider_artifact", task.provider_artifact()},
        {"risk_artifact", task.risk_artifact()},
        {"program_artifact", task.program_artifact()},
        {"state", state},
        {"attempt", task.attempt()},
        {"completed", task.completed()},
        {"total", task.total()},
        {"error", task.error()},
        {"result_digest", task.result_digest()},
        {"trading_day", task.trading_day()},
        {"instrument", task.instrument()},
        {"source_name", task.source_name()},
        {"history_dataset_id", task.history_dataset_id()},
        {"data_source", task.has_minutes() ? task.minutes().source() : task.data_source()},
        {"submission_sequence", task.submission_sequence()},
        {"submitted_at_ms", task.submitted_at_ms()},
        {"updated_at_ms", task.updated_at_ms()}
    };
    if (task.has_research_plan())
        output["research_plan"] = decode_project_binding(task.research_plan());
    if (task.has_strategy_use())
        output["strategy_use"] = decode_strategy_use(task.strategy_use());
    if (task.has_variable_definition())
        output["variable_definition"] = decode_variable_reference(task.variable_definition());
    if (!task.error_code().empty())
        output["error_code"] = task.error_code();
    if (task.has_material())
        output["material_request"] = decode_material_request(task.material().request());
    if (task.has_table_refresh()) {
        const auto& ref = task.table_refresh();
        output["table_refresh"] = {
            {"partition",
                {{"table", ref.partition().table()},
                    {"exchange", ref.partition().exchange()},
                    {"period", ref.partition().period()}}},
            {"source", ref.source()},
            {"previous_snapshot", ref.previous_snapshot()},
            {"previous_content", ref.previous_content()},
            {"provider_artifact", ref.provider_artifact()}
        };
    }
    if (task.kind() == task::proto::MINUTE_DOWNLOAD) {
        const auto interval =
            task.has_minutes() ? task.minutes().interval_minutes() : task.minute_interval_minutes();
        if (interval < 1 || interval > 1440)
            throw std::invalid_argument("invalid minute dataset page response");
        output["minute_interval_minutes"] = interval;
    }
    if (task.has_retirement())
        output["retired"] = true;
    return output;
}

Json decode_task_result(const task::proto::TaskResponse& response, const std::string& id)
{
    validate_message(response);
    if (!response.has_result_task())
        throw std::invalid_argument("task result is missing its task evidence");
    const auto& task = response.result_task();
    const auto metadata = decode_task(task);
    if (task.id() != id || task.state() != task::proto::SUCCEEDED || !task.attempt()
        || task.completed() != task.total() || task.result_digest().size() != 64
        || task.result_digest().find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::invalid_argument("task result task identity or completion mismatch");
    const auto kind = metadata.at("kind");
    Json envelope = {{"id", id}, {"kind", kind}, {"task", metadata}};
    if (task.kind() == task::proto::MINUTE_DOWNLOAD && task.has_minutes()
        && response.has_minutes()) {
        const auto& result = response.minutes();
        const auto& input = task.minutes();
        if (result.manifest_sha256().size() != 64 || result.directory().empty())
            throw std::invalid_argument("invalid minute download result");
        envelope["result"] = {
            {"directory", result.directory()},
            {"manifest_sha256", result.manifest_sha256()},
            {"rows", result.rows()},
            {"pages", result.pages()}
        };
        envelope["experiment"] = {
            {"source", input.source()},
            {"contract_id", input.contract_id()},
            {"interval_minutes", input.interval_minutes()},
            {"begin_ns", std::to_string(input.begin_ns())},
            {"end_ns", std::to_string(input.end_ns())}
        };
    } else if (task.kind() == task::proto::TABLE_UPDATE && task.has_table_update()
        && response.has_table_update()) {
        envelope["result"] = decode_table_update_result(response.table_update());
        envelope["experiment"] = decode_table_update(task.table_update());
    } else if (task.kind() == task::proto::MATERIAL_ACQUISITION && task.has_material()
        && response.has_material()) {
        validate_material_result(task.material(), response.material());
        if (sha256_bytes(response.material().SerializeAsString()) != task.result_digest())
            throw std::invalid_argument("material result digest mismatch");
        envelope["experiment"] = decode_material_request(task.material().request());
        envelope["result"] = decode_material_receipt(response.material());
    } else if (task.kind() == task::proto::BACKTEST && task.has_input()
        && response.has_backtest()) {
        envelope["input_sha256"] = sha256_bytes(task.input().SerializeAsString());
        auto experiment = decode_backtest(task.input(), DatasetView::metadata);
        if (response.backtest().dataset_revision() != task.input().dataset_revision())
            throw std::invalid_argument("backtest result does not belong to input data");
        Json data = Json::array();
        for (const auto& contract : task.input().paper().contracts())
            data.push_back(decode_bar_dataset_range(contract.dataset()));
        experiment["data"] = std::move(data);
        envelope["experiment"] = std::move(experiment);
        envelope["result"] = decode_backtest_result(response.backtest());
    } else if (task.kind() == task::proto::FACTOR && task.has_factor() && response.has_factor()) {
        envelope["experiment"] = decode_factor(task.factor(), DatasetView::metadata);
        envelope["result"] = decode_factor_result(task.factor(), response.factor());
    } else if (task.kind() == task::proto::LOGIC_TRIAL && task.has_logic_trial()
        && response.has_logic_trial()) {
        if (sha256_bytes(response.logic_trial().SerializeAsString()) != task.result_digest())
            throw std::invalid_argument("logic trial result digest mismatch");
        envelope["input_sha256"] = sha256_bytes(task.logic_trial().SerializeAsString());
        envelope["experiment"] = decode_logic_trial_input(task.logic_trial());
        envelope["result"] = decode_logic_trial_result(task.logic_trial(), response.logic_trial());
    } else if (task.kind() == task::proto::SAMPLE && task.has_sample() && response.has_sample()) {
        if (sha256_bytes(response.sample().SerializeAsString()) != task.result_digest())
            throw std::invalid_argument("sample result differs from its fixed input");
        envelope["input_sha256"] = sha256_bytes(task.sample().SerializeAsString());
        envelope["experiment"] = decode_sample_input(task.sample());
        envelope["result"] = decode_sample_result(task.sample(), response.sample());
    } else if (task.kind() == task::proto::PERFORMANCE_EVALUATION && task.has_evaluation()
        && response.has_evaluation()) {
        if (sha256_bytes(response.evaluation().SerializeAsString()) != task.result_digest())
            throw std::invalid_argument("evaluation result digest mismatch");
        envelope["input_sha256"] = sha256_bytes(task.evaluation().SerializeAsString());
        envelope["experiment"] = decode_evaluation_input(task.evaluation());
        envelope["result"] = decode_evaluation_result(task.evaluation(), response.evaluation());
    } else if (task.kind() == task::proto::STATISTICS && task.has_statistics()
        && response.has_statistics()) {
        if (sha256_bytes(response.statistics().SerializeAsString()) != task.result_digest())
            throw std::invalid_argument("statistics result digest mismatch");
        envelope["input_sha256"] = sha256_bytes(task.statistics().SerializeAsString());
        envelope["experiment"] = decode_statistics_input(task.statistics());
        envelope["result"] = decode_statistics_result(task.statistics(), response.statistics());
    } else if (task.kind() == task::proto::ACCOUNT_REVIEW && task.has_account_review()
        && response.has_account_review()) {
        if (sha256_bytes(response.account_review().SerializeAsString()) != task.result_digest())
            throw std::invalid_argument("account review result digest mismatch");
        envelope["input_sha256"] = sha256_bytes(task.account_review().SerializeAsString());
        envelope["experiment"] = decode_account_review_input(task.account_review());
        envelope["result"] =
            decode_account_review_result(task.account_review(), response.account_review());
    } else {
        throw std::invalid_argument("task input and result kind mismatch");
    }
    return envelope;
}

} // namespace asterion::protocol
