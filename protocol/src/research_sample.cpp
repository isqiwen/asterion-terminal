#include <asterion/protocol/research_sample.hpp>
#include <asterion/protocol/data.hpp>
#include <asterion/base/id.hpp>
#include <algorithm>
#include <stdexcept>

namespace asterion::protocol {
namespace {
namespace r = research::proto;

Json owner(const resource::proto::Owner& value)
{
    return {
        {"node_id", value.node_id()},
        {"service_id", value.service_id()},
        {"domain", value.domain()}
    };
}

Json request(const r::SampleRequest& value)
{
    Json result = {
        {"selection", decode_bar_dataset_request(value.selection())},
        {"use_scope", value.use_scope()},
        {"mode", "post_hoc"}
    };
    result["evaluation"] = value.has_full_sample()
        ? Json{{"mode", "full_sample"}}
        : Json{{"mode", "holdout"}, {"split_index", value.development_end()}};
    return result;
}

void input_shape(const r::SampleInput& value)
{
    validate_message(value);
    validate_sample_request(value.request());
    validate_id(value.owner().node_id());
    validate_id(value.owner().service_id());
    validate_id(value.data_owner().node_id());
    validate_id(value.data_owner().service_id());
    if (value.owner().domain() != "research")
        throw std::invalid_argument("invalid research sample owner");
    if (value.owner().node_id() != value.data_owner().node_id())
        throw std::invalid_argument("sample input cannot cross business nodes");
    const auto& data = value.dataset();
    const auto& selected = value.request().selection();
    if (data.bars_size() < 1 || data.bars_size() > 200000 || !data.has_contract()
        || history_identity(data.contract()).key() != selected.contract()
        || data.daily().SerializeAsString() != selected.daily().SerializeAsString()
        || !std::equal(
            data.source_dataset_ids().begin(),
            data.source_dataset_ids().end(),
            selected.source_dataset_ids().begin(),
            selected.source_dataset_ids().end()))
        throw std::invalid_argument("sample selection differs from its fixed Data evidence");
    if (value.request().has_development_end()
        && value.request().development_end() >= static_cast<unsigned>(data.bars_size()))
        throw std::invalid_argument(
            "sample development boundary is outside the actual observations");
    for (const auto& bar : data.bars())
        if ((!selected.begin_day().empty() && bar.trading_day() < selected.begin_day())
            || (!selected.end_day().empty() && bar.trading_day() > selected.end_day()))
            throw std::invalid_argument("sample selection differs from its fixed Data evidence");
}

} // namespace

void validate_sample_request(const r::SampleRequest& value)
{
    validate_message(value);
    if (!value.has_selection())
        throw std::invalid_argument("sample selection differs from its fixed Data evidence");
    (void)decode_bar_dataset_request(value.selection());
    if (value.mode() != r::SampleRequest::POST_HOC)
        throw std::invalid_argument("sample requires explicit post-hoc exploration");
    if (value.use_scope().empty() || value.use_scope().size() > 4096
        || value.use_scope().find_first_not_of(" \t\r\n") == std::string::npos
        || value.use_scope().find('\0') != std::string::npos)
        throw std::invalid_argument("sample scope is required");
    if ((!value.has_full_sample() && !value.has_development_end())
        || (value.has_full_sample() && !value.full_sample())
        || (value.has_development_end()
            && (value.development_end() == 0 || value.development_end() >= 200000)))
        throw std::invalid_argument("sample requires full sample or a development boundary");
}

void validate_sample_input(const r::SampleInput& value)
{
    input_shape(value);
    validate_bar_dataset(value.dataset()); // Data content contract once at the input boundary.
}

r::SampleExecution sample_execution(const r::SampleInput& value)
{
    input_shape(value);
    r::SampleExecution result;
    *result.mutable_owner() = value.owner();
    *result.mutable_request() = value.request();
    return result;
}

void validate_sample_result(const r::SampleInput& input, const r::SampleResult& value)
{
    input_shape(input);
    validate_message(value);
    if (value.owner().SerializeAsString() != input.owner().SerializeAsString()
        || value.dataset_revision() != input.dataset().revision()
        || value.rows_size() != input.dataset().bars_size())
        throw std::invalid_argument("sample result differs from its fixed input");
    for (int i = 0; i < value.rows_size(); ++i) {
        const auto& row = value.rows(i);
        const auto& source = input.dataset().bars(i);
        const auto role = input.request().has_full_sample() ? r::SampleRow::FULL_SAMPLE
            : static_cast<unsigned>(i) < input.request().development_end()
            ? r::SampleRow::DEVELOPMENT
            : r::SampleRow::HOLDOUT;
        if (row.event_index() != static_cast<unsigned>(i)
            || row.observed_ns() != source.timestamp_ns()
            || row.trading_day() != source.trading_day() || row.role() != role)
            throw std::invalid_argument("sample result differs from its fixed input");
    }
}

r::SampleRequest encode_sample_request(const Json& json)
{
    require_fields(json, {"selection", "use_scope", "mode", "evaluation"});
    r::SampleRequest result;
    *result.mutable_selection() = encode_bar_dataset_request(json.at("selection"));
    result.set_use_scope(json.at("use_scope").get<std::string>());
    if (json.at("mode") != "post_hoc")
        throw std::invalid_argument("sample requires explicit post-hoc exploration");
    result.set_mode(r::SampleRequest::POST_HOC);
    const auto& evaluation = json.at("evaluation");
    if (evaluation.at("mode") == "full_sample") {
        require_fields(evaluation, {"mode"});
        result.set_full_sample(true);
    } else if (evaluation.at("mode") == "holdout") {
        require_fields(evaluation, {"mode", "split_index"});
        const auto& split = evaluation.at("split_index");
        if (!split.is_number_integer() || split < 1 || split >= 200000)
            throw std::invalid_argument("sample requires full sample or a development boundary");
        result.set_development_end(split.get<unsigned>());
    } else
        throw std::invalid_argument("sample requires full sample or a development boundary");
    validate_sample_request(result);
    return result;
}

Json decode_sample_input(const r::SampleInput& input)
{
    validate_sample_input(input);
    return {
        {"owner", owner(input.owner())},
        {"data_owner",
            {{"node_id", input.data_owner().node_id()},
                {"service_id", input.data_owner().service_id()}}},
        {"request", request(input.request())},
        {"dataset", decode_bar_dataset(input.dataset(), DatasetView::metadata)},
        {"historical_availability", "unknown"},
        {"complete_history", "unproven"}
    };
}

Json decode_sample_result(const r::SampleInput& input, const r::SampleResult& value)
{
    validate_sample_result(input, value);
    Json rows = Json::array();
    for (const auto& row : value.rows()) {
        const auto& source = input.dataset().bars(static_cast<int>(row.event_index()));
        const bool held = row.role() == r::SampleRow::HOLDOUT;
        rows.push_back(
            {{"event_index", row.event_index()},
                {"observed",
                    input.dataset().interval_minutes() == 0
                        ? row.trading_day()
                        : format_utc_nanoseconds(row.observed_ns())},
                {"observed_ns", std::to_string(row.observed_ns())},
                {"trading_day", row.trading_day()},
                {"role",
                    row.role() == r::SampleRow::FULL_SAMPLE ? "full_sample"
                        : held                              ? "holdout"
                                                            : "development"},
                {"value",
                    held ? Json(nullptr) : Json(Decimal::from_raw(source.close().units()).str())}});
    }
    return {
        {"owner", owner(value.owner())},
        {"dataset_revision", value.dataset_revision()},
        {"price_power", 1},
        {"historical_availability", "unknown"},
        {"complete_history", "unproven"},
        {"rows", std::move(rows)}
    };
}

} // namespace asterion::protocol
