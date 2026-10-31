#include "domain_backup.hpp"
#include "data_backup.hpp"
#include "research_backup.hpp"
#include <asterion/base/error.hpp>
#include <algorithm>
#include <map>
#include <set>

namespace asterion::terminal_backup {
namespace {

std::string key(const resource::proto::Owner& owner)
{
    return owner.node_id() + "/" + owner.domain() + "/" + owner.service_id();
}

std::string key(const resource::proto::Object& value)
{
    return key(value.owner()) + "/" + value.kind() + "/" + value.id() + "/" + value.version();
}

} // namespace

backup_files::Cut cold_cut(std::vector<Selection> selections, std::stop_token stop)
{
    if (selections.empty() || selections.size() > 32)
        throw std::invalid_argument("cold backup requires one to 32 local domain owners");
    std::ranges::sort(selections, {}, [](const auto& value) {
        return key(value.owner);
    });
    std::map<std::string, data::ColdBackup> data;
    std::map<std::string, research::ColdBackup> research;
    std::set<std::filesystem::path> roots;
    std::set<std::string> owners;
    for (const auto& selection : selections) {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "cold backup interrupted");
        const auto identity = key(selection.owner);
        if (!owners.insert(identity).second
            || !roots.insert(std::filesystem::canonical(selection.directory)).second)
            throw std::invalid_argument("duplicate cold backup owner or root");
        if (selection.owner.domain() == "data") {
            data::proto::DataOwner owner;
            owner.set_node_id(selection.owner.node_id());
            owner.set_service_id(selection.owner.service_id());
            data.emplace(identity, data::cold_backup(selection.directory, owner, stop));
        } else if (selection.owner.domain() == "research") {
            research.emplace(
                identity,
                research::cold_backup(selection.directory, selection.owner, stop));
        } else
            throw std::invalid_argument("cold backup domain is not implemented");
    }
    // Every selected domain remains locked. Missing owners are partial scope;
    // disagreeing selected authorities are an inconsistent cut, never partial success.
    std::map<std::string, std::string> data_references;
    std::map<std::string, std::string> research_references;
    std::set<std::string> objects;
    for (const auto& [identity, domain] : data) {
        for (const auto& object : domain.objects)
            objects.insert(key(object));
        for (const auto& reference : domain.references)
            data_references.emplace(
                identity + ":" + key(reference.consumer),
                reference.retention_digest);
    }
    const auto inspect_consumers = [&](const auto& domains) {
        for (const auto& [identity, domain] : domains) {
            for (const auto& object : domain.objects)
                objects.insert(key(object));
            for (const auto& reference : domain.references) {
                resource::proto::Owner source;
                source.set_node_id(reference.source.node_id());
                source.set_service_id(reference.source.service_id());
                source.set_domain("data");
                const auto source_key = key(source);
                const auto reference_key = source_key + ":" + key(reference.consumer);
                research_references.emplace(reference_key, reference.retention_digest);
                if (const auto found = data_references.find(reference_key);
                    found != data_references.end()) {
                    if (found->second != reference.retention_digest)
                        throw Error(
                            ErrorCode::conflict,
                            "backup Data and consumer retention evidence differs");
                } else if (data.contains(source_key) && !reference.released)
                    throw Error(
                        ErrorCode::conflict,
                        "backup accepted consumer lacks Data retention");
            }
        }
    };
    inspect_consumers(research);
    for (const auto& [identity, domain] : data)
        for (const auto& reference : domain.references) {
            const auto expected = identity + ":" + key(reference.consumer);
            if (owners.contains(key(reference.consumer.owner()))
                && !research_references.contains(expected))
                throw Error(ErrorCode::conflict, "backup Data retention lacks consumer evidence");
        }
    std::vector<backup_files::Snapshot> snapshots;
    const auto append = [&](auto& domains) {
        for (auto& [identity, domain] : domains) {
            for (auto& dependency : domain.snapshot.dependencies)
                dependency.included = objects.contains(key(dependency.object));
            snapshots.push_back(std::move(domain.snapshot));
        }
    };
    append(data);
    append(research);
    return backup_files::Cut(std::move(snapshots));
}

} // namespace asterion::terminal_backup
