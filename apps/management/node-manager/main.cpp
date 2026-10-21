#include "manager.hpp"
#include "node_paths.hpp"
#include <asterion/manager.pb.h>
#include <asterion/kernel/environment.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/logger.hpp>
#include <asterion/kernel/process/file_lock.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/owner.hpp>
#include <CLI/CLI.hpp>
#include <iostream>
#include <unistd.h>

int main(int argc, char** argv)
{
    CLI::App app{"Asterion Node Manager"};
    app.set_version_flag("--version", "asterion_node_manager " ASTERION_PRODUCT_VERSION);
    std::string directory, enrollment;
    bool development = false, isolated = false;
    asterion::service::Transport transport;
    app.add_option("--directory", directory)->required()->check(CLI::ExistingDirectory);
    app.add_option("--endpoint", transport.endpoint)->required();
    app.add_option("--enrollment-directory", enrollment)->required();
    app.add_flag("--development", development);
    app.add_flag("--isolated", isolated);
    argv = app.ensure_utf8(argv);
    CLI11_PARSE(app, argc, argv);
    using namespace asterion;
    try {
        const auto root = std::filesystem::path(directory);
        FileLock ownership(root, "manager.lock");
        const auto node = root.parent_path();
        if (!node.is_absolute() || !std::filesystem::path(enrollment).is_absolute())
            throw std::invalid_argument("invalid node manager configuration");
        ::setenv("ASTERION_ENVIRONMENT", development ? "development" : "production", 1);
        if (isolated)
            ::setenv("ASTERION_NODE_DIRECTORY", node.c_str(), 1);
        else
            ::unsetenv("ASTERION_NODE_DIRECTORY");
        terminal::configure_node_paths(node, enrollment);
        // Service and plugin resolvers already use siblings of this executable.
        // Isolated child processes inherit explicit fixture resource overrides.
        if (!environment_path("ASTERION_REMOTE_RESOURCES"))
            ::setenv(
                "ASTERION_REMOTE_RESOURCES",
                (current_executable().parent_path().parent_path() / "remote-linux").c_str(),
                1);
        ::setenv("ASTERION_LOG_DIRECTORY", (node / "logs").c_str(), 1);
        initialize_process_logging("node-manager");
        transport.validate();
        service::install_stop_signals();
        replace_file_durably(root / "manager.pid", std::to_string(current_process_id()));
        terminal::NodeManager manager;
        service::RpcHost::Options options;
        options.connections = 16;
        if (isolated) {
            if (const auto owner = environment_variable("ASTERION_TEST_OWNER_PID")) {
                const auto test_owner = std::stoull(*owner);
                options.advance = [test_owner, next = std::chrono::steady_clock::now()](
                                      service::RpcHost::Stage stage) mutable {
                    if (stage == service::RpcHost::Stage::running
                        && std::chrono::steady_clock::now() >= next) {
                        next = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
                        if (!process_running(test_owner))
                            service::request_stop();
                    }
                    return true;
                };
            } else
                options.owner_pid = static_cast<std::uint64_t>(::getppid());
        }
        Progress progress;
        service::RpcHost host(
            transport,
            [&](const service::RpcHost::Peer&, std::string frame) {
                return manager.accept(std::move(frame));
            },
            std::move(options),
            progress);
        manager.open();
        log_process_event("manager", LogLevel::info, "manager.started");
        if (!host.run())
            std::_Exit(0);
        log_process_event("manager", LogLevel::info, "manager.stopped");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Node Manager failed: " << error.what() << '\n';
        log_process_event(
            "manager",
            LogLevel::error,
            "manager.failed",
            {{"message", error.what()}});
        return 1;
    }
}
