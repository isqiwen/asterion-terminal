#include <asterion/protocol/research_statistics.hpp>
#include <asterion/protocol/factor.hpp>
#include <asterion/protocol/research_sample.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/base/id.hpp>
#include <algorithm>
#include <map>
#include <asterion/futures/trading_days.hpp>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace asterion::protocol {
namespace {
namespace r = research::proto;

bool hash(const std::string& value)
{
    return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string::npos;
}

void source_identity(const resource::proto::Object& value)
{
    validate_message(value);
    validate_id(value.id());
    validate_id(value.owner().node_id());
    validate_id(value.owner().service_id());
    if (value.owner().domain() != "research"
        || (value.kind() != "factor_result" && value.kind() != "sample_result")
        || !hash(value.version()) || value.id().find_first_of("/\\") != std::string::npos
        || value.id() == "." || value.id() == "..")
        throw std::invalid_argument("invalid statistics source identity");
}

Json object(const resource::proto::Object& value)
{
    return {
        {"node", value.owner().node_id()},
        {"service", value.owner().service_id()},
        {"domain", value.owner().domain()},
        {"kind", value.kind()},
        {"id", value.id()},
        {"version", value.version()}
    };
}

Json owner(const resource::proto::Owner& value)
{
    return {
        {"node_id", value.node_id()},
        {"service_id", value.service_id()},
        {"domain", value.domain()}
    };
}

[[noreturn]] void reject_result()
{
    throw std::invalid_argument("statistics result does not match its fixed input");
}

void value_shape(const r::StatisticValue& value)
{
    if (value.state() < r::StatisticValue::DEFINED || value.state() > r::StatisticValue::NON_FINITE
        || value.has_value() != (value.state() == r::StatisticValue::DEFINED)
        || (value.has_value() && !std::isfinite(value.value())))
        reject_result();
}

Json number(const r::StatisticValue& value)
{
    return {
        {"state", r::StatisticValue::State_Name(value.state())},
        {"value", value.has_value() ? Json(value.value()) : Json(nullptr)}
    };
}

void summary_values(
    const r::StatisticsSummary& summary,
    unsigned valid,
    double minimum,
    double maximum)
{
    for (const auto* value :
        {&summary.mean(),
            &summary.standard_deviation(),
            &summary.minimum(),
            &summary.q25(),
            &summary.median(),
            &summary.q75(),
            &summary.maximum()})
        value_shape(*value);
    for (const auto* value :
        {&summary.minimum(), &summary.q25(), &summary.median(), &summary.q75(), &summary.maximum()})
        if (value->state()
            != (valid ? r::StatisticValue::DEFINED : r::StatisticValue::NO_VALID_SAMPLES))
            reject_result();
    if (!valid) {
        if (summary.mean().state() != r::StatisticValue::NO_VALID_SAMPLES
            || summary.standard_deviation().state() != r::StatisticValue::NO_VALID_SAMPLES)
            reject_result();
        return;
    }
    if (summary.minimum().value() != minimum || summary.maximum().value() != maximum
        || summary.q25().value() < minimum || summary.q25().value() > summary.median().value()
        || summary.median().value() > summary.q75().value() || summary.q75().value() > maximum
        || (summary.mean().state() != r::StatisticValue::DEFINED
            && summary.mean().state() != r::StatisticValue::NON_FINITE)
        || (summary.mean().has_value()
            && (summary.mean().value() < minimum || summary.mean().value() > maximum)))
        reject_result();
    const auto sd = summary.standard_deviation().state();
    if (valid == 1 ? sd != r::StatisticValue::INSUFFICIENT_SAMPLES
                   : sd != r::StatisticValue::DEFINED && sd != r::StatisticValue::NON_FINITE)
        reject_result();
    if (summary.standard_deviation().has_value() && summary.standard_deviation().value() < 0)
        reject_result();
}

Json summary_json(const r::StatisticsSummary& s)
{
    return {
        {"total", s.total()},
        {"allowed", s.allowed()},
        {"valid", s.valid()},
        {"missing", s.missing()},
        {"mean", number(s.mean())},
        {"standard_deviation", number(s.standard_deviation())},
        {"minimum", number(s.minimum())},
        {"q25", number(s.q25())},
        {"median", number(s.median())},
        {"q75", number(s.q75())},
        {"maximum", number(s.maximum())}
    };
}

Json request(const r::StatisticsRequest& value)
{
    return {
        {"source", object(value.source())},
        {"use_scope", value.use_scope()},
        {"column",
            value.column() == r::StatisticsRequest::RAW_CLOSE     ? "raw_close"
                : value.column() == r::StatisticsRequest::FEATURE ? "feature"
                                                                  : "forward_return"},
        {"role", value.role() == r::StatisticsRequest::FULL_SAMPLE ? "full_sample" : "development"},
        {"begin_index", value.begin_index()},
        {"end_index", value.end_index()},
        {"bins", value.bins()},
        {"grouping", value.grouping() == r::StatisticsRequest::TRADING_DAY ? "trading_day" : "none"}
    };
}

} // namespace

