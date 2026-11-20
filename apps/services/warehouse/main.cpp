#include <asterion/version.hpp>
#include "gc.hpp"
#include "service.hpp"
#include <asterion/system/environment.hpp>
#include <CLI/CLI.hpp>
#include <asterion/system/log.hpp>
#include <asterion/system/crash.hpp>
#include <asterion/system/process/child.hpp>
#include <asterion/system/process/file_lock.hpp>
#include <cstdlib>
#include <iostream>
#include <nlohmann/json.hpp>
using namespace asterion;
using namespace std::chrono_literals;

int main(int argc, char** argv)
{
    CLI::App app{"Asterion historical data service"};
    app.set_version_flag(
        "--version",
        std::string("Asterion Warehouse ") + asterion::build::description);

    auto* gc = app.add_subcommand(
        "gc",
        "Safely reclaim unreferenced Parquet partitions and old versions (offline)");
    std::string gc_data_dir;
    std::size_t gc_keep_last = 5;
    std::size_t gc_keep_source_last = 1;
    std::vector<std::string> gc_keep_versions;
    std::string gc_keep_file;
    bool gc_apply = false;
    gc->add_option(
        "--data-dir",
        gc_data_dir,
        "Warehouse data directory (defaults to configured data directory)");
    gc->add_option("--keep-last", gc_keep_last, "Recent research versions to keep (min 1)")
        ->default_val(5);
    gc->add_option(
          "--keep-source-last",
          gc_keep_source_last,
          "Recent per-source versions to keep (min 1)")
        ->default_val(1);
    gc->add_option(
        "--keep-version",
        gc_keep_versions,
        "Pinned research dataset version SHA-256 to protect (repeatable)");
    gc->add_option(
        "--keep-file",
        gc_keep_file,
        "File listing pinned research dataset versions (one per line)");
    gc->add_flag("--apply", gc_apply, "Actually delete unreferenced files (default is dry-run)");

    argv = app.ensure_utf8(argv);
    CLI11_PARSE(app, argc, argv);
    try {
        const auto installation = asterion::installation_directory();
        if (gc->parsed()) {
            warehouse::GcOptions options;
            options.data_directory = gc_data_dir.empty()
                ? asterion::warehouse::load_data_directory(installation)
                : std::filesystem::weakly_canonical(gc_data_dir);
            options.keep_last = gc_keep_last;
            options.keep_source_last = gc_keep_source_last;
            options.keep_versions.insert(gc_keep_versions.begin(), gc_keep_versions.end());
            if (!gc_keep_file.empty()) {
                const auto from_file = warehouse::read_keep_versions_file(gc_keep_file);
                options.keep_versions.insert(from_file.begin(), from_file.end());
            }
            options.apply = gc_apply;
            const auto report = warehouse::collect_garbage(options);
            nlohmann::json out{{"applied", report.applied}, {"stores", nlohmann::json::array()}};
            for (const auto& item : report.stores) {
                out["stores"].push_back(
                    {{"provider", item.provider},
                        {"kept_versions", item.kept_versions},
                        {"removed_versions", item.removed_versions},
                        {"kept_shards", item.kept_shards},
                        {"removed_shards", item.removed_shards},
                        {"kept_files", item.kept_files},
                        {"removed_files", item.removed_files},
                        {"unlinked_bytes", item.unlinked_bytes},
                        {"freed_bytes", item.freed_bytes}});
            }
            std::cout << out.dump(2) << '\n';
            return 0;
        }
        asterion::log::init();
        const auto settings = asterion::warehouse::load_settings(
            installation,
            environment_path("CREDENTIALS_DIRECTORY"));
#if defined(__linux__) && !defined(ASTERION_SANITIZED)
        asterion::install_crash_logging(installation / "logs", "asterion_warehouse");
#endif
        FileLock owner(settings.directory, "warehouse.lock");
        service::Transport transport;
        transport.bind = settings.bind;
        transport.port = settings.port;
        transport.tls = settings.tls;
        transport.validate();
        service::install_stop_signals();
        using Host = service::RpcHost;
        Progress io_progress;
        std::unique_ptr<warehouse::WarehouseService> service;
        Host::Options options;
        options.connections = 64;
        options.send = 2s;
        options.drain = 20s;
        options.advance = [&](Host::Stage stage) {
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
        service = std::make_unique<asterion::warehouse::WarehouseService>(settings);
        try {
            if (!host.run())
                std::_Exit(1);
        } catch (const std::exception& error) {
            log::error("warehouse service failed: {}", error_name(classify(error)));
            std::_Exit(1);
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        log::error("warehouse service failed: {}", e.what());
        return 1;
    }
    return 0;
}
