#include <asterion/protocol/research_variable.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/foundation/id.hpp>
#include <asterion/kernel/process/artifact.hpp>

namespace asterion::protocol {
namespace {

bool hash(const std::string& s)
{
    return s.size() == 64 && s.find_first_not_of("0123456789abcdef") == std::string::npos;
}

} // namespace

void validate_variable_reference(const resource::proto::Object& object)
{
    validate_message(object);
    validate_id(object.id());
    validate_id(object.owner().node_id());
    validate_id(object.owner().service_id());
    if (object.owner().domain() != "research" || object.kind() != "research_variable"
        || !hash(object.version()))
        throw std::invalid_argument("invalid research variable identity");
}

std::string variable_digest(research::proto::VariableDefinition value)
{
    value.clear_digest();
    return sha256_bytes(value.SerializeAsString());
}

void validate_variable_definition(const research::proto::VariableDefinition& value)
{
    validate_message(value);
    validate_id(value.id());
    validate_id(value.owner().node_id());
    validate_id(value.owner().service_id());
    if (value.owner().domain() != "research" || value.name().empty() || value.name().size() > 256
        || value.purpose().empty() || value.purpose().size() > 4096 || value.expression().empty()
        || value.expression().size() > 8192 || !value.window() || value.window() > 10000
        || value.price_power() < -8 || value.price_power() > 8 || value.required_history() > 1000000
        || value.recorded_at_ms() < 0 || value.ByteSizeLong() > 32768
        || value.digest() != variable_digest(value))
        throw std::invalid_argument("invalid research variable definition");
}

resource::proto::Object variable_object(const research::proto::VariableDefinition& value)
{
    validate_variable_definition(value);
    resource::proto::Object object;
    *object.mutable_owner() = value.owner();
    object.set_kind("research_variable");
    object.set_id(value.id());
    object.set_version(value.digest());
    return object;
}

void verify_variable_reference(
    const resource::proto::Object& object,
    const research::proto::VariableDefinition& value)
{
    validate_variable_reference(object);
    if (object.SerializeAsString() != variable_object(value).SerializeAsString())
        throw Error(ErrorCode::conflict, "research variable definition mismatch");
}

Json decode_variable_reference(const resource::proto::Object& object)
{
    validate_variable_reference(object);
    return {
        {"node", object.owner().node_id()},
        {"service", object.owner().service_id()},
        {"domain", object.owner().domain()},
        {"kind", object.kind()},
        {"id", object.id()},
        {"version", object.version()}
    };
}

resource::proto::Object encode_variable_reference(const Json& value)
{
    require_fields(value, {"node", "service", "domain", "kind", "id", "version"});
    resource::proto::Object result;
    result.mutable_owner()->set_node_id(value.at("node").get<std::string>());
    result.mutable_owner()->set_service_id(value.at("service").get<std::string>());
    result.mutable_owner()->set_domain(value.at("domain").get<std::string>());
    result.set_kind(value.at("kind").get<std::string>());
    result.set_id(value.at("id").get<std::string>());
    result.set_version(value.at("version").get<std::string>());
    validate_variable_reference(result);
    return result;
}

Json decode_variable_definition(const research::proto::VariableDefinition& value)
{
    validate_variable_definition(value);
    return {
        {"object", decode_variable_reference(variable_object(value))},
        {"name", value.name()},
        {"purpose", value.purpose()},
        {"expression", value.expression()},
        {"window", value.window()},
        {"price_power", value.price_power()},
        {"required_history", value.required_history()},
        {"recorded_at_ms", std::to_string(value.recorded_at_ms())}
    };
}

Json decode_variable_reply(
    const research::proto::VariableReply& reply,
    const research::proto::VariableRequest& request,
    const resource::proto::Owner& owner)
{
    validate_message(reply);
    validate_message(request);
    const auto reject = [] {
        throw Error(ErrorCode::unavailable, "invalid research variable response");
    };
    const auto decode = [&](const research::proto::VariableDefinition& value) {
        if (value.owner().SerializeAsString() != owner.SerializeAsString())
            throw Error(ErrorCode::conflict, "research variable owner mismatch");
        return decode_variable_definition(value);
    };
    const bool wanted = request.has_get() || request.has_mutation();
    if (reply.has_definition() != wanted || reply.has_preview() != request.has_preview())
        reject();
    Json result = {{"definitions", Json::array()}, {"next_after", reply.next_after()}};
    if (wanted) {
        if (reply.definitions_size() || !reply.next_after().empty())
            reject();
        const auto id =
            request.has_get() ? request.get().id() : request.mutation().registration().id();
        if (reply.definition().id() != id)
            reject();
        if (request.has_mutation()) {
            const auto& input = request.mutation().registration();
            const auto& value = reply.definition();
            if (value.name() != input.name() || value.purpose() != input.purpose()
                || value.expression() != input.expression() || value.window() != input.window())
                reject();
        }
        result["definition"] = decode(reply.definition());
    } else if (request.has_list()) {
        const auto& query = request.list();
        if (!query.limit() || query.limit() > 200
            || reply.definitions_size() > static_cast<int>(query.limit()))
            reject();
        auto previous = query.after();
        for (const auto& row : reply.definitions()) {
            if (row.id() <= previous)
                reject();
            result["definitions"].push_back(decode(row));
            previous = row.id();
        }
        if (!reply.next_after().empty()
            && (reply.definitions_size() != static_cast<int>(query.limit())
                || reply.next_after() != previous))
            reject();
    } else if (request.has_preview()) {
        const auto& preview = reply.preview();
        if (reply.definitions_size() || !reply.next_after().empty() || preview.price_power() < -8
            || preview.price_power() > 8 || preview.required_history() > 1000000
            || (preview.valid() ? preview.has_error_offset() || !preview.diagnostic().empty()
                                : !preview.has_error_offset() || preview.error_offset() > 8192
                        || preview.diagnostic().empty()))
            reject();
        Json value = {
            {"valid", preview.valid()},
            {"price_power", preview.price_power()},
            {"required_history", preview.required_history()},
            {"diagnostic", preview.diagnostic()}
        };
        value["error_offset"] =
            preview.has_error_offset() ? Json(preview.error_offset()) : Json(nullptr);
        result["preview"] = std::move(value);
    } else
        reject();
    return result;
}

} // namespace asterion::protocol