void validate_statistics_request(const research::proto::StatisticsRequest& value)
{
    validate_message(value);
    source_identity(value.source());
    if (value.use_scope().find_first_not_of(" \t\r\n") == std::string::npos
        || value.use_scope().size() > 4096 || value.use_scope().find('\0') != std::string::npos
        || (value.source().kind() == "sample_result"
                ? value.column() != r::StatisticsRequest::RAW_CLOSE
                : value.column() != r::StatisticsRequest::FEATURE
                    && value.column() != r::StatisticsRequest::FORWARD_RETURN)
        || (value.role() != r::StatisticsRequest::FULL_SAMPLE
            && value.role() != r::StatisticsRequest::DEVELOPMENT)
        || value.begin_index() >= value.end_index() || value.end_index() > 200000 || !value.bins()
        || value.bins() > 64
        || (value.grouping() != r::StatisticsRequest::NONE
            && value.grouping() != r::StatisticsRequest::TRADING_DAY)
        || (value.grouping() == r::StatisticsRequest::TRADING_DAY
            && value.source().kind() != "sample_result"))
        throw std::invalid_argument("invalid statistics request");
}

void validate_statistics_input(const research::proto::StatisticsInput& value)
{
    validate_message(value);
    validate_statistics_request(value.request());
    const auto& source = value.source();
    source_identity(source.result());
    const auto& expected = value.request().source();
    const bool full = source.has_full_sample() && source.full_sample();
    const bool raw = source.result().kind() == "sample_result";
    if (value.owner().SerializeAsString() != expected.owner().SerializeAsString()
        || source.result().SerializeAsString() != expected.SerializeAsString()
        || !hash(source.input_sha256()) || !hash(source.program_artifact()) || !source.input_bytes()
        || !source.attempt() || !source.observations() || source.observations() > 200000
        || source.price_power() < -8 || source.price_power() > 8
        || (raw ? source.required_history() != 0 || source.horizon() != 0
                    || source.price_power() != 1
                : source.required_history() >= source.observations() || !source.horizon()
                    || source.horizon() >= source.observations())
        || value.request().end_index() > source.observations()
        || (full ? value.request().role() != r::StatisticsRequest::FULL_SAMPLE
                 : !source.has_holdout_start() || !source.holdout_start()
                    || source.holdout_start() >= source.observations()
                    || value.request().role() != r::StatisticsRequest::DEVELOPMENT))
        throw std::invalid_argument("invalid fixed statistics input");
}

void validate_statistics_source(
    const research::proto::StatisticsInput& value,
    const factor::proto::FactorInput& input,
    const factor::proto::FactorResult& result)
{
    validate_statistics_input(value);
    validate_factor_result(input, result);
    if (value.source().result().kind() != "factor_result" || input.series_size() != 1
        || input.lookbacks_size() != 1 || input.has_walk_forward() || !input.retain_observations()
        || result.selection_rule() != "fixed"
        || result.observations_size() != static_cast<int>(result.input_count()))
        throw std::invalid_argument(
            "statistics require fixed-window retained single-series factor evidence");
    const auto& source = value.source();
    const auto power =
        input.has_variable_definition() ? input.variable_definition().price_power() : 0;
    if (source.observations() != result.input_count()
        || source.required_history() != result.evaluation_warmup()
        || source.horizon() != input.horizon() || source.price_power() != power
        || source.has_full_sample() != input.has_full_sample()
        || (source.has_full_sample() && source.full_sample() != input.full_sample())
        || source.has_holdout_start() != input.has_holdout_start()
        || (source.has_holdout_start() && source.holdout_start() != input.holdout_start()))
        throw std::invalid_argument(
            "statistics source evidence does not match its captured identity");
}

