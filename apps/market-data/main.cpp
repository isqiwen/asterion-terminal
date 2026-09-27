#include "ctp_feed.hpp"
#include <CLI/CLI.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/service_host.hpp>
#include <asterion/protocol/market.hpp>
#include <asterion/protocol/trading.hpp>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>
using namespace asterion;
using namespace std::chrono_literals;
namespace wire = asterion::market::v1;
int main(int argc, char** argv) {
  CLI::App app{"Asterion read-only live market-data host"};
  app.set_version_flag("--version", "Asterion Market Data 0.1.0");
  std::string service, health_endpoint, directory, sdk;
  std::uint64_t owner_pid = 0;
  service::Transport transport;
  app.add_option("--session", service)->required();
  app.add_option("--directory", directory)->required()->check(CLI::ExistingDirectory);
  app.add_option("--endpoint", transport.endpoint);
  app.add_option("--health-endpoint", health_endpoint);
  app.add_option("--bind", transport.bind);
  app.add_option("--port", transport.port);
  app.add_option("--tls-ca", transport.tls.ca_file);
  app.add_option("--tls-cert", transport.tls.certificate_file);
  app.add_option("--tls-key", transport.tls.private_key_file);
  app.add_option("--ctp-library", sdk);
  app.add_option("--owner-pid", owner_pid);
  argv = app.ensure_utf8(argv);
  CLI11_PARSE(app, argc, argv);
  try {
    validate_id(service);
    transport.validate();
    const auto instance = unique_process_id();
    service::install_stop_signals();
    service::OwnerWatch owner(owner_pid);
    PluginManager plugins;
    ctp::Feed* feed = nullptr;
    std::mutex mutex;
    auto snapshot = [&] {
      std::lock_guard lock(mutex);
      LiveMarketSnapshot state;
      if (feed)
        state = feed->snapshot();
      else
        state.phase = sdk.empty() ? "sdk_unavailable" : "disconnected";
      return protocol::encode_market(state, instance);
    };
    auto serve = [&](service::Connection& channel, std::stop_token token) {
      try {
        wire::Request request;
        if (!request.ParseFromString(channel.receive(2s)))
          return;
        protocol::validate_message(request);
        wire::Response response;
        response.set_version(1);
        response.set_service_id(service);
        response.set_correlation_id(request.correlation_id());
        try {
          validate_id(request.correlation_id());
          if (request.version() != 1 || request.service_id() != service)
            throw std::invalid_argument("market service identity mismatch");
          if (request.has_heartbeat()) {
            auto* h = response.mutable_health();
            h->set_instance_id(instance);
            h->set_phase(snapshot().phase());
          } else if (request.has_events()) {
            std::lock_guard lock(mutex);
            if (!feed)
              throw Error(ErrorCode::unavailable, "market event stream not started");
            const auto& read = request.events();
            *response.mutable_events() = protocol::encode_market_events(
                feed->events_after(read.stream_id(), read.after_sequence(), read.limit()));
          } else if (request.has_watch()) {
            // Coalesced quote snapshots; intermediate ticks are not a
            // historical tick archive.
            while (!token.stop_requested()) {
              *response.mutable_snapshot() = snapshot();
              channel.send(response.SerializeAsString(), 2s);
              std::this_thread::sleep_for(250ms);
            }
            return;
          } else {
            {
              std::lock_guard lock(mutex);
              if (request.has_connect()) {
                if (feed && feed->snapshot().phase != "disconnected" &&
                    feed->snapshot().phase != "error")
                  throw Error(ErrorCode::conflict, "disconnect current market session first");
                if (!feed) {
                  auto plugin = std::make_unique<ctp::Feed>(
                      std::filesystem::path(std::u8string(sdk.begin(), sdk.end())),
                      std::filesystem::path(std::u8string(directory.begin(), directory.end())) /
                          "ctp-flow");
                  feed = plugin.get();
                  plugins.add(std::move(plugin));
                  plugins.start();
                }
                auto* c = request.mutable_connect();
                std::vector<InstrumentId> ids;
                for (const auto& i : c->instruments())
                  ids.push_back({i.venue(), i.symbol()});
                ctp::Configuration config{c->front(), c->broker(), c->user(), c->password()};
                c->clear_password();
                feed->connect(std::move(config), ids);
              } else if (request.has_subscribe()) {
                if (!feed)
                  throw Error(ErrorCode::conflict, "connect market data first");
                std::vector<InstrumentId> ids;
                for (const auto& i : request.subscribe().instruments())
                  ids.push_back({i.venue(), i.symbol()});
                feed->subscribe(ids);
              } else if (request.has_disconnect()) {
                if (feed)
                  feed->disconnect();
              } else if (!request.has_snapshot())
                throw std::invalid_argument("missing market operation");
            }
            *response.mutable_snapshot() = snapshot();
          }
        } catch (const Error& e) {
          response.mutable_error()->set_code(std::string(error_name(e.code())));
          response.mutable_error()->set_message(e.what());
        } catch (const std::exception& e) {
          // Provider diagnostics may echo configuration; only the code crosses.
          response.mutable_error()->set_code(std::string(error_name(classify(e))));
          response.mutable_error()->set_message(
              "Market request rejected; check configuration and SDK "
              "availability");
        }
        channel.send(response.SerializeAsString(), 2s);
      } catch (const std::exception&) { /* Bounded peer failure never stops the managed feed. */
      }
    };
    service::HealthChannel health(health_endpoint, [&](const std::string& frame) {
      wire::Request request;
      if (!request.ParseFromString(frame))
        return std::string();
      protocol::validate_message(request);
      validate_id(request.correlation_id());
      if (request.version() != 1 || request.service_id() != service || !request.has_heartbeat())
        return std::string();
      wire::Response response;
      response.set_version(1);
      response.set_service_id(service);
      response.set_correlation_id(request.correlation_id());
      auto* h = response.mutable_health();
      h->set_instance_id(instance);
      h->set_phase(snapshot().phase());
      return response.SerializeAsString();
    });
    // Watch requests stream until the client leaves or the service stops, so
    // concurrency is bounded by workers and a full host rejects immediately.
    service::HostOptions options;
    options.workers = 16;
    options.queue = 1;
    options.handshake = 2s;
    service::ServiceHost host(transport, serve, options);
    if (!host.run())
      std::_Exit(0);
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
  return 0;
}
