#include "application_impl.hpp"
#include <asterion/kernel/native_plugin.hpp>

namespace asterion::terminal {
namespace wire = manager::proto;

void Application::Impl::register_node_commands()
{
    command("development.shutdown", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"recover"});
        if (!p.at("recover").is_boolean())
            throw std::invalid_argument("invalid development shutdown request");
        co_await PollFuture{manager.shutdown(p.at("recover").get<bool>())};
        co_return json{{"stopped", true}};
    });
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
                local_node_directory() / "plugins",
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
                local_node_directory() / "plugins",
                text(p, "file"),
                text(p, "sha256"));
        });
        native_plugins = co_await service_io.admin<std::shared_ptr<const json>>([] {
            return std::make_shared<const json>(plugin_catalog_json(local_plugin_catalog()));
        });
        co_return snapshot();
    });
    command("node.data_tasks.local.create", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"plugins"});
        if (!p.at("plugins").is_array())
            throw std::invalid_argument("invalid native plugin selection");
        const auto plugins = p.at("plugins").get<std::vector<std::string>>();
        const auto generation = data_task_generation;
        auto awaited = (co_await manage<std::tuple<
                std::shared_ptr<NodeClient>,
                std::shared_ptr<TaskClient>,
                std::shared_ptr<DataClient>>>(
            [&]() -> PolledTask<std::tuple<
                      std::shared_ptr<NodeClient>,
                      std::shared_ptr<TaskClient>,
                      std::shared_ptr<DataClient>>> {
                auto node = (co_await local_node_client());
                const auto task = (co_await PollFuture{node->local_data_tasks(plugins)});
                const auto pair = (co_await PollFuture{node->data_task_endpoints(task.session)});
                auto client = (co_await PollFuture{TaskClient::open(service_io, pair.task)});
                auto data = (co_await PollFuture{DataClient::open(service_io, pair.data)});
                co_return std::tuple{std::move(node), std::move(client), std::move(data)};
            }));
        auto& [node, next, next_data] = awaited;
        if (generation != data_task_generation)
            throw Error(
                ErrorCode::conflict,
                "data/task service selection changed during operation; inspect the " "original services before retrying");
        adopt_data_tasks(std::move(next), std::move(next_data));
        co_return snapshot();
    });
    command("node.initializer.export", [this](const json& p) -> PolledTask<Response> {
        fields(p, {"path"});
        const auto destination = p.at("path").get<std::string>();
        auto result = snapshot();
        if (destination.empty()) {
            auto content = co_await service_io.admin<std::string>([] {
                return bundled_linux_initializer();
            });
            result.values["initializer"] = {
                {"name", "initialize-linux.py"},
                {"content", std::move(content)}
            };
        } else
            (co_await service_io.admin<void>([&] {
                return export_bundled_initializer(
                    std::filesystem::path(std::u8string(destination.begin(), destination.end())));
            }));
        co_return result;
    });

    for (const auto method :
        {"node.local",
            "node.connect",
            "node.disconnect",
            "node.deploy",
            "node.update",
            "node.action",
            "node.plugins.configure",
            "node.service_firewall"})
        command(
            method,
            [this, method = std::string(method)](const json& p) -> PolledTask<Response> {
                wire::Request request;
                auto& node = *request.mutable_node();
                if (method == "node.local") {
                    fields(p, {});
                    node.set_id("local");
                    node.mutable_connect();
                } else {
                    node.set_id(text(p, "id"));
                    if (method == "node.connect" || method == "node.disconnect") {
                        fields(p, {"id"});
                        if (method == "node.connect")
                            node.mutable_connect();
                        else
                            node.mutable_disconnect();
                    } else if (method == "node.deploy") {
                        fields(p, {"id", "service", "port", "kind"});
                        auto& d = *node.mutable_deploy();
                        d.set_service(text(p, "service"));
                        d.set_kind(parse_service_kind(text(p, "kind")));
                        d.set_port(
                            node.id() == "local" && text(p, "port") == "0"
                                ? 0
                                : port_number(p, "port"));
                    } else if (method == "node.update") {
                        fields(p, {"id", "service", "revision"});
                        node.mutable_update()->set_service(text(p, "service"));
                        node.mutable_update()->set_revision(text(p, "revision"));
                    } else if (method == "node.plugins.configure") {
                        fields(p, {"id", "service", "revision", "plugins"});
                        auto& c = *node.mutable_configure_plugins();
                        c.set_service(text(p, "service"));
                        c.set_revision(text(p, "revision"));
                        for (const auto& hash : p.at("plugins").get<std::vector<std::string>>())
                            c.mutable_plugins()->add_hashes(hash);
                    } else {
                        const bool firewall = method == "node.service_firewall";
                        if (firewall)
                            fields(p, {"id", "service", "action", "token"});
                        else
                            fields(p, {"id", "service", "action"});
                        auto& action = firewall ? *node.mutable_firewall() : *node.mutable_action();
                        action.set_service(text(p, "service"));
                        action.set_action(text(p, "action"));
                        if (firewall) {
                            action.set_token(p.at("token").get<std::string>());
                            firewall_plan = nullptr;
                        }
                    }
                }
                const auto reply = co_await PollFuture{manager.request(std::move(request))};
                if (method == "node.service_firewall")
                    firewall_plan = json::parse(reply.presentation());
                co_return snapshot();
            });
    for (const auto method :
        {"node.key.prepare",
            "node.bootstrap",
            "node.identity.replace",
            "node.firewall.inspect",
            "node.firewall.apply"})
        command(
            method,
            [this, method = std::string(method)](const json& p) -> PolledTask<Response> {
                wire::Request request;
                auto& ssh = *request.mutable_ssh();
                if (method == "node.key.prepare") {
                    fields(p, {"id"});
                    ssh.set_operation(wire::Ssh::PREPARE_KEY);
                    ssh.set_id(text(p, "id"));
                } else if (method == "node.firewall.apply") {
                    fields(p, {"token", "private_key"});
                    ssh.set_operation(wire::Ssh::APPLY_FIREWALL);
                    ssh.set_token(text(p, "token"));
                    ssh.set_private_key(p.at("private_key").get<std::string>());
                } else {
                    if (method == "node.bootstrap")
                        fields(
                            p,
                            {"id",
                                "host",
                                "ssh_port",
                                "username",
                                "key_source",
                                "private_key",
                                "known_hosts",
                                "agent_port"});
                    else if (method == "node.identity.replace")
                        fields(
                            p,
                            {"id",
                                "host",
                                "ssh_port",
                                "username",
                                "key_source",
                                "private_key",
                                "known_hosts"});
                    else
                        fields(
                            p,
                            {"id",
                                "host",
                                "ssh_port",
                                "username",
                                "key_source",
                                "private_key",
                                "known_hosts",
                                "agent_port",
                                "firewall_port",
                                "firewall_action"});
                    ssh.set_operation(
                        method == "node.bootstrap"              ? wire::Ssh::ENROLL
                            : method == "node.identity.replace" ? wire::Ssh::REPLACE_IDENTITY
                                                                : wire::Ssh::INSPECT_FIREWALL);
                    ssh.set_id(text(p, "id"));
                    ssh.set_host(text(p, "host"));
                    ssh.set_ssh_port(text(p, "ssh_port"));
                    ssh.set_username(text(p, "username"));
                    ssh.set_key_source(text(p, "key_source"));
                    ssh.set_private_key(p.at("private_key").get<std::string>());
                    ssh.set_known_hosts(p.at("known_hosts").get<std::string>());
                    if (p.contains("agent_port"))
                        ssh.set_agent_port(text(p, "agent_port"));
                    if (p.contains("firewall_port")) {
                        ssh.set_firewall_port(text(p, "firewall_port"));
                        ssh.set_firewall_action(text(p, "firewall_action"));
                    }
                }
                if (method.starts_with("node.firewall."))
                    firewall_plan = nullptr;
                const auto reply = co_await PollFuture{manager.request(std::move(request))};
                if (method == "node.key.prepare")
                    ssh_key = json::parse(reply.presentation());
                else if (method.starts_with("node.firewall."))
                    firewall_plan = json::parse(reply.presentation());
                co_return snapshot();
            });
}

} // namespace asterion::terminal