void validate_statistics_source(
    const research::proto::StatisticsInput& value,
    const research::proto::SampleInput& input,
    const research::proto::SampleResult& result)
{
    validate_statistics_input(value);
    validate_sample_input(input);
    validate_sample_result(input, result);
    const auto& source = value.source();
    if (source.result().kind() != "sample_result"
        || value.owner().SerializeAsString() != input.owner().SerializeAsString()
        || source.observations() != static_cast<unsigned>(result.rows_size())
        || source.required_history() != 0 || source.horizon() != 0 || source.price_power() != 1
        || source.has_full_sample() != input.request().has_full_sample()
        || (source.has_full_sample() && source.full_sample() != input.request().full_sample())
        || source.has_holdout_start() != input.request().has_development_end()
        || (source.has_holdout_start()
            && source.holdout_start() != input.request().development_end()))
        throw std::invalid_argument(
            "statistics source evidence does not match its captured identity");
}

void validate_statistics_result(
    const research::proto::StatisticsInput& input,
    const research::proto::StatisticsResult& result)
{
    validate_statistics_input(input);
    validate_message(result);
    const auto& request = input.request();
    const auto& source = input.source();
    const auto& summary = result.summary();
    if (result.owner().SerializeAsString() != input.owner().SerializeAsString()
        || result.source().SerializeAsString() != request.source().SerializeAsString()
        || result.price_power()
            != (request.column() == r::StatisticsRequest::FORWARD_RETURN ? 0 : source.price_power())
        || result.rows_size() != static_cast<int>(request.end_index() - request.begin_index())
        || result.reasons_size() != 7 || result.bins_size() > static_cast<int>(request.bins()))
        reject_result();
    std::array<unsigned, 7> counts{};
    std::array<unsigned, 64> bins{};
    unsigned allowed = 0, valid = 0;
    double minimum = std::numeric_limits<double>::max(),
           maximum = std::numeric_limits<double>::lowest();
    for (int n = 0; n < result.rows_size(); ++n) {
        const auto& row = result.rows(n);
        const unsigned i = request.begin_index() + static_cast<unsigned>(n);
        if (row.event_index() != i || row.observed().empty() || row.observed().size() > 64
            || row.reason() < r::StatisticsRow::VALID
            || row.reason() > r::StatisticsRow::OTHER_ROLE)
            reject_result();
        const bool outside = source.has_holdout_start() && i >= source.holdout_start();
        if (outside ? row.reason() != r::StatisticsRow::OTHER_ROLE
                    : row.reason() == r::StatisticsRow::OTHER_ROLE)
            reject_result();
        if (!outside) {
            ++allowed;
            if (request.column() == r::StatisticsRequest::RAW_CLOSE) {
                if (row.reason() != r::StatisticsRow::VALID)
                    reject_result();
            } else if (request.column() == r::StatisticsRequest::FEATURE) {
                if (i < source.required_history() ? row.reason() != r::StatisticsRow::WARMUP
                                                  : row.reason() != r::StatisticsRow::VALID
                            && row.reason() != r::StatisticsRow::ZERO_DENOMINATOR
                            && row.reason() != r::StatisticsRow::NON_FINITE)
                    reject_result();
            } else {
                const bool label = i + source.horizon() < source.observations();
                const bool crossed = source.has_holdout_start() && label
                    && i + source.horizon() >= source.holdout_start();
                const auto expected = crossed ? r::StatisticsRow::CROSSED_LABEL
                    : label                   ? r::StatisticsRow::VALID
                                              : r::StatisticsRow::UNKNOWN_LABEL;
                if (row.reason() != expected)
                    reject_result();
            }
        }
        const bool present = row.reason() == r::StatisticsRow::VALID;
        if (row.has_value() != present || row.has_bin() != present)
            reject_result();
        if (present) {
            if (!std::isfinite(row.value())
                || row.bin() >= static_cast<unsigned>(result.bins_size()))
                reject_result();
            const auto& bin = result.bins(static_cast<int>(row.bin()));
            if (row.value() < bin.lower() || row.value() > bin.upper()
                || (row.bin() + 1 < static_cast<unsigned>(result.bins_size())
                    && row.value() == bin.upper()))
                reject_result();
            ++valid;
            ++bins[row.bin()];
            minimum = std::min(minimum, row.value());
            maximum = std::max(maximum, row.value());
        }
        ++counts[static_cast<std::size_t>(row.reason()) - 1];
    }
    if (summary.total() != static_cast<unsigned>(result.rows_size()) || summary.allowed() != allowed
        || summary.valid() != valid || summary.missing() != allowed - valid)
        reject_result();
    for (int i = 0; i < 7; ++i)
        if (result.reasons(i).reason() != i + 1
            || result.reasons(i).count() != counts[static_cast<std::size_t>(i)])
            reject_result();
    summary_values(summary, valid, minimum, maximum);
    if (!valid) {
        if (result.bins_size())
            reject_result();
    } else if (result.bins_size() != (minimum == maximum ? 1 : static_cast<int>(request.bins()))
        || result.bins(0).lower() != minimum
        || result.bins(result.bins_size() - 1).upper() != maximum)
        reject_result();
    for (int i = 0; i < result.bins_size(); ++i) {
        const auto& bin = result.bins(i);
        if (!std::isfinite(bin.lower()) || !std::isfinite(bin.upper()) || bin.lower() > bin.upper()
            || (i && bin.lower() != result.bins(i - 1).upper())
            || bin.count() != bins[static_cast<std::size_t>(i)])
            reject_result();
    }
    if (request.grouping() == r::StatisticsRequest::NONE) {
        if (result.groups_size())
            reject_result();
        return;
    }
    std::map<std::string, std::vector<const r::StatisticsRow*>> members;
    for (const auto& row : result.rows()) {
        (void)parse_trading_date(row.trading_day());
        members[row.trading_day()].push_back(&row);
    }
    if (members.size() != static_cast<std::size_t>(result.groups_size()))
        reject_result();
    int group_index = 0;
    for (const auto& [day, rows] : members) {
        const auto& group = result.groups(group_index++);
        if (group.trading_day() != day || group.reasons_size() != 7
            || group.bin_counts_size() != result.bins_size())
            reject_result();
        unsigned group_allowed = 0, group_valid = 0;
        std::array<unsigned, 7> reasons{};
        std::array<unsigned, 64> bin_counts{};
        double low = std::numeric_limits<double>::max(),
               high = std::numeric_limits<double>::lowest();
        for (const auto* row : rows) {
            ++reasons[static_cast<std::size_t>(row->reason()) - 1];
            if (row->reason() != r::StatisticsRow::OTHER_ROLE)
                ++group_allowed;
            if (row->has_value()) {
                ++group_valid;
                ++bin_counts[row->bin()];
                low = std::min(low, row->value());
                high = std::max(high, row->value());
            }
        }
        const auto& summary = group.summary();
        if (summary.total() != rows.size() || summary.allowed() != group_allowed
            || summary.valid() != group_valid || summary.missing() != group_allowed - group_valid)
            reject_result();
        for (int i = 0; i < 7; ++i)
            if (group.reasons(i).reason() != i + 1
                || group.reasons(i).count() != reasons[static_cast<std::size_t>(i)])
                reject_result();
        for (int i = 0; i < group.bin_counts_size(); ++i)
            if (group.bin_counts(i) != bin_counts[static_cast<std::size_t>(i)])
                reject_result();
        summary_values(summary, group_valid, low, high);
    }
}

