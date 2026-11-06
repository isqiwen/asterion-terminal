#include <asterion/version.hpp>
#include "service.hpp"
#include <asterion/system/environment.hpp>
#include <CLI/CLI.hpp>
#include <asterion/system/log.hpp>
#include <asterion/system/crash.hpp>
#include <asterion/system/process/child.hpp>
#include <asterion/system/process/file_lock.hpp>
#include <cstdlib>
#include <iostream>
using namespace asterion;
using namespace std::chrono_literals;

int main(int argc, char** argv)
{
    CLI::App app{"Asterion market-data and acquisition host"};
    app.set_version_flag(
        "--version",
        std::string("Asterion History Data ") + asterion::build::description);
    argv = app.ensure_utf8(argv);
    CLI11_PARSE(app, argc, argv);
    try {
        const auto installation = asterion::installation_directory();
        asterion::log::init();
        const auto settings = asterion::history::load_settings(
            installation,
            environment_path("CREDENTIALS_DIRECTORY"));
#if defined(__linux__) && !defined(ASTERION_SANITIZED)
        asterion::install_crash_logging(installation / "logs", "asterion_history");
#endif
        FileLock owner(settings.directory, "history.lock");
        service::Transport transport;
        transport.bind = settings.bind;
        transport.port = settings.port;
        transport.tls = settings.tls;
        transport.validate();
        service::install_stop_signals();
        using Host = service::RpcHost;
        Progress io_progress;
        std::unique_ptr<asterion::history::HistoryService> market;
        Host::Options options;
        options.connections = 64;
        options.send = 2s;
        options.drain = 20s;
        options.advance = [&](Host::Stage stage) {
            if (stage != Host::Stage::running)
                market->drain();
            if (stage == Host::Stage::stopping_resources)
                market->stop();
            const auto stopped = market->stopped();
            if (stopped.wait_for(0ms) != std::future_status::ready)
                return false;
            stopped.get();
            if (stage == Host::Stage::running)
                service::request_stop();
            return true;
        };
        Host host(
            transport,
            [&](const Host::Peer& peer, std::string frame) {
                const auto client = settings.clients.find(peer.fingerprint);
                return market->serve(
                    std::move(frame),
                    client == settings.clients.end() ? std::nullopt
                                                     : std::optional<bool>(client->second));
            },
            std::move(options),
            io_progress);
        // Listen before starting resources whose teardown may wait for a provider.
        market = std::make_unique<asterion::history::HistoryService>(settings);
        try {
            if (!host.run())
                std::_Exit(1);
        } catch (const std::exception& error) {
            log::error("market service failed: {}", error_name(classify(error)));
            std::_Exit(1);
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        log::error("market service failed: {}", e.what());
        return 1;
    }
    return 0;
}
