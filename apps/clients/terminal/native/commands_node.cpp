#include "application_impl.hpp"
#include <asterion/kernel/native_plugin.hpp>
#include <stdexcept>
#include <algorithm>

namespace asterion::terminal {
namespace {
// Pair deployment gives each process its own durable directory and fixed peer.
PolledTask<void> deploy_service(ServiceIo& io, NodeClient& node, bool local, const json& p) {
  const auto status = (co_await PollFuture{node.inspect_status()});
  if (!status.online || !status.health)
    throw std::invalid_argument("connect the node before deployment");
  const auto kind = parse_service_kind(text(p, "kind"));
  const auto name = text(p, "service");
  const auto port = local ? std::uint16_t{0} : port_number(p, "port");
  if (local && (kind != node::v1::TASK_SERVICE || text(p, "port") != "0"))
    throw std::invalid_argument("local deployment requires a task service and port zero");
  if (!local &&
      (status.health->os() != "linux" || status.health->version() != ASTERION_PRODUCT_VERSION))
    throw std::invalid_argument(
        "remote services require an online Linux Agent of the same version");
  const auto platform =
      local ? current_platform() : HostPlatform{.os = "linux", .arch = status.health->arch()};
  const auto programs = [&](node::v1::ServiceKind type) -> PolledTask<ServicePrograms> {
    co_return co_await io.admin<ServicePrograms>([&] {
      return local ? local_service_programs(type) : bundled_service_programs(platform.arch, type);
    });
  };
  std::string data_service;
  if (kind == node::v1::TASK_SERVICE) {
    data_service = name + "-data";
    const node::v1::Service* existing = nullptr;
    for (const auto& service : status.health->services())
      if (service.id() == data_service)
        existing = &service;
    if (existing) {
      if (existing->kind() != node::v1::DATA_SERVICE || existing->task_service() != name)
        throw std::invalid_argument("data and task service bindings disagree");
      (co_await PollFuture{node.action(data_service, "start")});
    } else {
      unsigned data_port = 0;
      if (!local) {
        data_port = 7443;
        for (; data_port <= 65535; ++data_port) {
          const bool occupied =
              data_port == port || data_port == status.port ||
              std::ranges::any_of(status.health->services(),
                                  [&](const auto& service) { return service.port() == data_port; });
          if (!occupied)
            break;
        }
        if (data_port > 65535)
          throw std::invalid_argument("no port available for data service");
      }
      auto data_programs = co_await programs(node::v1::DATA_SERVICE);
      const ServiceDeployment deployment{.service = data_service,
                                         .kind = node::v1::DATA_SERVICE,
                                         .platform = platform,
                                         .programs = std::move(data_programs),
                                         .port = static_cast<std::uint16_t>(data_port),
                                         .task_service = name};
      co_await PollFuture{node.deploy(deployment)};
    }
  }
  auto service_programs = co_await programs(kind);
  const ServiceDeployment deployment{.service = name,
                                     .kind = kind,
                                     .platform = platform,
                                     .programs = std::move(service_programs),
                                     .port = port,
                                     .data_service = data_service};
  co_await PollFuture{node.deploy(deployment)};
}
// Replaces a stopped service's programs with this Terminal's build.
PolledTask<void> update_service(ServiceIo& io, NodeClient& node, bool local, const json& p) {
  const auto service = text(p, "service");
  const auto state = (co_await PollFuture{node.inspect_status()});
  if (!state.online || !state.health)
    throw std::invalid_argument("connect the node before updating");
  const auto& health = *state.health;
  auto kind = node::v1::UNSPECIFIED_SERVICE;
  for (const auto& service_status : health.services())
    if (service_status.id() == service)
      kind = service_status.kind();
  auto programs = co_await io.admin<ServicePrograms>([&] {
    return local ? local_service_programs(kind) : bundled_service_programs(health.arch(), kind);
  });
  const ServiceUpdate update{.service = service,
                             .expected_revision = text(p, "revision"),
                             .platform = {.os = health.os(), .arch = health.arch()},
                             .programs = std::move(programs)};
  co_await PollFuture{node.update(update)};
}
} // namespace
// Node lifecycle: SSH enrollment, firewall, Agent upgrade, service deployment.
void Application::Impl::register_node_commands() {
  command("development.shutdown", [this](const json& p) -> PolledTask<Response> {
    fields(p, {"recover"});
    if (!p.at("recover").is_boolean())
      throw std::invalid_argument("invalid development shutdown request");
    (co_await manage<void>([&]() -> PolledTask<void> {
      (co_await service_io.admin<void>(
          [&] { return shutdown_development_node(service_io, p.at("recover").get<bool>()); }));
    }));
    co_return json{{"stopped", true}};
  });
  command("native.plugins.inspect", [this](const json& p) -> PolledTask<Response> {
    fields(p, {});
    native_plugins = co_await service_io.admin<std::shared_ptr<const json>>(
        [] { return std::make_shared<const json>(plugin_catalog_json(local_plugin_catalog())); });
    co_return snapshot();
  });
  command("native.plugins.preview", [this](const json& p) -> PolledTask<Response> {
    fields(p, {"path"});
    auto result = snapshot();
    auto entry = co_await service_io.admin<PluginCatalogEntry>(
        [&] { return preview_plugin(text(p, "path")); });
    PluginCatalog preview;
    preview.entries.push_back(std::move(entry));
    result.values["plugin_candidate"] = plugin_catalog_json(preview).at("items").at(0);
    co_return result;
  });
  command("native.plugins.install", [this](const json& p) -> PolledTask<Response> {
    fields(p, {"path", "sha256"});
    (co_await service_io.admin<void>([&] {
      return install_plugin(native_plugin_directory(), local_node_directory() / "plugins",
                            text(p, "path"), text(p, "sha256"));
    }));
    native_plugins = co_await service_io.admin<std::shared_ptr<const json>>(
        [] { return std::make_shared<const json>(plugin_catalog_json(local_plugin_catalog())); });
    co_return snapshot();
  });
  command("native.plugins.uninstall", [this](const json& p) -> PolledTask<Response> {
    fields(p, {"file", "sha256"});
    co_await service_io.admin<void>([&] {
      uninstall_plugin(local_node_directory() / "plugins", text(p, "file"), text(p, "sha256"));
    });
    native_plugins = co_await service_io.admin<std::shared_ptr<const json>>(
        [] { return std::make_shared<const json>(plugin_catalog_json(local_plugin_catalog())); });
    co_return snapshot();
  });
  command("node.data_tasks.local.create", [this](const json& p) -> PolledTask<Response> {
    fields(p, {"plugins"});
    if (!p.at("plugins").is_array())
      throw std::invalid_argument("invalid native plugin selection");
    const auto plugins = p.at("plugins").get<std::vector<std::string>>();
    const auto generation = data_task_generation;
    auto [node, next, next_data] =
        (co_await manage<std::tuple<std::shared_ptr<NodeClient>, std::shared_ptr<TaskClient>,
                                    std::shared_ptr<DataClient>>>(
            [&, existing = existing_local_node()]()
                -> PolledTask<std::tuple<std::shared_ptr<NodeClient>, std::shared_ptr<TaskClient>,
                                         std::shared_ptr<DataClient>>> {
              auto node = (co_await local_node_client(existing));
              const auto task = (co_await PollFuture{node->local_data_tasks(plugins)});
              const auto pair = (co_await PollFuture{node->data_task_endpoints(task.session)});
              auto client = (co_await PollFuture{TaskClient::open(service_io, pair.task)});
              auto data = (co_await PollFuture{DataClient::open(service_io, pair.data)});
              co_return std::tuple{std::move(node), std::move(client), std::move(data)};
            }));
    if (generation != data_task_generation)
      throw Error(ErrorCode::conflict,
                  "data/task service selection changed during operation; inspect the "
                  "original services before retrying");
    nodes.try_emplace("local", std::move(node));
    adopt_data_tasks(std::move(next), std::move(next_data));
    co_return snapshot();
  });
  command("node.plugins.configure", [this](const json& p) -> PolledTask<Response> {
    fields(p, {"id", "service", "revision", "plugins"});
    if (!p.at("plugins").is_array())
      throw std::invalid_argument("invalid native plugin selection");
    const auto node = nodes.at(text(p, "id"));
    (co_await manage<void>([&]() -> PolledTask<void> {
      (co_await PollFuture{
          node->configure_plugins(text(p, "service"), text(p, "revision"),
                                  p.at("plugins").get<std::vector<std::string>>())});
    }));
    co_return snapshot();
  });
  command("node.initializer.export", [this](const json& p) -> PolledTask<Response> {
    fields(p, {"path"});
    const auto destination = p.at("path").get<std::string>();
    auto result = snapshot();
    if (destination.empty()) {
      auto content =
          co_await service_io.admin<std::string>([] { return bundled_linux_initializer(); });
      result.values["initializer"] = {{"name", "initialize-linux.py"},
                                      {"content", std::move(content)}};
    } else
      (co_await service_io.admin<void>([&] {
        return export_bundled_initializer(
            std::filesystem::path(std::u8string(destination.begin(), destination.end())));
      }));
    co_return result;
  });
  command("node.key.prepare", [this](const json& p) -> PolledTask<Response> {
    fields(p, {"id"});
    ssh_key = (co_await service_io.admin<json>([&] { return prepare_ssh_key(text(p, "id")); }));
    co_return snapshot();
  });
  command("node.service_firewall", [this](const json& p) -> PolledTask<Response> {
    fields(p, {"id", "service", "action", "token"});
    firewall_plan = nullptr;
    firewall_parameters = nullptr;
    const auto node = nodes.at(text(p, "id"));
    auto plan = (co_await manage<json>([&]() -> PolledTask<json> {
      co_return (co_await PollFuture{
          node->firewall(text(p, "service"), text(p, "action"), p.at("token").get<std::string>())});
    }));
    firewall_plan = std::move(plan);
    co_return snapshot();
  });
  command("node.firewall.inspect", [this](const json& p) -> PolledTask<Response> {
    fields(p, {"id", "host", "ssh_port", "username", "key_source", "private_key", "known_hosts",
               "agent_port", "firewall_port", "firewall_action"});
    firewall_plan = nullptr;
    firewall_parameters = nullptr;
    auto plan = (co_await manage<json>([&]() -> PolledTask<json> {
      co_return (co_await service_io.admin<json>([&] { return inspect_node_firewall(p); }));
    }));
    plan["transport"] = "ssh";
    plan["token"] = unique_process_id();
    firewall_parameters = p;
    firewall_parameters.erase("private_key");
    firewall_plan = std::move(plan);
    firewall_expiry = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    co_return snapshot();
  });
  command("node.firewall.apply", [this](const json& p) -> PolledTask<Response> {
    fields(p, {"token", "private_key"});
    if (firewall_plan.is_null() || firewall_parameters.is_null() ||
        text(p, "token") != firewall_plan.at("token").get<std::string>() ||
        std::chrono::steady_clock::now() > firewall_expiry)
      throw std::invalid_argument("firewall confirmation expired; inspect again");
    auto parameters = firewall_parameters;
    parameters["private_key"] = p.at("private_key").get<std::string>();
    firewall_parameters = nullptr;
    auto plan = firewall_plan;
    firewall_plan = nullptr;
    auto changed = (co_await manage<json>([&]() -> PolledTask<json> {
      co_return (
          co_await service_io.admin<json>([&] { return change_node_firewall(parameters, plan); }));
    }));
    firewall_plan = std::move(changed);
    co_return snapshot();
  });
  command("node.bootstrap", [this](const json& p) -> PolledTask<Response> {
    fields(p, {"id", "host", "ssh_port", "username", "key_source", "private_key", "known_hosts",
               "agent_port"});
    auto [id, node] = (co_await manage<std::pair<std::string, std::shared_ptr<NodeClient>>>(
        [&]() -> PolledTask<std::pair<std::string, std::shared_ptr<NodeClient>>> {
          auto config =
              (co_await service_io.admin<NodeEndpoint>([&] { return enroll_node(service_io, p); }));
          auto key = config.id;
          auto client = co_await PollFuture{NodeClient::open(service_io, std::move(config))};
          co_return std::pair{std::move(key), std::move(client)};
        }));
    nodes.insert_or_assign(id, std::move(node));
    co_return snapshot();
  });
  command("node.connect", [this](const json& p) -> PolledTask<Response> {
    fields(p, {"id"});
    auto [id, node] = (co_await manage<std::pair<std::string, std::shared_ptr<NodeClient>>>(
        [&]() -> PolledTask<std::pair<std::string, std::shared_ptr<NodeClient>>> {
          auto config = (co_await service_io.admin<NodeEndpoint>(
              [&] { return enrolled_node(text(p, "id")); }));
          auto key = config.id;
          auto client = co_await PollFuture{NodeClient::open(service_io, std::move(config))};
          co_return std::pair{std::move(key), std::move(client)};
        }));
    nodes.insert_or_assign(id, std::move(node));
    co_return snapshot();
  });
  command("node.agent.upgrade", [this](const json& p) -> PolledTask<Response> {
    fields(p, {"expected_digest"});
    const auto expected = text(p, "expected_digest");
    agent_program = (co_await service_io.admin<json>([&] { return local_node_program_status(); }));
    if ((agent_program.at("state") != "update_available" &&
         agent_program.at("state") != "recovery_required") ||
        agent_program.at("expected_digest") != expected)
      throw std::runtime_error("inspect the current Agent update before continuing");
    std::exception_ptr failure;
    try {
      auto node = (co_await manage<std::shared_ptr<NodeClient>>(
          [&]() -> PolledTask<std::shared_ptr<NodeClient>> {
            auto config = co_await service_io.admin<NodeEndpoint>(
                [&] { return upgrade_local_node(service_io, expected); });
            co_return co_await PollFuture{NodeClient::open(service_io, std::move(config))};
          }));
      nodes.insert_or_assign("local", std::move(node));
    } catch (...) {
      failure = std::current_exception();
    }
    try {
      agent_program = co_await service_io.admin<json>([] { return local_node_program_status(); });
    } catch (...) {
      agent_program = nullptr;
      if (!failure)
        throw;
    }
    if (failure)
      std::rethrow_exception(failure);
    co_return snapshot();
  });
  command("node.agent.inspect", [this](const json& p) -> PolledTask<Response> {
    fields(p, {});
    agent_program = nullptr;
    agent_program = (co_await service_io.admin<json>([&] { return local_node_program_status(); }));
    co_return snapshot();
  });
  command("node.local", [this](const json& p) -> PolledTask<Response> {
    fields(p, {});
    if (!nodes.contains("local")) {
      auto node = (co_await manage<std::shared_ptr<NodeClient>>(
          [this]() -> PolledTask<std::shared_ptr<NodeClient>> {
            auto config =
                co_await service_io.admin<NodeEndpoint>([&] { return local_node(service_io); });
            co_return co_await PollFuture{NodeClient::open(service_io, std::move(config))};
          }));
      nodes.try_emplace("local", std::move(node));
    }
    co_return snapshot();
  });
  command("node.disconnect", [this](const json& p) -> PolledTask<Response> {
    fields(p, {"id"});
    if (text(p, "id") == "local")
      throw std::invalid_argument("the local node monitor cannot be removed");
    nodes.erase(text(p, "id"));
    co_return snapshot();
  });
  command("node.deploy", [this](const json& p) -> PolledTask<Response> {
    fields(p, {"id", "service", "port", "kind"});
    const auto node = nodes.at(text(p, "id"));
    (co_await manage<void>([&]() -> PolledTask<void> {
      co_await deploy_service(service_io, *node, text(p, "id") == "local", p);
    }));
    co_return snapshot();
  });
  command("node.update", [this](const json& p) -> PolledTask<Response> {
    fields(p, {"id", "service", "revision"});
    const auto node = nodes.at(text(p, "id"));
    (co_await manage<void>([&]() -> PolledTask<void> {
      co_await update_service(service_io, *node, text(p, "id") == "local", p);
    }));
    co_return snapshot();
  });
  command("node.action", [this](const json& p) -> PolledTask<Response> {
    fields(p, {"id", "service", "action"});
    const auto node = nodes.at(text(p, "id"));
    (co_await manage<void>([&]() -> PolledTask<void> {
      (co_await PollFuture{node->action(text(p, "service"), text(p, "action"))});
    }));
    co_return snapshot();
  });
}
} // namespace asterion::terminal
