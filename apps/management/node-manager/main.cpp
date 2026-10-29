#include "manager.hpp"
#include "node_paths.hpp"
#include "application_environment.hpp"
#include <asterion/kernel/native_plugin.hpp>
#include <asterion/manager.pb.h>
#include <asterion/kernel/environment.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/log.hpp>
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
    std::string enrollment, python, application;
    bool development = false, isolated = false;
    asterion::service::Transport transport;
    app.add_option("--endpoint", transport.endpoint)->required();
    app.add_option("--enrollment-directory", enrollment)->required();
    app.add_flag("--development", development);
    app.add_flag("--isolated", isolated);
    app.add_option("--python", python, "The bundled Python interpreter the lab service runs");
    app.add_option(
        "--application",
        application,
        "The application's native directory: its bundled programs and plugins");
    argv = app.ensure_utf8(argv);
    CLI11_PARSE(app, argc, argv);
    using namespace asterion;
    try {
        // <root>/manager/bin/asterion_node_manager: its state is in data/, and
        // every program of this node is installed under <root>.
        const auto installation = installation_directory();
        const auto root = installation / "data";
        std::filesystem::create_directories(root);
        FileLock ownership(root, "manager.lock");
        const auto node = installation.parent_path();
        if (!node.is_absolute() || !std::filesystem::path(enrollment).is_absolute())
            throw std::invalid_argument("invalid node manager configuration");
        ::setenv("ASTERION_ENVIRONMENT", development ? "development" : "production", 1);
        if (isolated)
            ::setenv("ASTERION_ROOT", node.c_str(), 1);
        else
            ::unsetenv("ASTERION_ROOT");
        terminal::configure_node_paths(node, enrollment);
        // Bundled programs, plugins and remote resources are in the application,
        // not beside this copy of the Manager.
        if (!application.empty()) {
            terminal::set_application_directory(application);
            if (std::filesystem::is_directory(std::filesystem::path(application) / "plugins"))
                configure_native_plugins(std::filesystem::path(application) / "plugins");
        }
        // A development Manager runs away from the runtime; local program
        // preparation reads this like other overrides.
        if (!python.empty()) {
            if (!std::filesystem::path(python).is_absolute())
                throw std::invalid_argument("invalid bundled Python path: " + python);
            ::setenv("ASTERION_PYTHON", python.c_str(), 1);
        }
        // Isolated child processes inherit explicit fixture resource overrides.
        if (!environment_path("ASTERION_REMOTE_RESOURCES"))
            ::setenv(
                "ASTERION_REMOTE_RESOURCES",
                (terminal::application_directory().parent_path() / "remote-linux").c_str(),
                1);
        log::init();
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
        log::info("manager started");
        if (!host.run())
            std::_Exit(0);
        log::info("manager stopped");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Node Manager failed: " << error.what() << '\n';
        log::error("manager failed: {}", error.what());
        return 1;
    }
}