research::proto::StatisticsRequest encode_statistics_request(const Json& value)
{
    auto fields = value;
    fields.erase("grouping");
    require_fields(
        fields,
        {"source", "use_scope", "column", "role", "begin_index", "end_index", "bins"});
    const auto& object = value.at("source");
    require_fields(object, {"node", "service", "domain", "kind", "id", "version"});
    r::StatisticsRequest result;
    auto& source = *result.mutable_source();
    source.mutable_owner()->set_node_id(object.at("node").get<std::string>());
    source.mutable_owner()->set_service_id(object.at("service").get<std::string>());
    source.mutable_owner()->set_domain(object.at("domain").get<std::string>());
    source.set_kind(object.at("kind").get<std::string>());
    source.set_id(object.at("id").get<std::string>());
    source.set_version(object.at("version").get<std::string>());
    const auto integer = [&](const char* key, unsigned max) {
        const auto& n = value.at(key);
        if (!n.is_number_integer() || n < 0 || n > max)
            throw std::invalid_argument("invalid statistics request");
        return n.get<unsigned>();
    };
    result.set_use_scope(value.at("use_scope").get<std::string>());
    const auto column = value.at("column").get<std::string>();
    const auto role = value.at("role").get<std::string>();
    if (column != "feature" && column != "forward_return" && column != "raw_close")
        throw std::invalid_argument("invalid statistics request");
    if (role != "full_sample" && role != "development")
        throw std::invalid_argument("invalid statistics request");
    result.set_column(
        column == "raw_close"     ? r::StatisticsRequest::RAW_CLOSE
            : column == "feature" ? r::StatisticsRequest::FEATURE
                                  : r::StatisticsRequest::FORWARD_RETURN);
    result.set_role(
        role == "full_sample" ? r::StatisticsRequest::FULL_SAMPLE
                              : r::StatisticsRequest::DEVELOPMENT);
    result.set_begin_index(integer("begin_index", 200000));
    result.set_end_index(integer("end_index", 200000));
    result.set_bins(integer("bins", 64));
    const auto grouping = value.value("grouping", std::string("none"));
    if (grouping != "none" && grouping != "trading_day")
        throw std::invalid_argument("invalid statistics request");
    result.set_grouping(
        grouping == "trading_day" ? r::StatisticsRequest::TRADING_DAY : r::StatisticsRequest::NONE);
    validate_statistics_request(result);
    return result;
}

