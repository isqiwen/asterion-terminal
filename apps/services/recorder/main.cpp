#include "reader.hpp"
#include "recording.hpp"
#include <CLI/CLI.hpp>
#include <asterion/foundation/error.hpp>
#include <asterion/kernel/environment.hpp>
#include <asterion/kernel/logger.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/rpc_host.hpp>
#include <asterion/kernel/thread_pool.hpp>
#include <iostream>

using namespace asterion;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
namespace wire = asterion::recorder::proto;

namespace {

constexpr auto version = "0.1.0";

std::int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void fail(wire::Response& response, const std::exception& error)
{
    response.clear_body();
    response.mutable_error()->set_code(std::string(error_name(classify(error))));
    response.mutable_error()->set_message(
        dynamic_cast<const Error*>(&error) ? error.what() : "recording request rejected");
}

} // namespace

int main(int argc, char** argv)
{
    CLI::App app{"Asterion market recorder"};
    app.set_version_flag("--version", std::string("Asterion Recorder ") + version);
    std::string directory, configuration, sdk, catalog_sdk;
    service::Transport transport;
    app.add_option("--directory", directory)->required()->check(CLI::ExistingDirectory);
    app.add_option("--config", configuration, "Counter addresses and account")->required();
    app.add_option("--ctp-library", sdk)->required()->check(CLI::ExistingFile);
    app.add_option("--ctp-catalog-library", catalog_sdk)->required()->check(CLI::ExistingFile);
    app.add_option("--endpoint", transport.endpoint);
    app.add_option("--bind", transport.bind);
    app.add_option("--port", transport.port);
    app.add_option("--tls-ca", transport.tls.ca_file);
    app.add_option("--tls-cert", transport.tls.certificate_file);
    app.add_option("--tls-key", transport.tls.private_key_file);
    argv = app.ensure_utf8(argv);
    CLI11_PARSE(app, argc, argv);
    try {
        initialize_process_logging("recorder", "recorder");
        transport.validate();
        const fs::path root = fs::absolute(directory);
        auto login =
            recording::load_login(configuration, environment_path("CREDENTIALS_DIRECTORY"));
        const auto instance = unique_process_id();
        const auto started_ms = now_ms();
        service::install_stop_signals();
        using Host = service::RpcHost;
        Progress io_progress;
        std::unique_ptr<recording::Recording> recording;
        recording::Reader reader(root / "recordings");
        // File reads leave the I/O owner free; one at a time, a few queued.
        ThreadPool reads{1, 8};
        auto serve = [&](const Host::Peer&, std::string frame) -> Host::Reply {
            wire::Request request;
            wire::Response response;
            response.set_version(1);
            std::shared_ptr<std::future<wire::Response>> pending;
            try {
                if (!request.ParseFromString(frame) || request.version() != 1)
                    throw std::invalid_argument("malformed recorder request");
                validate_id(request.correlation_id());
                response.set_correlation_id(request.correlation_id());
                if (request.has_status()) {
                    const auto state = recording->status();
                    auto* status = response.mutable_status();
                    status->set_instance(instance);
                    status->set_version(version);
                    status->set_started_ms(started_ms);
                    status->set_phase(state.phase);
                    status->set_error_code(state.error_code);
                    status->set_catalog_phase(state.catalog_phase);
                    status->set_catalog_trading_day(state.catalog_trading_day);
                    status->set_catalog_error(state.catalog_error);
                    status->set_subscribed(static_cast<std::uint32_t>(state.subscribed));
                    status->set_last_quote_ms(state.last_quote_ms);
                    status->set_trading_day(state.written.trading_day);
                    status->set_rows(state.written.rows);
                    status->set_files(state.written.files);
                    status->set_bytes(state.written.bytes);
                    status->set_lost(state.written.lost);
                    status->set_write_error(state.written.error);
                    status->set_written_ms(state.written.written_ms);
                    status->set_disk_free_bytes(fs::space(root).available);
                    status->set_merging(state.merging);
                    status->set_merge_error(state.merge_error);
                } else if (request.has_days() || request.has_minutes()) {
                    auto promise = std::make_shared<std::promise<wire::Response>>();
                    auto result = promise->get_future();
                    reads.submit([&reader, request, response, promise](std::stop_token) mutable {
                        try {
                            if (request.has_days())
                                *response.mutable_days() = reader.days();
                            else
                                *response.mutable_minutes() = reader.minutes(request.minutes());
                        } catch (const std::exception& error) {
                            fail(response, error);
                        }
                        promise->set_value(std::move(response));
                        service::wake_io_owner();
                    });
                    pending = std::make_shared<std::future<wire::Response>>(std::move(result));
                } else
                    throw std::invalid_argument("missing recorder query");
            } catch (const std::exception& error) {
                fail(response, error);
            }
            return [pending, response]() -> std::optional<Host::Message> {
                if (!pending)
                    return Host::Message(response.SerializeAsString());
                if (pending->wait_for(0ms) != std::future_status::ready)
                    return {};
                return Host::Message(pending->get().SerializeAsString());
            };
        };
        Host::Options options;
        options.connections = 16;
        // The last files of a stop are written before the process exits.
        options.drain = 20s;
        options.advance = [&](Host::Stage stage) {
            const auto stopped = recording->stopped();
            const bool done = stopped.wait_for(0ms) == std::future_status::ready;
            // A loop that ended by itself failed: exit, and systemd starts it again.
            if (stage == Host::Stage::running) {
                if (done)
                    service::request_stop();
                return false;
            }
            if (stage == Host::Stage::stopping_resources)
                recording->stop();
            if (!done)
                return false;
            stopped.get();
            return true;
        };
        Host host(transport, serve, std::move(options), io_progress);
        recording = std::make_unique<recording::Recording>(recording::Recording::Configuration{
            root,
            sdk,
            catalog_sdk,
            instance,
            std::move(login)
        });
        log_process_event(
            "recorder",
            LogLevel::info,
            "recorder.started",
            {{"instance", instance}, {"config", configuration}});
        try {
            if (!host.run())
                std::_Exit(1);
        } catch (const std::exception& error) {
            log_process_event(
                "recorder",
                LogLevel::error,
                "recorder.failed",
                {{"message", error.what()}});
            std::_Exit(1);
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        log_process_event(
            "recorder",
            LogLevel::error,
            "recorder.failed",
            {{"message", error.what()}});
        return 1;
    }
    return 0;
}
