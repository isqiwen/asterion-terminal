#include <asterion/protocol/material.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/system/process/artifact.hpp>
#include <algorithm>

namespace asterion::protocol {
namespace {

void digest(std::string_view value)
{
    if (value.size() != 64 || value.find_first_not_of("0123456789abcdef") != std::string_view::npos)
        throw std::invalid_argument("invalid material content identity");
}

void text(std::string_view value, std::size_t maximum)
{
    if (value.empty() || value.size() > maximum
        || std::any_of(value.begin(), value.end(), [](unsigned char c) {
               return c < 32 || c == 127;
           }))
        throw std::invalid_argument("invalid material metadata");
}

void owner(const resource::proto::Owner& value)
{
    validate_id(value.node_id());
    validate_id(value.service_id());
    if (value.domain() != "data")
        throw std::invalid_argument("invalid material owner");
}

bool continuation(unsigned char c)
{
    return (c & 0xc0) == 0x80;
}

bool utf8(std::string_view bytes)
{
    for (std::size_t i = 0; i < bytes.size();) {
        const auto c = static_cast<unsigned char>(bytes[i++]);
        if (c < 0x80) {
            if (c == 0 || (c < 32 && c != 9 && c != 10 && c != 13) || c == 127)
                return false;
            continue;
        }
        const unsigned count = c >= 0xc2 && c <= 0xdf ? 1
            : c >= 0xe0 && c <= 0xef                  ? 2
            : c >= 0xf0 && c <= 0xf4                  ? 3
                                                      : 0;
        if (!count || count > bytes.size() - i)
            return false;
        const auto second = static_cast<unsigned char>(bytes[i]);
        if ((c == 0xe0 && second < 0xa0) || (c == 0xed && second >= 0xa0)
            || (c == 0xf0 && second < 0x90) || (c == 0xf4 && second >= 0x90))
            return false;
        for (unsigned n = 0; n < count; ++n)
            if (!continuation(static_cast<unsigned char>(bytes[i++])))
                return false;
    }
    return true;
}

} // namespace

void validate_material_request(const data::proto::MaterialRequest& value)
{
    validate_message(value);
    validate_id(value.id());
    validate_id(value.source_id());
    text(value.resource_id(), 2048);
    text(value.use_scope(), 4096);
    digest(value.provider_artifact());
    if (!value.max_requests() || value.max_requests() > 32 || !value.max_bytes()
        || value.max_bytes() > max_material_bytes)
        throw std::invalid_argument("material acquisition budget is missing or exceeds bounds");
}

void validate_material_receipt(const data::proto::MaterialReceipt& value)
{
    validate_message(value);
    const auto& evidence = value.evidence();
    const auto& ref = value.reference();
    const auto& content = evidence.content();
    validate_material_request(evidence.request());
    owner(ref.owner());
    owner(content.owner());
    text(evidence.media_type(), 128);
    digest(content.version());
    if (ref.kind() != "material_acquisition" || ref.id() != evidence.request().id()
        || content.kind() != "material_content" || content.id() != content.version()
        || ref.owner().SerializeAsString() != content.owner().SerializeAsString()
        || evidence.bytes() == 0 || evidence.bytes() > evidence.request().max_bytes()
        || evidence.acquired_at_ms() <= 0
        || (evidence.has_source_published_at_ms() && evidence.source_published_at_ms() <= 0)
        || ref.version() != sha256_bytes(evidence.SerializeAsString()))
        throw std::invalid_argument("invalid material acquisition evidence");
}

void validate_material_input(const data::proto::MaterialInput& input)
{
    validate_message(input);
    owner(input.owner());
    validate_material_request(input.request());
}

void validate_material_result(
    const data::proto::MaterialInput& input,
    const data::proto::MaterialReceipt& receipt)
{
    validate_material_input(input);
    validate_material_receipt(receipt);
    if (input.owner().SerializeAsString() != receipt.reference().owner().SerializeAsString()
        || input.request().SerializeAsString() != receipt.evidence().request().SerializeAsString())
        throw Error(ErrorCode::conflict, "material receipt differs from its admitted request");
}

Json decode_material_request(const data::proto::MaterialRequest& request)
{
    validate_material_request(request);
    return {
        {"id", request.id()},
        {"source_id", request.source_id()},
        {"resource_id", request.resource_id()},
        {"use_scope", request.use_scope()},
        {"provider_artifact", request.provider_artifact()},
        {"max_requests", request.max_requests()},
        {"max_bytes", request.max_bytes()}
    };
}

Json decode_material_receipt(const data::proto::MaterialReceipt& receipt)
{
    validate_material_receipt(receipt);
    const auto ref = [](const resource::proto::Object& value) {
        return Json{
            {"node", value.owner().node_id()},
            {"service", value.owner().service_id()},
            {"domain", value.owner().domain()},
            {"kind", value.kind()},
            {"id", value.id()},
            {"version", value.version()}
        };
    };
    const auto& e = receipt.evidence();
    Json result{
        {"reference", ref(receipt.reference())},
        {"content", ref(e.content())},
        {"request", decode_material_request(e.request())},
        {"media_type", e.media_type()},
        {"bytes", e.bytes()},
        {"acquired_at_ms", std::to_string(e.acquired_at_ms())},
        {"source_published_at_ms", nullptr}
    };
    if (e.has_source_published_at_ms())
        result["source_published_at_ms"] = std::to_string(e.source_published_at_ms());
    return result;
}

data::proto::MaterialReceipt freeze_material(
    const resource::proto::Owner& source,
    const data::proto::MaterialRequest& request,
    std::string_view bytes,
    const std::string& media_type,
    std::int64_t acquired_at_ms,
    std::optional<std::int64_t> source_published_at_ms)
{
    validate_material_request(request);
    if (bytes.empty() || bytes.size() > request.max_bytes())
        throw std::invalid_argument("material original exceeds size bounds");
    data::proto::MaterialReceipt result;
    auto& evidence = *result.mutable_evidence();
    *evidence.mutable_request() = request;
    auto& content = *evidence.mutable_content();
    *content.mutable_owner() = source;
    content.set_kind("material_content");
    content.set_id(sha256_bytes(bytes));
    content.set_version(content.id());
    evidence.set_media_type(media_type);
    evidence.set_bytes(bytes.size());
    evidence.set_acquired_at_ms(acquired_at_ms);
    if (source_published_at_ms)
        evidence.set_source_published_at_ms(*source_published_at_ms);
    auto& ref = *result.mutable_reference();
    *ref.mutable_owner() = source;
    ref.set_kind("material_acquisition");
    ref.set_id(request.id());
    ref.set_version(sha256_bytes(evidence.SerializeAsString()));
    validate_material_receipt(result);
    return result;
}

data::proto::MaterialText
read_material_text(const data::proto::MaterialReceipt& receipt, std::string_view bytes)
{
    validate_material_receipt(receipt);
    const auto& e = receipt.evidence();
    if (bytes.size() != e.bytes() || sha256_bytes(bytes) != e.content().version())
        throw std::invalid_argument("material original bytes mismatch");
    data::proto::MaterialText result;
    *result.mutable_content() = e.content();
    if (e.media_type() != "text/plain" && e.media_type() != "text/plain; charset=utf-8")
        result.set_state(data::proto::MaterialText::UNSUPPORTED_MEDIA);
    else if (!utf8(bytes))
        result.set_state(data::proto::MaterialText::INVALID_UTF8);
    else {
        result.set_state(data::proto::MaterialText::READABLE);
        result.set_readable_bytes(bytes.size());
    }
    return result;
}

std::string material_excerpt(
    const data::proto::MaterialReceipt& receipt,
    std::string_view original,
    std::size_t begin,
    std::size_t end)
{
    const auto parsed = read_material_text(receipt, original);
    if (parsed.state() != data::proto::MaterialText::READABLE
        || parsed.readable_bytes() != original.size() || begin >= end || end > original.size()
        || continuation(static_cast<unsigned char>(original[begin]))
        || (end < original.size() && continuation(static_cast<unsigned char>(original[end]))))
        throw std::invalid_argument("invalid material UTF-8 evidence range");
    return std::string(original.substr(begin, end - begin));
}

} // namespace asterion::protocol
