#include "task_host.hpp"
#include "task_configuration.hpp"
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/logger.hpp>
#include <asterion/kernel/native_plugin.hpp>
#include <CLI/CLI.hpp>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
using namespace std::chrono_literals;

int main(int argc, char** argv)
{
    CLI::App app{"Asterion durable task service (service-owned execution)"};
    app.set_version_flag("--version", "asterion_task_service " ASTERION_PRODUCT_VERSION);
    std::string directory, service, health_endpoint, data_instance, data_endpoint;
    std::string configuration;
    app.add_option("--config", configuration, "Service instance task policy")->required();
    unsigned file_workers = 2;
    app.add_option("--file-workers", file_workers)->check(CLI::Range(1, 2));
    std::uint64_t owner_pid = 0;
    asterion::service::Transport transport;
    app.add_option("--directory", directory)->check(CLI::ExistingDirectory);
    app.add_option("--endpoint", transport.endpoint);
    app.add_option("--session", service);
    app.add_option("--bind", transport.bind);
    app.add_option("--port", transport.port)->check(CLI::Range(1, 65535));
    app.add_option("--tls-ca", transport.tls.ca_file);
    app.add_option("--tls-cert", transport.tls.certificate_file);
    app.add_option("--tls-key", transport.tls.private_key_file);
    app.add_option("--data-instance", data_instance)->required();
    app.add_option("--data-endpoint", data_endpoint)->required();
    app.add_option("--health-endpoint", health_endpoint);
    app.add_option("--owner-pid", owner_pid);
    argv = app.ensure_utf8(argv);
    std::string plugin_directory;
    app.add_option("--plugin-directory", plugin_directory)->check(CLI::ExistingDirectory);
    CLI11_PARSE(app, argc, argv);
    if (argc == 1) {
        std::cerr
            << "asterion_task_service: supply --session, --directory and a " "private endpoint or TLS listener.\n";
        return 3;
    }
    try {
        asterion::initialize_process_logging("task-service", service);
        if (!plugin_directory.empty())
            asterion::configure_native_plugins(plugin_directory);
        asterion::validate_id(service);
        asterion::validate_id(data_instance);
        if (service == data_instance || data_endpoint.empty())
            throw std::invalid_argument(
                "task service requires a distinct data instance and private endpoint");
        if (directory.empty())
            throw std::invalid_argument("a task directory is required");
        transport.validate();
        asterion::service::install_stop_signals();
        using Host = asterion::service::RpcHost;
        using asterion::tasks::TaskHost;
        TaskHost tasks(
            {directory,
                service,
                data_instance,
                data_endpoint,
                file_workers,
                asterion::tasks::load_task_concurrency(configuration)});
        Host::Options options;
        options.owner_pid = owner_pid;
        options.connections = 12;
        options.handshake = 3s;
        options.receive = 10s;
        options.send = 3s;
        options.request_bytes = asterion::ipc::Channel::max_frame;
        options.payload_bytes = 128 * 1024 * 1024;
        if (!health_endpoint.empty())
            options.local_endpoints.push_back(
                {health_endpoint,
                    [&](const Host::Peer&, std::string frame) {
                        return tasks.accept(std::move(frame), TaskHost::Lane::health);
                    },
                    4,
                    true,
                    65536,
                    4 * 65536});
        options.advance = [&](Host::Stage stage) {
            return tasks.advance(stage);
        };
        asterion::Progress io_progress;
        Host host(
            transport,
            [&](const Host::Peer&, std::string frame) {
                return tasks.accept(std::move(frame), TaskHost::Lane::client);
            },
            std::move(options),
            io_progress);
        tasks.open();
        try {
            if (!host.run())
                std::_Exit(0);
        } catch (const std::exception& error) {
            asterion::log_process_event(
                "task-service",
                asterion::LogLevel::error,
                "service.failed",
                {{"message", error.what()}});
            std::cerr << "Task service failed: " << error.what() << '\n';
            std::_Exit(1);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Task service failed: " << error.what() << '\n';
        asterion::log_process_event(
            "task-service",
            asterion::LogLevel::error,
            "service.failed",
            {{"message", error.what()}});
        return 1;
    }
}
