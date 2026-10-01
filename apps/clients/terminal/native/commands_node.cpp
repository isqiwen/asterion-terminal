#include "application_impl.hpp"
#include <asterion/kernel/native_plugin.hpp>
#include <stdexcept>

namespace asterion::terminal {
namespace {
// Uploads and starts a bundled Linux service on a remote Agent of this version.
void deploy_service(NodeClient& node, const json& p) {
  const auto status = node.inspect_status();
  if (!status.online || !status.health || status.health->os() != "linux" ||
      status.health->version() != ASTERION_PRODUCT_VERSION)
    throw std::invalid_argument(
        "remote services require an online Linux Agent of the same version");
  const auto& arch = status.health->arch();
  const auto kind = parse_service_kind(text(p, "kind"));
  if (kind == node::v1::STRATEGY)
    throw std::invalid_argument("invalid service kind");
  node.deploy({.service = text(p, "service"),
               .kind = kind,
               .platform = {.os = "linux", .arch = arch},
               .programs = bundled_service_programs(arch, kind),
               .port = port_number(p, "port")});
}
// Replaces a stopped service's programs with this Terminal's build.
void update_service(NodeClient& node, bool local, const json& p) {
  const auto service = text(p, "service");
  const auto state = node.inspect_status();
  if (!state.online || !state.health)
    throw std::invalid_argument("connect the node before updating");
  const auto& health = *state.health;
  auto kind = node::v1::UNSPECIFIED_SERVICE;
  for (const auto& service_status : health.services())
    if (service_status.id() == service)
      kind = service_status.kind();
  if (kind != node::v1::PAPER_TRADING && kind != node::v1::MARKET_DATA &&
      kind != node::v1::TASK_SERVICE && kind != node::v1::STRATEGY &&
      kind != node::v1::LIVE_TRADING)
    throw std::invalid_argument("unknown service kind");
  node.update({.service = service,
               .expected_revision = text(p, "revision"),
               .platform = {.os = health.os(), .arch = health.arch()},
               .programs = local ? local_service_programs(kind)
                                 : bundled_service_programs(health.arch(), kind)});
}
} // namespace
// Node lifecycle: SSH enrollment, firewall, Agent upgrade, service deployment.
void Application::Impl::register_node_commands() {
  core.access().grant("terminal.local", "node.manage");
  core.command("native.plugins.inspect", "node.manage", [this](const json& p) {
    fields(p, {});
    native_plugins = plugin_catalog_json(local_plugin_catalog());
    return snapshot();
  });
  core.command("native.plugins.preview", "node.manage", [this](const json& p) {
    fields(p, {"path"});
    auto result = snapshot();
    PluginCatalog preview{.directory = {}, .entries = {preview_plugin(text(p, "path"))}};
    result["plugin_candidate"] = plugin_catalog_json(preview).at("items").at(0);
    return result;
  });
  core.command("native.plugins.install", "node.manage", [this](const json& p) {
    fields(p, {"path", "sha256"});
    install_plugin(native_plugin_directory(), local_node_directory() / "plugins", text(p, "path"),
                   text(p, "sha256"));
    native_plugins = plugin_catalog_json(local_plugin_catalog());
    return snapshot();
  });
  core.command("native.plugins.uninstall", "node.manage", [this](const json& p) {
    fields(p, {"file", "sha256"});
    uninstall_plugin(local_node_directory() / "plugins", text(p, "file"), text(p, "sha256"));
    native_plugins = plugin_catalog_json(local_plugin_catalog());
    return snapshot();
  });
  core.command("research.local.create", "node.manage", [this](const json& p) {
    fields(p, {"plugins"});
    if (!p.at("plugins").is_array())
      throw std::invalid_argument("invalid native plugin selection");
    const auto plugins = p.at("plugins").get<std::vector<std::string>>();
    auto [node, next] = without_operations([&, existing = existing_local_node()] {
      auto node = local_node_client(existing);
      auto client = std::make_shared<ResearchClient>(node->local_research(plugins));
      return std::pair{std::move(node), std::move(client)};
    });
    nodes.try_emplace("local", std::move(node));
    research = std::move(next);
    ++research_generation;
    return snapshot();
  });
  core.command("node.plugins.configure", "node.manage", [this](const json& p) {
    fields(p, {"id", "service", "revision", "plugins"});
    if (!p.at("plugins").is_array())
      throw std::invalid_argument("invalid native plugin selection");
    const auto node = nodes.at(text(p, "id"));
    without_operations([&] {
      node->configure_plugins(text(p, "service"), text(p, "revision"),
                              p.at("plugins").get<std::vector<std::string>>());
    });
    return snapshot();
  });
  core.command("node.initializer.export", "node.manage", [this](const json& p) {
    fields(p, {"path"});
    const auto destination = p.at("path").get<std::string>();
    auto result = snapshot();
    if (destination.empty())
      result["initializer"] = {{"name", "initialize-linux.py"},
                               {"content", bundled_linux_initializer()}};
    else
      export_bundled_initializer(
          std::filesystem::path(std::u8string(destination.begin(), destination.end())));
    return result;
  });
  core.command("node.key.prepare", "node.manage", [this](const json& p) {
    fields(p, {"id"});
    ssh_key = prepare_ssh_key(text(p, "id"));
    return snapshot();
  });
  core.command("node.service_firewall", "node.manage", [this](const json& p) {
    fields(p, {"id", "service", "action", "token"});
    firewall_plan = nullptr;
    firewall_parameters = nullptr;
    const auto node = nodes.at(text(p, "id"));
    auto plan = without_operations([&] {
      return node->firewall(text(p, "service"), text(p, "action"),
                            p.at("token").get<std::string>());
    });
    firewall_plan = std::move(plan);
    return snapshot();
  });
  core.command("node.firewall.inspect", "node.manage", [this](const json& p) {
    fields(p, {"id", "host", "ssh_port", "username", "key_source", "private_key", "known_hosts",
               "agent_port", "firewall_port", "firewall_action"});
    firewall_plan = nullptr;
    firewall_parameters = nullptr;
    auto plan = without_operations([&] { return inspect_node_firewall(p); });
    plan["transport"] = "ssh";
    plan["token"] = unique_process_id();
    firewall_parameters = p;
    firewall_parameters.erase("private_key");
    firewall_plan = std::move(plan);
    firewall_expiry = std::chrono::steady_clock::now() + std::chrono::minutes(5);
    return snapshot();
  });
  core.command("node.firewall.apply", "node.manage", [this](const json& p) {
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
    auto changed = without_operations([&] { return change_node_firewall(parameters, plan); });
    firewall_plan = std::move(changed);
    return snapshot();
  });
  core.command("node.bootstrap", "node.manage", [this](const json& p) {
    fields(p, {"id", "host", "ssh_port", "username", "key_source", "private_key", "known_hosts",
               "agent_port"});
    auto [id, node] = without_operations([&] {
      auto config = enroll_node(p);
      auto key = config.id;
      return std::pair{std::move(key), std::make_shared<NodeClient>(std::move(config))};
    });
    nodes.insert_or_assign(id, std::move(node));
    return snapshot();
  });
  core.command("node.connect", "node.manage", [this](const json& p) {
    fields(p, {"id"});
    auto [id, node] = without_operations([&] {
      auto config = enrolled_node(text(p, "id"));
      auto key = config.id;
      return std::pair{std::move(key), std::make_shared<NodeClient>(std::move(config))};
    });
    nodes.insert_or_assign(id, std::move(node));
    return snapshot();
  });
  core.command("node.agent.upgrade", "node.manage", [this](const json& p) {
    fields(p, {"expected_digest"});
    const auto expected = text(p, "expected_digest");
    agent_program = local_node_program_status();
    if ((agent_program.at("state") != "update_available" &&
         agent_program.at("state") != "recovery_required") ||
        agent_program.at("expected_digest") != expected)
      throw std::runtime_error("inspect the current Agent update before continuing");
    try {
      auto node = without_operations(
          [&] { return std::make_shared<NodeClient>(upgrade_local_node(expected)); });
      nodes.insert_or_assign("local", std::move(node));
      agent_program = local_node_program_status();
    } catch (...) {
      try {
        agent_program = local_node_program_status();
      } catch (...) {
        agent_program = nullptr;
      }
      throw;
    }
    return snapshot();
  });
  core.command("node.agent.inspect", "node.manage", [this](const json& p) {
    fields(p, {});
    agent_program = nullptr;
    agent_program = local_node_program_status();
    return snapshot();
  });
  core.command("node.local", "node.manage", [this](const json& p) {
    fields(p, {});
    if (!nodes.contains("local"))
      nodes.emplace("local", std::make_shared<NodeClient>(local_node()));
    return snapshot();
  });
  core.command("node.disconnect", "node.manage", [this](const json& p) {
    fields(p, {"id"});
    if (text(p, "id") == "local")
      throw std::invalid_argument("the local node monitor cannot be removed");
    nodes.erase(text(p, "id"));
    return snapshot();
  });
  core.command("node.attach", "node.manage", [this](const json& p) {
    fields(p, {"id", "service"});
    const auto node = nodes.at(text(p, "id"));
    const auto service = text(p, "service");
    auto kind = node::v1::UNSPECIFIED_SERVICE;
    if (const auto state = node->inspect_status(); state.health)
      for (const auto& item : state.health->services())
        if (item.id() == service)
          kind = item.kind();
    if (kind != node::v1::PAPER_TRADING && kind != node::v1::LIVE_TRADING)
      throw std::invalid_argument("attach a paper or live trading service");
    auto& client = kind == node::v1::LIVE_TRADING ? live : paper;
    if (client)
      throw std::invalid_argument("disconnect the current trading session first");
    client = std::make_unique<TradingClient>(node->service_endpoint(service, kind),
                                             kind == node::v1::LIVE_TRADING ? TradingMode::live
                                                                            : TradingMode::paper);
    return snapshot();
  });
  core.command("node.deploy", "node.manage", [this](const json& p) {
    fields(p, {"id", "service", "port", "kind"});
    const auto node = nodes.at(text(p, "id"));
    without_operations([&] { deploy_service(*node, p); });
    return snapshot();
  });
  core.command("node.update", "node.manage", [this](const json& p) {
    fields(p, {"id", "service", "revision"});
    const auto node = nodes.at(text(p, "id"));
    without_operations([&] { update_service(*node, text(p, "id") == "local", p); });
    return snapshot();
  });
  core.command("node.action", "node.manage", [this](const json& p) {
    fields(p, {"id", "service", "action"});
    nodes.at(text(p, "id"))->action(text(p, "service"), text(p, "action"));
    return snapshot();
  });
}
} // namespace asterion::terminal
