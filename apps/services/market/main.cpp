#include "session.hpp"
#include <asterion/system/environment.hpp>
#include <CLI/CLI.hpp>
#include <asterion/system/log.hpp>
#include <asterion/system/process/child.hpp>
#include <asterion/system/process/file_lock.hpp>
#include <asterion/protocol/common.hpp>
#include <asterion/protocol/market.hpp>
#include <asterion/protocol/trading.hpp>
#include <algorithm>
#include <cstdlib>
#include <iostream>
using namespace asterion;
using namespace std::chrono_literals;
namespace wire = ::market;

int main(int argc, char** argv)
{
    CLI::App app{"Asterion market-data and acquisition host"};
    app.set_version_flag("--version", "Asterion Market Data 0.1.0");
    std::string installation;
    app.add_option("--installation", installation)->required()->check(CLI::ExistingDirectory);
    argv = app.ensure_utf8(argv);
    CLI11_PARSE(app, argc, argv);
    try {
        asterion::log::init();
        const auto settings =
            market_data::load_settings(installation, environment_path("CREDENTIALS_DIRECTORY"));
        FileLock owner(settings.directory, "market.lock");
        service::Transport transport;
        transport.bind = settings.bind;
        transport.port = settings.port;
        transport.tls = settings.tls;
        transport.validate();
        const auto instance = unique_process_id();
        service::install_stop_signals();
        using Host = service::RpcHost;
        Progress io_progress;
        std::unique_ptr<market_data::Session> state;
        bool draining = false;
        std::size_t watchers = 0;
        auto serve = [&](const Host::Peer& peer, std::string frame) -> Host::Reply {
            wire::Request request;
            wire::Response failure;
            const auto fail = [](wire::Response& response, const std::exception& error) {
                *response.mutable_error() = protocol::error(error);
                if (!dynamic_cast<const Error*>(&error)
                    && classify(error) != ErrorCode::invalid_request)
                    response.mutable_error()->set_message(
                        "Market request rejected; check configuration and SDK availability");
            };
            struct Pending
            {
                std::future<Host::Message> result;
                wire::Request watch;
                std::shared_ptr<market_data::WatchCursor> cursor;
                std::chrono::steady_clock::time_point next;
                std::size_t* watchers = nullptr;
                ~Pending()
                {
                    if (watchers)
                        --*watchers;
                }
            };
            auto pending = std::make_shared<Pending>();
            try {
                if (!request.ParseFromString(frame))
                    throw std::invalid_argument("malformed Protobuf request");
                protocol::validate_message(request);
                failure.set_id(request.id());
                const auto client = settings.clients.find(peer.fingerprint);
                if (client == settings.clients.end()
                    || ((request.has_start_acquisition() || request.has_cancel_acquisition())
                        && !client->second))
                    throw Error(
                        ErrorCode::permission_denied,
                        "Market acquisition management permission is required");
                if (request.has_stream_quotes()) {
                    if (watchers == 32)
                        throw Error(ErrorCode::resource_exhausted, "market watch capacity is full");
                    pending->watchers = &watchers;
                    ++watchers;
                    pending->watch = request;
                    pending->cursor = std::make_shared<market_data::WatchCursor>();
                }
                pending->result =
                    state->request(std::move(request), pending->cursor, client->second);
            } catch (const std::exception& error) {
                fail(failure, error);
            }
            std::fill(frame.begin(), frame.end(), '\0');
            return [&,
                       pending,
                       failure = std::move(failure),
                       fail]() mutable -> std::optional<Host::Message> {
                if (failure.has_error())
                    return Host::Message(failure.SerializeAsString());
                try {
                    if (!pending->result.valid()) {
                        if (!draining && std::chrono::steady_clock::now() < pending->next)
                            return {};
                        pending->result = state->request(pending->watch, pending->cursor);
                    }
                    if (pending->result.wait_for(0ms) != std::future_status::ready)
                        return {};
                    auto result = pending->result.get();
                    result.more = result.more && !draining;
                    pending->next = std::chrono::steady_clock::now() + 250ms;
                    return result;
                } catch (const std::exception& error) {
                    fail(failure, error);
                    return Host::Message(failure.SerializeAsString());
                }
            };
        };
        Host::Options options;
        options.connections = 64;
        options.send = 2s;
        options.drain = 20s;
        options.advance = [&](Host::Stage stage) {
            draining = stage != Host::Stage::running;
            if (stage == Host::Stage::stopping_resources)
                state->stop();
            const auto stopped = state->stopped();
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
                return serve(peer, std::move(frame));
            },
            std::move(options),
            io_progress);
        // Listen before starting resources whose teardown may wait for a provider.
        state = std::make_unique<market_data::Session>(
            market_data::Session::Configuration{instance, settings});
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
