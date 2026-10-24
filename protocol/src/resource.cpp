#include <asterion/protocol/resource.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/foundation/id.hpp>
#include <asterion/foundation/error.hpp>
#include <filesystem>

namespace asterion::protocol {

unsigned catalogue_limit(const resource::proto::CatalogueQuery& query)
{
    validate_message(query);
    validate_id(query.kind());
    if (!query.after().empty())
        validate_id(query.after());
    const auto limit = query.limit() ? query.limit() : 64U;
    if (limit > 128)
        throw std::invalid_argument("resource catalogue page exceeds capacity");
    return limit;
}

Json decode_resource_catalogue(
    const resource::proto::Catalogue& page,
    const resource::proto::CatalogueQuery& query,
    const resource::proto::Owner& expected)
{
    validate_message(page);
    const auto limit = catalogue_limit(query);
    if (page.owner().SerializeAsString() != expected.SerializeAsString()
        || page.kind() != query.kind() || page.items_size() > static_cast<int>(limit))
        throw Error(ErrorCode::unavailable, "resource catalogue owner or scope mismatch");
    validate_id(expected.node_id());
    validate_id(expected.service_id());
    Json result{
        {"owner",
            {{"node", expected.node_id()},
                {"service", expected.service_id()},
                {"domain", expected.domain()}}},
        {"kind", page.kind()},
        {"items", Json::array()},
        {"next_after", page.next_after()}
    };
    auto previous = query.after();
    for (const auto& entry : page.items()) {
        const auto& object = entry.object();
        if (object.owner().SerializeAsString() != expected.SerializeAsString()
            || object.kind() != page.kind() || object.id() <= previous
            || object.version().size() != 64
            || object.version().find_first_not_of("0123456789abcdef") != std::string::npos
            || entry.name().size() > 4096
            || entry.state() < resource::proto::CatalogueEntry::REGISTERED
            || entry.state() > resource::proto::CatalogueEntry::RETIRED)
            throw Error(ErrorCode::unavailable, "invalid resource catalogue entry");
        validate_id(object.id());
        const std::filesystem::path location(entry.relative_location());
        if (location.empty() || location.is_absolute())
            throw Error(ErrorCode::unavailable, "invalid resource locator");
        for (const auto& part : location)
            if (part == ".." || part == ".")
                throw Error(ErrorCode::unavailable, "invalid resource locator");
        result["items"].push_back(
            {{"object",
                 {{"node", expected.node_id()},
                     {"service", expected.service_id()},
                     {"domain", expected.domain()},
                     {"kind", object.kind()},
                     {"id", object.id()},
                     {"version", object.version()}}},
                {"state",
                    entry.state() == resource::proto::CatalogueEntry::REGISTERED   ? "registered"
                        : entry.state() == resource::proto::CatalogueEntry::SEALED ? "sealed"
                                                                                   : "retired"},
                {"name", entry.name()},
                {"logical_bytes",
                    entry.has_logical_bytes() ? Json(std::to_string(entry.logical_bytes()))
                                              : Json(nullptr)},
                {"relative_location", entry.relative_location()}});
        previous = object.id();
    }
    if (!page.next_after().empty()
        && (page.items_size() != static_cast<int>(limit) || page.next_after() != previous))
        throw Error(ErrorCode::unavailable, "invalid resource catalogue continuation");
    return result;
}

} // namespace asterion::protocol
