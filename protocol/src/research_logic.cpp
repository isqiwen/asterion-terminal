#include <asterion/protocol/research_logic.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/base/id.hpp>
#include <asterion/base/decimal.hpp>
#include <asterion/system/process/artifact.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>

namespace asterion::protocol {
namespace {
namespace r = research::proto;

bool hash(const std::string& value)
{
    return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string::npos;
}

bool text(const std::string& value, std::size_t limit)
{
    return !value.empty() && value.size() <= limit && value.find(char(0)) == std::string::npos
        && !std::ranges::all_of(value, [](unsigned char c) {
               return std::isspace(c);
           });
}

void owner(const resource::proto::Owner& value)
{
    validate_message(value);
    validate_id(value.node_id());
    validate_id(value.service_id());
    if (value.domain() != "research")
        throw std::invalid_argument("research logic owner required");
}

resource::proto::Object object(
    const resource::proto::Owner& who,
    const std::string& id,
    const std::string& version,
    const char* kind)
{
    resource::proto::Object result;
    *result.mutable_owner() = who;
    result.set_id(id);
    result.set_version(version);
    result.set_kind(kind);
    return result;
}

} // namespace

void validate_logic_object(const resource::proto::Object& value, std::string_view kind)
{
    validate_message(value);
    owner(value.owner());
    validate_id(value.id());
    if (value.kind() != kind || !hash(value.version()))
        throw std::invalid_argument("invalid research logic identity");
}

std::string logic_digest(const r::LogicDefinition& value)
{
    r::LogicDefinition semantic;
    semantic.set_fast(value.fast());
    semantic.set_slow(value.slow());
    semantic.set_input(value.input());
    semantic.set_output(value.output());
    return sha256_bytes(semantic.SerializeAsString());
}

std::string logic_example_digest(r::LogicExample value)
{
    value.clear_id();
    value.clear_owner();
    value.clear_name();
    value.clear_use_scope();
    value.clear_digest();
    value.clear_recorded_at_ms();
    return sha256_bytes(value.SerializeAsString());
}

void validate_logic_definition(const r::LogicDefinition& value)
{
    validate_message(value);
    owner(value.owner());
    validate_id(value.id());
    if (!text(value.name(), 256) || !text(value.purpose(), 4096) || !value.fast()
        || value.fast() >= value.slow() || value.slow() > 10000
        || value.input() != r::LogicDefinition::COMPLETED_CLOSE
        || value.output() != r::LogicDefinition::TARGET_SIGN || value.recorded_at_ms() < 0
        || value.ByteSizeLong() > 16384
        || (!value.retirement_reason().empty() && !text(value.retirement_reason(), 4096))
        || value.digest() != logic_digest(value))
        throw std::invalid_argument("invalid research logic definition");
}

void validate_logic_example(const r::LogicExample& value)
{
    validate_message(value);
    owner(value.owner());
    validate_id(value.id());
    validate_logic_object(value.definition(), "logic_definition");
    if (value.definition().owner().SerializeAsString() != value.owner().SerializeAsString()
        || !text(value.name(), 256) || !text(value.use_scope(), 4096) || !value.fictional()
        || value.observations_size() < 1 || value.observations_size() > 512
        || value.recorded_at_ms() < 0 || value.ByteSizeLong() > 131072
        || value.digest() != logic_example_digest(value))
        throw std::invalid_argument("invalid fictional logic example");
    std::int64_t previous = 0;
    for (int i = 0; i < value.observations_size(); ++i) {
        const auto& row = value.observations(i);
        const auto reject = [&](const char* reason) {
            throw std::invalid_argument(
                "logic example observation " + std::to_string(i) + ": " + reason);
        };
        if (!row.has_close())
            reject("close must be explicitly present");
        if (row.closed_at_ns() <= previous)
            reject("complete boundaries must strictly increase");
        if (row.available_at_ns() <= 0 || row.available_at_ns() > row.closed_at_ns())
            reject("input availability must precede or equal its decision boundary");
        if (row.has_warmup()
                ? !row.warmup()
                : !row.has_target_sign() || row.target_sign() < -1 || row.target_sign() > 1)
            reject("expected output must explicitly be warmup or target sign -1/0/+1");
        previous = row.closed_at_ns();
    }
}

resource::proto::Object logic_object(const r::LogicDefinition& value)
{
    validate_logic_definition(value);
    return object(value.owner(), value.id(), value.digest(), "logic_definition");
}

resource::proto::Object logic_object(const r::LogicExample& value)
{
    validate_logic_example(value);
    return object(value.owner(), value.id(), value.digest(), "logic_case");
}

void validate_logic_trial_input(const r::LogicTrialInput& value)
{
    validate_message(value);
    owner(value.owner());
    validate_logic_definition(value.definition());
    validate_logic_example(value.example());
    if (value.owner().SerializeAsString() != value.definition().owner().SerializeAsString()
        || value.owner().SerializeAsString() != value.example().owner().SerializeAsString()
        || logic_object(value.definition()).SerializeAsString()
            != value.example().definition().SerializeAsString()
        || value.ByteSizeLong() > 262144)
        throw std::invalid_argument("logic trial fixed input mismatch");
}

void validate_logic_trial_result(const r::LogicTrialInput& input, const r::LogicTrialResult& result)
{
    validate_logic_trial_input(input);
    validate_message(result);
    if (result.rows_size() != input.example().observations_size())
        throw std::invalid_argument("logic trial result row count mismatch");
    unsigned matched = 0, decisions = 0;
    for (int i = 0; i < result.rows_size(); ++i) {
        const auto& row = result.rows(i);
        const auto& expected = input.example().observations(i);
        const bool warmed = static_cast<unsigned>(i + 1) >= input.definition().slow();
        if (row.index() != static_cast<unsigned>(i) || row.has_target_sign() != warmed
            || row.has_warmup() == warmed || (row.has_warmup() && !row.warmup())
            || (warmed && (row.target_sign() < -1 || row.target_sign() > 1))
            || row.has_fast_mean() != (static_cast<unsigned>(i + 1) >= input.definition().fast())
            || row.has_slow_mean() != warmed || row.has_difference() != warmed
            || (row.has_fast_mean() && !std::isfinite(row.fast_mean()))
            || (row.has_slow_mean() && !std::isfinite(row.slow_mean()))
            || (warmed
                && (!std::isfinite(row.difference())
                    || row.difference() != row.fast_mean() - row.slow_mean())))
            throw std::invalid_argument("invalid logic trial observation evidence");
        const bool match = row.has_warmup()
            ? expected.has_warmup()
            : expected.has_target_sign() && row.target_sign() == expected.target_sign();
        if (row.matches() != match)
            throw std::invalid_argument("logic trial expectation comparison mismatch");
        matched += match;
        decisions += warmed;
    }
    if (result.matched() != matched || result.decisions() != decisions
        || result.all_match() != (matched == static_cast<unsigned>(result.rows_size())))
        throw std::invalid_argument("logic trial result summary mismatch");
}

resource::proto::Object encode_logic_object(const Json& value, std::string_view kind)
{
    require_fields(value, {"node", "service", "domain", "kind", "id", "version"});
    resource::proto::Object result;
    result.mutable_owner()->set_node_id(value.at("node").get<std::string>());
    result.mutable_owner()->set_service_id(value.at("service").get<std::string>());
    result.mutable_owner()->set_domain(value.at("domain").get<std::string>());
    result.set_kind(value.at("kind").get<std::string>());
    result.set_id(value.at("id").get<std::string>());
    result.set_version(value.at("version").get<std::string>());
    validate_logic_object(result, kind);
    return result;
}

Json decode_logic_object(const resource::proto::Object& value)
{
    validate_logic_object(value, value.kind());
    return {
        {"node", value.owner().node_id()},
        {"service", value.owner().service_id()},
        {"domain", value.owner().domain()},
        {"kind", value.kind()},
        {"id", value.id()},
        {"version", value.version()}
    };
}

Json decode_logic_definition(const r::LogicDefinition& value)
{
    validate_logic_definition(value);
    return {
        {"object", decode_logic_object(logic_object(value))},
        {"name", value.name()},
        {"purpose", value.purpose()},
        {"fast", value.fast()},
        {"slow", value.slow()},
        {"input", "completed_close"},
        {"output", "target_sign"},
        {"recorded_at_ms", std::to_string(value.recorded_at_ms())},
        {"retirement_reason", value.retirement_reason()}
    };
}

Json decode_logic_example(const r::LogicExample& value)
{
    validate_logic_example(value);
    Json rows = Json::array();
    for (const auto& row : value.observations())
        rows.push_back(
            {{"closed_at_ns", std::to_string(row.closed_at_ns())},
                {"available_at_ns", std::to_string(row.available_at_ns())},
                {"close", Decimal::from_raw(row.close().units()).str()},
                {"expected",
                    row.has_warmup()
                        ? Json{{"state", "warmup"}}
                        : Json{{"state", "target_sign"}, {"value", row.target_sign()}}}});
    return {
        {"object", decode_logic_object(logic_object(value))},
        {"definition", decode_logic_object(value.definition())},
        {"name", value.name()},
        {"use_scope", value.use_scope()},
        {"fictional", true},
        {"observations", std::move(rows)},
        {"recorded_at_ms", std::to_string(value.recorded_at_ms())}
    };
}

Json decode_logic_trial_input(const r::LogicTrialInput& value)
{
    validate_logic_trial_input(value);
    return {
        {"definition", decode_logic_definition(value.definition())},
        {"example", decode_logic_example(value.example())}
    };
}

Json decode_logic_trial_result(const r::LogicTrialInput& input, const r::LogicTrialResult& result)
{
    validate_logic_trial_result(input, result);
    Json rows = Json::array();
    for (const auto& row : result.rows())
        rows.push_back(
            {{"index", row.index()},
                {"actual",
                    row.has_warmup()
                        ? Json{{"state", "warmup"}}
                        : Json{{"state", "target_sign"}, {"value", row.target_sign()}}},
                {"fast_mean", row.has_fast_mean() ? Json(row.fast_mean()) : Json(nullptr)},
                {"slow_mean", row.has_slow_mean() ? Json(row.slow_mean()) : Json(nullptr)},
                {"difference", row.has_difference() ? Json(row.difference()) : Json(nullptr)},
                {"matches", row.matches()}});
    return {
        {"rows", std::move(rows)},
        {"matched", result.matched()},
        {"decisions", result.decisions()},
        {"all_match", result.all_match()},
        {"fictional", true}
    };
}

Json decode_logic_reply(
    const r::LogicReply& reply,
    const r::LogicRequest& request,
    const resource::proto::Owner& expected_owner)
{
    validate_message(reply);
    owner(expected_owner);
    Json result = Json::object();
    const auto definition = [&](const r::LogicDefinition& value) {
        if (value.owner().SerializeAsString() != expected_owner.SerializeAsString())
            throw std::invalid_argument("logic definition response owner mismatch");
        return decode_logic_definition(value);
    };
    const auto example = [&](const r::LogicExample& value) {
        if (value.owner().SerializeAsString() != expected_owner.SerializeAsString())
            throw std::invalid_argument("logic example response owner mismatch");
        return decode_logic_example(value);
    };
    const bool list_definitions = request.has_list();
    const bool list_examples = request.has_examples();
    const bool single_example =
        request.has_example() || (request.has_mutation() && request.mutation().has_example());
    const bool single_definition = request.has_get()
        || (request.has_mutation()
            && (request.mutation().has_registration() || request.mutation().has_retire()));
    if (single_example) {
        if (!reply.has_example() || reply.has_definition() || reply.definitions_size()
            || reply.examples_size() || !reply.next_after().empty())
            throw std::invalid_argument("invalid logic example response");
        const auto id =
            request.has_example() ? request.example().id() : request.mutation().example().id();
        if (reply.example().id() != id)
            throw std::invalid_argument("logic example response identity mismatch");
        result["example"] = example(reply.example());
    } else if (single_definition) {
        if (!reply.has_definition() || reply.has_example() || reply.definitions_size()
            || reply.examples_size() || !reply.next_after().empty())
            throw std::invalid_argument("invalid logic definition response");
        const auto id = request.has_get()           ? request.get().id()
            : request.mutation().has_registration() ? request.mutation().registration().id()
                                                    : request.mutation().retire().definition().id();
        if (reply.definition().id() != id)
            throw std::invalid_argument("logic definition response identity mismatch");
        result["definition"] = definition(reply.definition());
    } else if (list_definitions || list_examples) {
        const auto& page = list_definitions ? request.list() : request.examples();
        if (page.limit() < 1 || page.limit() > 200 || reply.has_definition() || reply.has_example()
            || (list_definitions ? reply.examples_size() != 0 : reply.definitions_size() != 0))
            throw std::invalid_argument("invalid logic page response");
        Json rows = Json::array();
        std::string cursor = page.after();
        if (list_definitions)
            for (const auto& value : reply.definitions()) {
                if (value.id() <= cursor)
                    throw std::invalid_argument("invalid logic page order");
                cursor = value.id();
                rows.push_back(definition(value));
            }
        else
            for (const auto& value : reply.examples()) {
                if (value.id() <= cursor
                    || (!page.definition_id().empty()
                        && value.definition().id() != page.definition_id()))
                    throw std::invalid_argument("invalid logic example page order or definition");
                cursor = value.id();
                rows.push_back(example(value));
            }
        if (rows.size() > page.limit()
            || (!reply.next_after().empty()
                && (rows.size() != page.limit() || reply.next_after() != cursor)))
            throw std::invalid_argument("invalid logic page continuation");
        result[list_definitions ? "definitions" : "examples"] = std::move(rows);
        result["next_after"] = reply.next_after();
    } else
        throw std::invalid_argument("invalid research logic request");
    return result;
}

} // namespace asterion::protocol
