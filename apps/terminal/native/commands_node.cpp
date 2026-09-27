#include "application_impl.hpp"

namespace asterion::terminal {
// Node lifecycle: SSH enrollment, firewall, Agent upgrade, service deployment.
void Application::Impl::register_node_commands() {
  core.access().grant("terminal.local", "node.manage");
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
    firewall_plan =
        nodes.at(text(p, "id"))
            ->firewall(text(p, "service"), text(p, "action"), p.at("token").get<std::string>());
    return snapshot();
  });
  core.command("node.firewall.inspect", "node.manage", [this](const json& p) {
    fields(p, {"id", "host", "ssh_port", "username", "key_source", "private_key", "known_hosts",
               "agent_port", "firewall_port", "firewall_action"});
    firewall_plan = nullptr;
    firewall_parameters = nullptr;
    auto plan = inspect_node_firewall(p);
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
    firewall_plan = change_node_firewall(parameters, plan);
    return snapshot();
  });
  core.command("node.bootstrap", "node.manage", [this](const json& p) {
    fields(p, {"id", "host", "ssh_port", "username", "key_source", "private_key", "known_hosts",
               "agent_port"});
    auto config = enroll_node(p);
    const auto id = config.id;
    nodes.insert_or_assign(id, std::make_unique<NodeClient>(config));
    return snapshot();
  });
  core.command("node.connect", "node.manage", [this](const json& p) {
    fields(p, {"id"});
    auto config = enrolled_node(text(p, "id"));
    const auto id = config.id;
    nodes.insert_or_assign(id, std::make_unique<NodeClient>(config));
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
      const auto endpoint = upgrade_local_node(expected);
      nodes.erase("local");
      nodes.emplace("local", std::make_unique<NodeClient>(endpoint));
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
      nodes.emplace("local", std::make_unique<NodeClient>(local_node()));
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
    if (paper)
      throw std::invalid_argument("disconnect the current trading session first");
    paper = std::make_unique<TradingClient>(
        nodes.at(text(p, "id"))->service_endpoint(text(p, "service")));
    return snapshot();
  });
  core.command("node.deploy", "node.manage", [this](const json& p) {
    fields(p, {"id", "service", "port", "kind"});
    auto& node = *nodes.at(text(p, "id"));
    const auto status = node.status();
    if (status.at("state") != "online" || status.at("health").at("os") != "linux" ||
        status.at("health").at("version") != ASTERION_PRODUCT_VERSION)
      throw std::invalid_argument(
          "remote services require an online Linux Agent of the same version");
    const auto arch = status.at("health").at("arch").get<std::string>();
    const auto kind = text(p, "kind");
    if (kind != "paper" && kind != "market" && kind != "research")
      throw std::invalid_argument("invalid service kind");
    node.deploy(bundled_linux_program(arch, kind == "market"     ? "asterion-market-data"
                                            : kind == "research" ? "asterion-task-service"
                                                                 : "asterion-trading"),
                "linux", arch, text(p, "service"), port_number(p, "port"), {}, kind,
                kind == "market" && arch == "x86_64" ? bundled_linux_program(arch, "ctp-md.so")
                                                     : std::filesystem::path{},
                kind == "research" ? bundled_linux_program(arch, "asterion-backtest")
                                   : std::filesystem::path{},
                kind == "research" ? bundled_linux_program(arch, "asterion-factor")
                                   : std::filesystem::path{},
                kind == "research" ? bundled_linux_program(arch, "asterion-data-pipeline")
                                   : std::filesystem::path{});
    return snapshot();
  });
  core.command("node.update", "node.manage", [this](const json& p) {
    fields(p, {"id", "service", "revision"});
    const auto id = text(p, "id"), service = text(p, "service");
    auto& node = *nodes.at(id);
    const auto state = node.status();
    if (state.at("state") != "online")
      throw std::invalid_argument("connect the node before updating");
    const auto& health = state.at("health");
    const auto os = text(health, "os"), arch = text(health, "arch");
    std::string kind;
    for (const auto& s : health.at("services"))
      if (s.at("id") == service)
        kind = text(s, "kind");
    if (kind != "paper" && kind != "market" && kind != "research" && kind != "strategy")
      throw std::invalid_argument("unknown service kind");
    auto program = [&](const char* variable, const char* name) {
      if (id != "local")
        return bundled_linux_program(arch, name);
      const auto* configured = std::getenv(variable);
      return configured ? std::filesystem::path(
                              std::u8string(configured, configured + std::strlen(configured)))
                        : current_executable().parent_path() /
                              (std::string(name) + (os == "windows" ? ".exe" : ""));
    };
    const auto executable =
        kind == "paper"      ? program("ASTERION_TRADING_EXECUTABLE", "asterion-trading")
        : kind == "market"   ? program("ASTERION_MARKET_EXECUTABLE", "asterion-market-data")
        : kind == "research" ? program("ASTERION_TASK_EXECUTABLE", "asterion-task-service")
                             : program("ASTERION_STRATEGY_EXECUTABLE", "asterion-strategy");
    std::filesystem::path provider;
    if (kind == "market") {
      if (id != "local")
        provider = bundled_linux_program(arch, "ctp-md.so");
      else {
        const auto* configured = std::getenv("ASTERION_CTP_LIBRARY");
        provider = configured ? std::filesystem::path(
                                    std::u8string(configured, configured + std::strlen(configured)))
                              : current_executable().parent_path() /
                                    ("ctp-md" + std::string(os == "windows" ? ".dll"
                                                            : os == "macos" ? ".dylib"
                                                                            : ".so"));
      }
    }
    node.update(executable, os, arch, service, text(p, "revision"), provider,
                kind == "research" ? program("ASTERION_BACKTEST_EXECUTABLE", "asterion-backtest")
                                   : std::filesystem::path{},
                kind == "research" ? program("ASTERION_FACTOR_EXECUTABLE", "asterion-factor")
                                   : std::filesystem::path{},
                kind == "research"
                    ? program("ASTERION_DATA_PIPELINE_EXECUTABLE", "asterion-data-pipeline")
                    : std::filesystem::path{});
    return snapshot();
  });
  core.command("node.action", "node.manage", [this](const json& p) {
    fields(p, {"id", "service", "action"});
    nodes.at(text(p, "id"))->action(text(p, "service"), text(p, "action"));
    return snapshot();
  });
}
} // namespace asterion::terminal
