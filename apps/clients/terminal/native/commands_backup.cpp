#include "application_impl.hpp"
#include "domain_backup.hpp"
#include <asterion/kernel/process/artifact.hpp>
#include <set>

namespace asterion::terminal {
namespace {

struct BackupOperation
{
    bool busy = false;
    std::stop_source stop;
};

std::vector<terminal_backup::Selection>
local_backup_selections(const std::vector<NodeSnapshot>& nodes, const json& ids)
{
    if (!ids.is_array() || ids.empty() || ids.size() > 32)
        throw std::invalid_argument(
            "choose one to 32 local Data, Intelligence, Research or Trading services");
    std::set<std::string> wanted;
    for (const auto& id : ids)
        if (!id.is_string() || !wanted.insert(id.get<std::string>()).second)
            throw std::invalid_argument("duplicate or invalid backup service selection");
    const auto node = std::ranges::find(nodes, std::string("local"), &NodeSnapshot::id);
    if (node == nodes.end() || !node->online || !node->health || node->health->node_id().empty())
        throw Error(ErrorCode::unavailable, "local Agent observation is required for cold backup");
    std::vector<terminal_backup::Selection> result;
    for (const auto& service : node->health->services()) {
        if (!wanted.erase(service.id()))
            continue;
        if (service.desired_running() || service.pid())
            throw Error(ErrorCode::conflict, "stop selected business services before cold backup");
        if (service.kind() != node::proto::DATA_SERVICE && service.kind() != node::proto::RESEARCH
            && service.kind() != node::proto::LIVE_TRADING
            && service.kind() != node::proto::INTELLIGENCE)
            throw std::invalid_argument(
                "cold backup currently supports Data, Intelligence, Research and Trading only");
        resource::proto::Owner owner;
        owner.set_node_id(node->health->node_id());
        owner.set_service_id(service.id());
        owner.set_domain(
            service.kind() == node::proto::DATA_SERVICE       ? "data"
                : service.kind() == node::proto::RESEARCH     ? "research"
                : service.kind() == node::proto::INTELLIGENCE ? "intelligence"
                                                              : "trading");
        result.push_back({std::move(owner), service.directory()});
    }
    if (!wanted.empty())
        throw Error(ErrorCode::not_found, "selected local backup service is absent");
    return result;
}

json report(json manifest)
{
    bool partial = !manifest.at("limitations").empty();
    for (const auto& dependency : manifest.at("dependencies"))
        partial = partial || !dependency.at("included").get<bool>();
    std::uint64_t bytes = 0;
    for (const auto& file : manifest.at("files"))
        bytes += file.at("bytes").get<std::uint64_t>();
    return {{"manifest", std::move(manifest)}, {"partial", partial}, {"payload_bytes", bytes}};
}

json verify_domains(const std::filesystem::path& root, const json& manifest, std::stop_token stop)
{
    std::vector<terminal_backup::Selection> sources;
    std::size_t index = 0;
    for (const auto& identity : manifest.at("owners")) {
        resource::proto::Owner owner;
        owner.set_node_id(identity.at("node").get<std::string>());
        owner.set_service_id(identity.at("service").get<std::string>());
        owner.set_domain(identity.at("domain").get<std::string>());
        sources.push_back({std::move(owner), root / "owners" / std::to_string(index++)});
    }
    auto cut = terminal_backup::cold_cut(std::move(sources), stop);
    auto checked = cut.preview(manifest.at("backup_id").get<std::string>(), stop);
    if (checked.value("scopes", json::array()) != manifest.value("scopes", json::array()))
        throw Error(ErrorCode::conflict, "backup business scope differs from restored owner data");
    return report(std::move(checked));
}

} // namespace

void Application::Impl::register_backup_commands()
{
    const auto state = std::make_shared<BackupOperation>();
    command("system.backup.cancel", [this, state](const json& p) -> PolledTask<Response> {
        fields(p, {});
        const bool requested = state->busy && state->stop.request_stop();
        auto result = snapshot();
        result.values["backup_cancel_requested"] = requested;
        co_return result;
    });
    for (const auto method :
        {"system.backup.preview", "system.backup.save", "system.backup.resume.preview"})
        command(
            method,
            [this, state, method = std::string(method)](const json& p) -> PolledTask<Response> {
                const bool saving = method == "system.backup.save";
                const bool resuming = method == "system.backup.resume.preview";
                if (resuming)
                    fields(p, {"backup"});
                else if (saving)
                    fields(p, {"operation_id", "services", "destination", "preview_digest"});
                else
                    fields(p, {"operation_id", "services", "destination"});
                if (state->busy)
                    throw Error(ErrorCode::conflict, "another backup operation is active");
                state->busy = true;
                ResetFlag reset{state->busy};
                state->stop = std::stop_source{};
                const auto stop = state->stop.get_token();
                std::optional<json> prepared;
                json service_ids = resuming ? json::array() : p.at("services");
                if (resuming) {
                    prepared = co_await service_io.admin<json>([&] {
                        return backup_files::inspect_preparation(text(p, "backup"), stop);
                    });
                    for (const auto& owner : prepared->at("owners"))
                        service_ids.push_back(owner.at("service"));
                }
                manager::proto::Request observe;
                observe.mutable_snapshot();
                co_await PollFuture{manager.request(std::move(observe))};
                const auto sources = local_backup_selections(manager.view(), service_ids);
                const auto operation = resuming ? prepared->at("backup_id").get<std::string>()
                                                : text(p, "operation_id");
                const auto destination = resuming ? text(p, "backup") : text(p, "destination");
                const auto expected = resuming ? sha256_bytes(prepared->dump())
                    : saving                   ? text(p, "preview_digest")
                                               : std::string{};
                auto result = snapshot();
                result.values["backup"] = co_await service_io.admin<json>([&] {
                    auto cut = terminal_backup::cold_cut(sources, stop);
                    auto manifest = saving ? cut.save(operation, destination, expected, stop)
                                           : cut.preview(operation, stop);
                    const auto digest = sha256_bytes(manifest.dump());
                    if (resuming && digest != expected)
                        throw Error(ErrorCode::conflict, "backup scope changed; preview again");
                    auto value = report(std::move(manifest));
                    value["preview_digest"] = digest;
                    value["saved"] = saving;
                    const std::filesystem::path target(destination);
                    if (!target.is_absolute())
                        throw std::invalid_argument("choose a new absolute backup directory");
                    value["available_bytes"] =
                        std::filesystem::space(target.parent_path()).available;
                    return value;
                });
                co_return result;
            });
    for (const auto method : {"system.backup.inspect", "system.backup.restore"})
        command(
            method,
            [this, state, method = std::string(method)](const json& p) -> PolledTask<Response> {
                const bool restoring = method == "system.backup.restore";
                if (restoring)
                    fields(p, {"backup", "destination"});
                else
                    fields(p, {"backup"});
                if (state->busy)
                    throw Error(ErrorCode::conflict, "another backup operation is active");
                state->busy = true;
                ResetFlag reset{state->busy};
                state->stop = std::stop_source{};
                const auto stop = state->stop.get_token();
                const std::filesystem::path source = text(p, "backup");
                const std::filesystem::path destination =
                    restoring ? text(p, "destination") : std::string{};
                auto result = snapshot();
                result.values["backup"] = co_await service_io.admin<json>([&] {
                    const auto manifest = backup_files::inspect(source, stop);
                    // Validate domain schemas and bilateral references in the immutable
                    // backup itself before creating any restore target.
                    auto value = verify_domains(source, manifest, stop);
                    if (restoring) {
                        if (!destination.is_absolute())
                            throw std::invalid_argument("choose a new absolute backup directory");
                        if (value.at("payload_bytes").get<std::uint64_t>()
                            > std::filesystem::space(destination.parent_path()).available)
                            throw Error(
                                ErrorCode::resource_exhausted,
                                "backup destination has insufficient space");
                        backup_files::restore_files(source, destination, stop);
                        value = verify_domains(destination, manifest, stop);
                    }
                    value["restored"] = restoring;
                    return value;
                });
                co_return result;
            });
}

} // namespace asterion::terminal
