#include "notice_host.hpp"
#include <asterion/kernel/logger.hpp>
#include <CLI/CLI.hpp>
#include <iostream>
#include <cstdlib>

int main(int argc, char** argv)
{
    CLI::App app{"Asterion human-reviewed intelligence service"};
    app.set_version_flag("--version", "asterion_intelligence " ASTERION_PRODUCT_VERSION);
    std::string directory, service, node, discovery, artifact, health, local_endpoint;
    std::uint64_t owner_pid = 0;
    asterion::service::Transport transport;
    app.add_option("--directory", directory)->required()->check(CLI::ExistingDirectory);
    app.add_option("--session", service)->required();
    app.add_option("--node-id", node)->required();
    app.add_option("--program-artifact", artifact)->required();
    app.add_option("--discovery-endpoint", discovery)->required();
    app.add_option("--endpoint", transport.endpoint);
    app.add_option("--local-endpoint", local_endpoint);
    app.add_option("--bind", transport.bind);
    app.add_option("--port", transport.port)->check(CLI::Range(1, 65535));
    app.add_option("--tls-ca", transport.tls.ca_file);
    app.add_option("--tls-cert", transport.tls.certificate_file);
    app.add_option("--tls-key", transport.tls.private_key_file);
    app.add_option("--health-endpoint", health);
    app.add_option("--owner-pid", owner_pid);
    argv = app.ensure_utf8(argv);
    CLI11_PARSE(app, argc, argv);
    try {
        asterion::initialize_process_logging("intelligence-service", service);
        transport.validate();
        asterion::service::install_stop_signals();
        asterion::resource::proto::Owner owner;
        owner.set_node_id(node);
        owner.set_service_id(service);
        owner.set_domain("intelligence");
        asterion::intelligence::NoticeHost business(
            {std::filesystem::absolute(directory), owner, discovery, artifact});
        using Host = asterion::service::RpcHost;
        Host::Options options;
        options.owner_pid = owner_pid;
        options.connections = 8;
        options.request_bytes = 1024 * 1024;
        options.payload_bytes = 16 * 1024 * 1024;
        options.advance = [&](Host::Stage stage) {
            return business.advance(stage);
        };
        if (!health.empty())
            options.local_endpoints.push_back(
                {health,
                    [&](const Host::Peer&, std::string frame) {
                        return business.accept(std::move(frame), true);
                    },
                    4,
                    true,
                    65536,
                    4 * 65536});
        if (!local_endpoint.empty())
            options.local_endpoints.push_back(
                {local_endpoint,
                    [&](const Host::Peer&, std::string frame) {
                        return business.accept(std::move(frame));
                    },
                    6,
                    false,
                    1024 * 1024,
                    16 * 1024 * 1024});
        asterion::Progress progress;
        Host host(
            transport,
            [&](const Host::Peer&, std::string frame) {
                return business.accept(std::move(frame));
            },
            std::move(options),
            progress);
        business.open();
        if (!host.run())
            std::_Exit(0); // Pending worker captures must not outlive drained reply frames.
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Intelligence service failed: " << e.what() << '\n';
        return 1;
    }
}
