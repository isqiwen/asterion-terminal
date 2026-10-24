#include "node_program.hpp"
#include "node_service.hpp"
#include "agent_client.hpp"
#include "market_client.hpp"
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/file_lock.hpp>
#include <CLI/CLI.hpp>
#include <asterion/kernel/environment.hpp>
#include <chrono>
#include <iostream>
#include <thread>

int main(int argc, char** argv)
{
    CLI::App app{"Native user-service control acceptance"};
    std::string operation, executable, root, endpoint, name, source, expected, provider;
    std::uint64_t pid = 0;
    std::string enrollment;
    app.add_option("--enrollment-directory", enrollment);
    app.add_option("--operation", operation)
        ->required()
        ->check(
            CLI::IsMember(
                {"install",
                    "register",
                    "start",
                    "bootstrap",
                    "stop",
                    "replace",
                    "verify-stopped",
                    "deploy-market",
                    "deploy-task",
                    "market-status",
                    "status"}));
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
        asterion::terminal::ServiceIo io;
        std::vector<std::string> arguments;
        if (!enrollment.empty())
            arguments = {"--enrollment-directory", enrollment};
        if (name.rfind("me.asterion.acceptance.", 0) != 0)
            throw std::invalid_argument("test requires an isolated acceptance service name");
        auto path = [](const std::string& value) {
            return std::filesystem::path(std::u8string(value.begin(), value.end()));
        };
        if (operation == "deploy-market" || operation == "deploy-task"
            || operation == "market-status" || operation == "status") {
            auto client =
                asterion::terminal::AgentClient::open(io, {"local", "localhost", 0, {}, endpoint})
                    .get();
            if (operation == "deploy-task") {
                const auto isolated = asterion::environment_path("ASTERION_NODE_DIRECTORY");
                if (asterion::environment_variable("ASTERION_TEST_NODE_ISOLATED") != "1"
                    || !isolated
                    || std::filesystem::canonical(*isolated) != std::filesystem::canonical(root))
                    throw std::invalid_argument("task fixture requires isolated node");
                for (const auto& id : {"other-task", "stopped-task"}) {
                    client
                        ->deploy(
                            {.service = id,
                                .kind = asterion::node::proto::RESEARCH,
                                .platform = asterion::current_platform(),
                                .programs = asterion::terminal::local_service_programs(
                                    asterion::node::proto::RESEARCH)})
                        .get();
                }
                // Stop only after the task service has started once: a ledger that never
                // created manager.lock is refused by the read-only usage check.
                const auto lock = std::filesystem::path(root) / "services" / "stopped-task"
                    / "ledger" / "manager.lock";
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
                while (
                    !std::filesystem::exists(lock) && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                if (!std::filesystem::exists(lock))
                    throw std::runtime_error("stopped-task did not start");
                client->action("stopped-task", "stop").get();
            }
            if (operation == "deploy-market") {
                const auto platform = asterion::current_platform();
                client
                    ->deploy(
                        {.service = "market-running",
                            .kind = asterion::node::proto::MARKET_DATA,
                            .platform = platform,
                            .programs = {.executable = path(source), .provider = path(provider)}})
                    .get();
                client
                    ->deploy(
                        {.service = "market-stopped",
                            .kind = asterion::node::proto::MARKET_DATA,
                            .platform = platform,
                            .programs = {.executable = path(source), .provider = path(provider)}})
                    .get();
                client->action("market-stopped", "stop").get();
                auto market = asterion::terminal::MarketClient::open(
                    io,
                    client->service_endpoint("market-running", asterion::node::proto::MARKET_DATA)
                        .get())
                                  .get();
                // A no-op Agent upgrade must preserve this session. The SDK double's
                // default user injects a timed reconnect, which would race that check.
                market
                    ->connect(
                        {{"front", "tcp://127.0.0.1:12345"},
                            {"broker", "test"},
                            {"user", "steady"},
                            {"password", "fixture"},
                            {"instruments", asterion::Json::array()}})
                    .get();
            }
            if (operation == "market-status") {
                auto market = asterion::terminal::MarketClient::open(
                    io,
                    client->service_endpoint("market-running", asterion::node::proto::MARKET_DATA)
                        .get())
                                  .get();
                std::cout << market->snapshot().get().dump() << '\n';
                return 0;
            }
            std::cout << client->status().get().dump() << '\n';
        } else if (operation == "verify-stopped")
            asterion::terminal::verify_node_service_stopped(
                path(executable),
                path(root),
                endpoint,
                name,
                arguments);
        else if (operation == "replace")
            asterion::terminal::replace_node_program(
                path(source),
                path(executable),
                path(root),
                expected);
        else if (operation == "bootstrap") {
            asterion::FileLock owner(path(root), "bootstrap.lock");
            asterion::terminal::install_node_program(path(source), path(executable), path(root));
            asterion::terminal::install_node_service(
                path(executable),
                path(root),
                endpoint,
                name,
                arguments);
        } else if (operation == "start")
            asterion::terminal::start_node_service(name);
        else if (operation == "install" || operation == "register")
            asterion::terminal::install_node_service(
                path(executable),
                path(root),
                endpoint,
                name,
                arguments,
                operation == "install");
        else
            asterion::terminal::stop_node_service(
                path(executable),
                path(root),
                endpoint,
                pid,
                name,
                arguments);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
