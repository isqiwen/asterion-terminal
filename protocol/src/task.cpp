#include <asterion/protocol/task.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/protocol/factor.hpp>
#include <asterion/protocol/data.hpp>
#include <asterion/domain/trading_days.hpp>
#include <stdexcept>

namespace asterion::protocol {

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
        && task.kind() != task::proto::MINUTE_DOWNLOAD && task.kind() != task::proto::TABLE_UPDATE)
        throw std::invalid_argument("invalid task kind");
    Json output = {
        {"kind",
            task.kind() == task::proto::MINUTE_DOWNLOAD    ? "minute_download"
                : task.kind() == task::proto::TABLE_UPDATE ? "table_update"
                : task.kind() == task::proto::FACTOR       ? "factor"
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
    if (!task.error_code().empty())
        output["error_code"] = task.error_code();
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
    } else {
        throw std::invalid_argument("task input and result kind mismatch");
    }
    return envelope;
}

} // namespace asterion::protocol
