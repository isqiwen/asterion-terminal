#include "application_impl.hpp"
#include <asterion/kernel/native_plugin.hpp>

namespace asterion::terminal {

void Application::Impl::register_node_commands()
{
    command("native.plugins.inspect", [this](const json& p) -> PolledTask<Response> {
        fields(p, {});
        native_plugins = co_await service_io.admin<std::shared_ptr<const json>>([] {
            return std::make_shared<const json>(plugin_catalog_json(local_plugin_catalog()));
        });
        co_return snapshot();
    });
    command("native.plugins.preview", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"path"});
        auto result = snapshot();
        auto entry = co_await service_io.admin<PluginCatalogEntry>([&] {
            return preview_plugin(text(p, "path"));
        });
        PluginCatalog preview;
        preview.entries.push_back(std::move(entry));
        result.values["plugin_candidate"] = plugin_catalog_json(preview).at("items").at(0);
        co_return result;
    });
    command("native.plugins.install", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"path", "sha256"});
        (co_await service_io.admin<void>([&] {
            return install_plugin(
                native_plugin_directory(),
                program_directory("terminal") / "data" / "plugins",
                text(p, "path"),
                text(p, "sha256"));
        }));
        native_plugins = co_await service_io.admin<std::shared_ptr<const json>>([] {
            return std::make_shared<const json>(plugin_catalog_json(local_plugin_catalog()));
        });
        co_return snapshot();
    });
    command("native.plugins.uninstall", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"file", "sha256"});
        co_await service_io.admin<void>([&] {
            uninstall_plugin(
                program_directory("terminal") / "data" / "plugins",
                text(p, "file"),
                text(p, "sha256"));
        });
        native_plugins = co_await service_io.admin<std::shared_ptr<const json>>([] {
            return std::make_shared<const json>(plugin_catalog_json(local_plugin_catalog()));
        });
        co_return snapshot();
    });
    command("node.research.local.create", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"plugins"});
        if (!p.at("plugins").is_array())
            throw std::invalid_argument("invalid native plugin selection");
        const auto plugins = p.at("plugins").get<std::vector<std::string>>();
        const auto generation = data_task_generation;
        const auto address = co_await PollFuture{services.start(node::proto::RESEARCH, plugins)};
        auto client = co_await PollFuture{ResearchClient::open(service_io, address)};
        if (generation != data_task_generation)
            throw Error(ErrorCode::conflict, "research owner selection changed");
        adopt_research(std::move(client));
        co_return snapshot();
    });
    // A local service's run state, kept across Terminal restarts.
    command("node.action", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"service", "action"});
        co_await PollFuture{services.act(text(p, "service"), text(p, "action"))};
        co_return snapshot();
    });
    // A stopped Data or Research service's plugin selection.
    command("node.plugins.configure", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"service", "revision", "plugins"});
        co_await PollFuture{services.configure_plugins(
            text(p, "service"),
            text(p, "revision"),
            p.at("plugins").get<std::vector<std::string>>())};
        co_return snapshot();
    });
}

} // namespace asterion::terminal
