#include "data_store.hpp"
#include "observation_store.hpp"
#include "material_store.hpp"
#include <asterion/protocol/resource.hpp>
#include <asterion/foundation/error.hpp>
#include <asterion/foundation/id.hpp>
#include <map>
#include <asterion/kernel/process/artifact.hpp>

namespace asterion::data {

resource::proto::Catalogue Store::resources(
    const resource::proto::Owner& owner,
    const resource::proto::CatalogueQuery& query,
    std::stop_token stop) const
{
    const auto limit = protocol::catalogue_limit(query);
    validate_id(owner.node_id());
    if (owner.service_id() != instance_ || owner.domain() != "data")
        throw Error(ErrorCode::conflict, "resource catalogue owner mismatch");
    if (!query.after().empty() && query.kind() == "material_acquisition")
        validate_id(query.after());
    if (!query.after().empty() && query.kind() != "material_acquisition"
        && (query.after().size() != 64
            || query.after().find_first_not_of("0123456789abcdef") != std::string::npos))
        throw std::invalid_argument("invalid Data resource cursor");
    resource::proto::Catalogue result;
    *result.mutable_owner() = owner;
    result.set_kind(query.kind());
    std::map<std::string, resource::proto::CatalogueEntry> entries;
    const auto add = [&](const std::string& id,
                         resource::proto::CatalogueEntry::State state,
                         const std::string& name,
                         const std::filesystem::path& relative) {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "resource catalogue cancelled");
        if (id <= query.after())
            return;
        auto& entry = entries[id];
        auto* object = entry.mutable_object();
        *object->mutable_owner() = owner;
        object->set_kind(query.kind());
        object->set_id(id);
        object->set_version(id);
        entry.set_state(state);
        entry.set_name(name);
        entry.set_relative_location(relative.generic_string());
        if (entries.size() > 100000)
            throw Error(ErrorCode::resource_exhausted, "resource catalogue scan exceeds capacity");
    };
    if (query.kind() == "table_snapshot") {
        for (const auto& row : tables_.snapshot_catalogue(query.after(), limit + 1)) {
            const auto retired = tables_.retirement(row.snapshot);
            add(row.snapshot,
                !retired                ? resource::proto::CatalogueEntry::REGISTERED
                    : retired->complete ? resource::proto::CatalogueEntry::RETIRED
                                        : resource::proto::CatalogueEntry::SEALED,
                "",
                "tables");
        }
    } else if (query.kind() == "minute_version") {
        const auto record = [&](const proto::HistoryRecord& source,
                                resource::proto::CatalogueEntry::State state) {
            add(source.minute_result().manifest_sha256(),
                state,
                source.minutes().contract_id() + " / " + source.minutes().source(),
                std::filesystem::path(source.minute_result().directory())
                    .lexically_relative(root_));
        };
        for (const auto& row : archive_.datasets({}))
            if (row.id > query.after())
                record(archive_.get(row.id), resource::proto::CatalogueEntry::REGISTERED);
        for (const auto& row : archive_.retirements(query.after(), limit + 1))
            record(
                row.record(),
                row.complete() ? resource::proto::CatalogueEntry::RETIRED
                               : resource::proto::CatalogueEntry::SEALED);
    } else if (query.kind() == "observation_archive") {
        Observations observations(root_, owner);
        for (const auto& operation : observations.operations(stop)) {
            if (operation.state() != proto::ObservationOperation::PUBLISHED)
                continue;
            const auto relative =
                std::filesystem::path("observations/objects") / (operation.archive().id() + ".pb");
            const auto file = root_ / relative;
            if (std::filesystem::is_symlink(file) || !std::filesystem::is_regular_file(file))
                throw std::invalid_argument("published observation archive is missing");
            add(operation.archive().id(),
                resource::proto::CatalogueEntry::REGISTERED,
                operation.id(),
                relative);
            if (const auto found = entries.find(operation.archive().id()); found != entries.end())
                found->second.set_logical_bytes(std::filesystem::file_size(file));
        }
    } else if (query.kind() == "material_acquisition" || query.kind() == "material_content") {
        const Materials materials(root_, owner);
        for (const auto& receipt : materials.receipts(stop)) {
            const auto& content = receipt.evidence().content();
            const auto& ref =
                query.kind() == "material_acquisition" ? receipt.reference() : content;
            if (ref.id() <= query.after())
                continue;
            const auto original = root_ / "materials/objects" / content.version();
            if (std::filesystem::is_symlink(original) || !std::filesystem::is_regular_file(original)
                || std::filesystem::file_size(original) != receipt.evidence().bytes())
                throw std::invalid_argument("material original is missing from its catalogue");
            auto& entry = entries[ref.id()];
            *entry.mutable_object() = ref;
            entry.set_state(resource::proto::CatalogueEntry::REGISTERED);
            if (query.kind() == "material_acquisition") {
                entry.set_name(
                    receipt.evidence().request().source_id() + " / "
                    + receipt.evidence().request().resource_id());
                entry.set_relative_location(
                    (std::filesystem::path("materials/receipts") / (ref.id() + ".pb"))
                        .generic_string());
                entry.set_logical_bytes(receipt.ByteSizeLong());
            } else {
                entry.set_relative_location(
                    (std::filesystem::path("materials/objects") / ref.id()).generic_string());
                entry.set_logical_bytes(receipt.evidence().bytes());
            }
        }
    } else if (query.kind() == "provider_module") {
        namespace fs = std::filesystem;
        const auto programs = root_ / "programs";
        if (fs::is_symlink(programs))
            throw std::invalid_argument("invalid Data program directory");
        if (fs::exists(programs)) {
            if (!fs::is_directory(programs))
                throw std::invalid_argument("invalid Data program directory");
            unsigned scanned = 0;
            for (const auto& file : fs::directory_iterator(programs)) {
                if (stop.stop_requested())
                    throw Error(ErrorCode::cancelled, "resource catalogue cancelled");
                if (++scanned > 100000)
                    throw Error(
                        ErrorCode::resource_exhausted,
                        "resource catalogue scan exceeds capacity");
                const auto id = file.path().stem().string();
                const auto extension = file.path().extension().string();
                if (!file.is_symlink() && file.is_directory()
                    && file.path().filename() == "staging")
                    continue;
                if (file.is_symlink() || !file.is_regular_file() || id.size() != 64
                    || id.find_first_not_of("0123456789abcdef") != std::string::npos
                    || (extension != ".so" && extension != ".dylib")
                    || fs::file_size(file.path()) > max_artifact_bytes)
                    throw std::invalid_argument("invalid retained Data provider program");
                add(id,
                    resource::proto::CatalogueEntry::REGISTERED,
                    "",
                    file.path().lexically_relative(root_));
                if (const auto found = entries.find(id); found != entries.end())
                    found->second.set_logical_bytes(fs::file_size(file.path()));
                // A rebuildable catalogue is not an integrity or deletion authority.
                // Cold backup verifies the content digest and platform before copying.
            }
        }
    } else
        throw Error(ErrorCode::invalid_request, "resource kind is not implemented by Data");
    for (auto& [id, entry] : entries) {
        if (result.items_size() == static_cast<int>(limit)) {
            result.set_next_after(result.items(result.items_size() - 1).object().id());
            break;
        }
        *result.add_items() = std::move(entry);
    }
    return result;
}

} // namespace asterion::data
