#include <asterion/version.hpp>
#include "ctp_catalog.hpp"
#include "service.hpp"
#include <asterion/base/error.hpp>
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
    CLI::App app{"Asterion live quotes service"};
    app.set_version_flag(
        "--version",
        std::string("Asterion Market ") + asterion::build::description);
    bool check_login = false;
    app.add_flag(
        "--check-login",
        check_login,
        "Verify CTP credentials with a single login attempt and exit");
    argv = app.ensure_utf8(argv);
    CLI11_PARSE(app, argc, argv);
    try {
        const auto installation = asterion::installation_directory();
        if (check_login) {
            const auto settings = asterion::market::load_settings(
                installation,
                environment_path("CREDENTIALS_DIRECTORY"));
            const auto flow = std::filesystem::temp_directory_path()
                / ("asterion-login-check-" + unique_process_id());
            struct Cleanup
            {
                ~Cleanup()
                {
                    std::error_code ignored;
                    std::filesystem::remove_all(path, ignored);
                }

                std::filesystem::path path;
            } cleanup{flow};
            try {
                const auto trading_day = ctp::verify_ctp_login(
                    settings.ctp_catalog_library,
                    flow,
                    settings.catalog);
                std::cout << "CTP login verified (trading_day=" << trading_day << ")\n";
                return 0;
            } catch (const Error& error) {
                std::cerr << error.what() << '\n';
                return error.code() == ErrorCode::unavailable ? 2 : 1;
            }
        }
        asterion::log::init();
        const auto settings = asterion::market::load_settings(
            installation,
            environment_path("CREDENTIALS_DIRECTORY"));
#if defined(__linux__) && !defined(ASTERION_SANITIZED)
        asterion::install_crash_logging(installation / "logs", "asterion_market");
#endif
        FileLock owner(settings.directory, "market.lock");
        service::Transport transport;
        transport.bind = settings.bind;
        transport.port = settings.port;
        transport.tls = settings.tls;
        transport.validate();
        service::install_stop_signals();
        using Host = service::RpcHost;
        Progress io_progress;
        std::unique_ptr<asterion::market::MarketService> service;
        Host::Options options;
        options.connections = 64;
        options.send = 2s;
        options.drain = 20s;
        options.advance = [&](Host::Stage stage) {
            if (stage != Host::Stage::running)
                service->drain();
            if (stage == Host::Stage::stopping_resources)
                service->stop();
            const auto stopped = service->stopped();
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
                return service->serve(
                    std::move(frame),
                    settings.clients.contains(peer.fingerprint));
            },
            std::move(options),
            io_progress);
        // Listen before starting resources whose teardown may wait for a provider.
        service = std::make_unique<asterion::market::MarketService>(settings);
        try {
            if (!host.run())
                std::_Exit(1);
        } catch (const std::exception& error) {
            log::error("market service failed: {}", error_name(classify(error)));
            std::_Exit(1);
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        if (!check_login)
            log::error("market service failed: {}", e.what());
        return 1;
    }
    return 0;
}