Json decode_statistics_input(const research::proto::StatisticsInput& value)
{
    validate_statistics_input(value);
    const auto& source = value.source();
    Json captured{
        {"result", object(source.result())},
        {"input_sha256", source.input_sha256()},
        {"input_bytes", std::to_string(source.input_bytes())},
        {"attempt", source.attempt()},
        {"program_artifact", source.program_artifact()},
        {"observations", source.observations()},
        {"price_power", source.price_power()},
        {"required_history", source.required_history()},
        {"horizon", source.horizon()}
    };
    captured["evaluation"] = source.has_full_sample()
        ? Json{{"mode", "full_sample"}}
        : Json{{"mode", "holdout"}, {"split_index", source.holdout_start()}};
    return {
        {"owner", owner(value.owner())},
        {"request", request(value.request())},
        {"source", std::move(captured)}
    };
}

Json decode_statistics_result(
    const research::proto::StatisticsInput& input,
    const research::proto::StatisticsResult& result)
{
    validate_statistics_result(input, result);
    Json reasons = Json::array(), bins = Json::array(), rows = Json::array(),
         groups = Json::array();
    for (const auto& value : result.reasons())
        reasons.push_back(
            {{"reason", r::StatisticsRow::Reason_Name(value.reason())}, {"count", value.count()}});
    for (const auto& bin : result.bins())
        bins.push_back({{"lower", bin.lower()}, {"upper", bin.upper()}, {"count", bin.count()}});
    for (const auto& row : result.rows())
        rows.push_back(
            {{"event_index", row.event_index()},
                {"observed", row.observed()},
                {"trading_day", row.trading_day()},
                {"value", row.has_value() ? Json(row.value()) : Json(nullptr)},
                {"reason", r::StatisticsRow::Reason_Name(row.reason())},
                {"bin", row.has_bin() ? Json(row.bin()) : Json(nullptr)}});
    for (const auto& group : result.groups()) {
        Json reasons = Json::array();
        for (const auto& item : group.reasons())
            reasons.push_back(
                {{"reason", r::StatisticsRow::Reason_Name(item.reason())},
                    {"count", item.count()}});
        groups.push_back(
            {{"trading_day", group.trading_day()},
                {"summary", summary_json(group.summary())},
                {"reasons", std::move(reasons)},
                {"bin_counts",
                    std::vector<unsigned>(group.bin_counts().begin(), group.bin_counts().end())}});
    }
    return {
        {"owner", owner(result.owner())},
        {"source", object(result.source())},
        {"price_power", result.price_power()},
        {"summary", summary_json(result.summary())},
        {"reasons", std::move(reasons)},
        {"bins", std::move(bins)},
        {"rows", std::move(rows)},
        {"groups", std::move(groups)}
    };
}

} // namespace asterion::protocol
