#include "session.hpp"
#include <CLI/CLI.hpp>
#include <asterion/kernel/logger.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/protocol/market.hpp>
#include <asterion/protocol/trading.hpp>
#include <algorithm>
#include <cstdlib>
#include <iostream>
using namespace asterion;
using namespace std::chrono_literals;
namespace wire = asterion::market::v1;
int main(int argc, char** argv) {
  CLI::App app{"Asterion read-only live market-data host"};
  app.set_version_flag("--version", "Asterion Market Data 0.1.0");
  std::string service, health_endpoint, directory, sdk, catalog_sdk;
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
  app.add_option("--ctp-catalog-library", catalog_sdk);
  app.add_option("--owner-pid", owner_pid);
  argv = app.ensure_utf8(argv);
  CLI11_PARSE(app, argc, argv);
  try {
    validate_id(service);
    transport.validate();
    const auto instance = unique_process_id();
    service::install_stop_signals();
    using Host = service::RpcHost;
    Progress io_progress;
    std::unique_ptr<market_data::Session> state;
    bool draining = false;
    std::size_t watchers = 0;
    auto serve = [&](std::string frame, bool control) -> Host::Reply {
      wire::Request request;
      wire::Response failure;
      failure.set_version(1);
      failure.set_service_id(service);
      const auto fail = [](wire::Response& response, const std::exception& error) {
        response.mutable_error()->set_code(std::string(error_name(classify(error))));
        response.mutable_error()->set_message(
            dynamic_cast<const Error*>(&error)
                ? error.what()
                : "Market request rejected; check configuration and SDK availability");
      };
      struct Pending {
        std::future<Host::Message> result;
        wire::Request watch;
        std::shared_ptr<market_data::WatchCursor> cursor;
        std::chrono::steady_clock::time_point next;
        std::size_t* watchers = nullptr;
        ~Pending() {
          if (watchers)
            --*watchers;
        }
      };
      auto pending = std::make_shared<Pending>();
      try {
        if (!request.ParseFromString(frame))
          throw std::invalid_argument("malformed Protobuf request");
        protocol::validate_message(request);
        failure.set_correlation_id(request.correlation_id());
        validate_id(request.correlation_id());
        if (request.version() != 1 || request.service_id() != service)
          throw std::invalid_argument("market service identity mismatch");
        if (control && !request.has_heartbeat() && !request.has_quiesce())
          throw std::invalid_argument("invalid service health");
        if (request.has_watch()) {
          if (watchers == 32)
            throw Error(ErrorCode::resource_exhausted, "market watch capacity is full");
          pending->watchers = &watchers;
          ++watchers;
          pending->watch = request;
          pending->cursor = std::make_shared<market_data::WatchCursor>();
        }
        pending->result = state->request(std::move(request), pending->cursor, control);
      } catch (const std::exception& error) {
        fail(failure, error);
      }
      std::fill(frame.begin(), frame.end(), '\0');
      return [&, pending, failure = std::move(failure),
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
    options.owner_pid = owner_pid;
    options.connections = 64;
    options.send = 2s;
    if (!health_endpoint.empty())
      options.local_endpoints.push_back(
          {health_endpoint,
           [&](const Host::Peer&, std::string frame) { return serve(std::move(frame), true); }, 4,
           false});
    options.advance = [&](Host::Stage stage) {
      draining = stage != Host::Stage::running;
      if (stage == Host::Stage::stopping_resources)
        state->stop();
      const auto stopped = state->stopped();
      if (stopped.wait_for(0ms) != std::future_status::ready)
        return false;
      stopped.get();
      return true;
    };
    Host host(
        transport,
        [&](const Host::Peer&, std::string frame) { return serve(std::move(frame), false); },
        std::move(options), io_progress);
    // Listen before starting resources whose teardown may wait for a provider.
    state = std::make_unique<market_data::Session>(
        market_data::Session::Configuration{service, instance, directory, sdk, catalog_sdk});
    try {
      if (!host.run())
        std::_Exit(0);
    } catch (const std::exception& error) {
      log_process_failure("market-data", "service.failed", classify(error), 1);
      std::_Exit(1);
    }
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    log_process_event("market-data", LogLevel::error, "service.failed", {{"message", e.what()}});
    return 1;
  }
  return 0;
}
