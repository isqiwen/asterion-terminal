#include "node_program.hpp"
#include "node_service.hpp"
#include "node_client.hpp"
#include "market_client.hpp"
#include <asterion/kernel/process/artifact.hpp>
#include <CLI/CLI.hpp>
#include <asterion/kernel/environment.hpp>
#include <chrono>
#include <iostream>
#include <thread>
int main(int argc, char** argv) {
  CLI::App app{"Native user-service control acceptance"};
  std::string operation, executable, root, endpoint, name, source, expected, provider;
  std::uint64_t pid = 0;
  app.add_option("--operation", operation)
      ->required()
      ->check(CLI::IsMember({"install", "stop", "replace", "upgrade", "inspect", "verify-stopped",
                             "deploy-market", "deploy-research", "market-status", "status"}));
  app.add_option("--executable", executable)->required();
  app.add_option("--root", root)->required();
  app.add_option("--endpoint", endpoint)->required();
  app.add_option("--name", name)->required();
  app.add_option("--pid", pid);
  app.add_option("--source", source);
  app.add_option("--provider", provider);
  app.add_option("--expected", expected);
  argv = app.ensure_utf8(argv);
  CLI11_PARSE(app, argc, argv);
  try {
    if (name.rfind("me.asterion.acceptance.", 0) != 0)
      throw std::invalid_argument("test requires an isolated acceptance service name");
    auto path = [](const std::string& value) {
      return std::filesystem::path(std::u8string(value.begin(), value.end()));
    };
    if (operation == "deploy-market" || operation == "deploy-research" ||
        operation == "market-status" || operation == "status") {
      asterion::terminal::NodeClient client({"local", "localhost", 0, {}, endpoint});
      if (operation == "deploy-research") {
        const auto isolated = asterion::environment_path("ASTERION_NODE_DIRECTORY");
        if (asterion::environment_variable("ASTERION_TEST_NODE_ISOLATED") != "1" || !isolated ||
            std::filesystem::canonical(*isolated) != std::filesystem::canonical(root))
          throw std::invalid_argument("research fixture requires isolated node");
        for (const auto& id : {"other-research", "stopped-research"}) {
          client.deploy({.service = id,
                         .kind = asterion::node::v1::TASK_SERVICE,
                         .platform = asterion::current_platform(),
                         .programs = asterion::terminal::local_service_programs(
                             asterion::node::v1::TASK_SERVICE)});
        }
        // Stop only after the task service has started once: a ledger that never
        // created manager.lock is refused by the read-only usage check.
        const auto lock = std::filesystem::path(root) / "services" / "stopped-research" / "ledger" /
                          "manager.lock";
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!std::filesystem::exists(lock) && std::chrono::steady_clock::now() < deadline)
          std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (!std::filesystem::exists(lock))
          throw std::runtime_error("stopped-research did not start");
        client.action("stopped-research", "stop");
      }
      if (operation == "deploy-market") {
        const auto platform = asterion::current_platform();
        client.deploy({.service = "market-running",
                       .kind = asterion::node::v1::MARKET_DATA,
                       .platform = platform,
                       .programs = {.executable = path(source), .provider = path(provider)}});
        client.deploy({.service = "market-stopped",
                       .kind = asterion::node::v1::MARKET_DATA,
                       .platform = platform,
                       .programs = {.executable = path(source), .provider = path(provider)}});
        client.action("market-stopped", "stop");
        asterion::terminal::MarketClient market(
            client.service_endpoint("market-running", asterion::node::v1::MARKET_DATA));
        market.connect({{"front", "tcp://127.0.0.1:12345"},
                        {"broker", "test"},
                        {"user", "test"},
                        {"password", "fixture"},
                        {"instruments", asterion::Json::array()}});
      }
      if (operation == "market-status") {
        asterion::terminal::MarketClient market(
            client.service_endpoint("market-running", asterion::node::v1::MARKET_DATA));
        std::cout << market.snapshot().dump() << '\n';
        return 0;
      }
      std::cout << client.status().dump() << '\n';
    } else if (operation == "verify-stopped")
      asterion::terminal::verify_node_service_stopped(path(executable), path(root), endpoint, name);
    else if (operation == "inspect")
      std::cout << asterion::terminal::inspect_node_program(path(source), path(executable),
                                                            path(root))
                       .dump()
                << '\n';
    else if (operation == "upgrade")
      asterion::terminal::upgrade_node_service(path(source), path(executable), path(root), endpoint,
                                               expected, name);
    else if (operation == "replace")
      asterion::terminal::replace_node_program(path(source), path(executable), path(root),
                                               expected);
    else if (operation == "install")
      asterion::terminal::install_node_service(path(executable), path(root), endpoint, name);
    else
      asterion::terminal::stop_node_service(path(executable), path(root), endpoint, pid, name);
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
