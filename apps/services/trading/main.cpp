#include <asterion/kernel/logger.hpp>
#include <asterion/kernel/native_plugin.hpp>
#include "live_session.hpp"
#include <CLI/CLI.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/service_host.hpp>
#include <asterion/protocol/trading.hpp>
#include <algorithm>
#include <atomic>
#include <iostream>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
using namespace std::chrono_literals;
int main(int argc, char** argv) {
  CLI::App app{"Asterion CTP trading session host (one account record per process)"};
  app.set_version_flag("--version", "Asterion Trading 0.1.0");
  std::string session_id, directory, health_endpoint;
  app.add_option("--health-endpoint", health_endpoint, "Private supervisor health channel");
  std::uint64_t owner_pid = 0;
  app.add_option("--owner-pid", owner_pid, "Managed service supervisor identity");
  asterion::service::Transport transport;
  app.add_option("--session", session_id, "Unique session identity")->required();
  app.add_option("--endpoint", transport.endpoint, "Private local IPC endpoint (Agent-managed)");
  app.add_option("--bind", transport.bind, "TCP bind IP address (independent service)");
  app.add_option("--port", transport.port, "TCP listen port")->check(CLI::Range(1, 65535));
  app.add_option("--tls-ca", transport.tls.ca_file, "Dedicated client CA PEM");
  app.add_option("--tls-cert", transport.tls.certificate_file, "Server certificate chain PEM");
  app.add_option("--tls-key", transport.tls.private_key_file, "Server private key PEM");
  app.add_option("--directory", directory, "Existing dedicated account journal directory")
      ->required()
      ->check(CLI::ExistingDirectory);
  std::string plugin_directory, ctp_library;
  app.add_option("--plugin-directory", plugin_directory)->check(CLI::ExistingDirectory);
  app.add_option("--ctp-library", ctp_library, "CTP trader SDK")
      ->required()
      ->check(CLI::ExistingFile);
  argv = app.ensure_utf8(argv);
  CLI11_PARSE(app, argc, argv);
  try {
    if (!plugin_directory.empty())
      asterion::configure_native_plugins(
          std::filesystem::path(std::u8string(plugin_directory.begin(), plugin_directory.end())));
    asterion::validate_id(session_id);
    namespace wire = asterion::protocol::v1;
    const std::filesystem::path sdk(std::u8string(ctp_library.begin(), ctp_library.end()));
    transport.validate();
    asterion::service::install_stop_signals();
    asterion::service::OwnerWatch owner(owner_pid);
    const std::filesystem::path journal(std::u8string(directory.begin(), directory.end()));
    std::unique_ptr<asterion::trading::LiveSession> live_session;
    // The service recovers its own record before accepting clients, then
    // waits for credentials; nothing is sent.
    if (std::filesystem::exists(journal / "journal.sqlite"))
      live_session = std::make_unique<asterion::trading::LiveSession>(journal, sdk);
    const auto ready = [&] { return bool(live_session); };
    const auto needs_recovery = [&] { return live_session && live_session->recovery_required(); };
    const auto started = std::chrono::steady_clock::now();
    const auto instance = asterion::unique_process_id();
    std::atomic<bool> initialized{ready()}, degraded{false};
    std::atomic<std::int64_t> busy_since{0};
    auto now = [] {
      return std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now().time_since_epoch())
          .count();
    };
    asterion::service::HealthChannel health(health_endpoint, [&](const std::string& frame) {
      asterion::protocol::v1::Request request;
      if (!request.ParseFromString(frame))
        return std::string();
      asterion::protocol::validate_message(request);
      if (request.version() != 1 || request.session_id() != session_id || !request.has_heartbeat())
        return std::string();
      asterion::protocol::v1::Response response;
      response.set_version(1);
      response.set_session_id(session_id);
      response.set_correlation_id(request.correlation_id());
      auto* h = response.mutable_health();
      h->set_instance_id(instance);
      h->set_version("0.1.0");
      h->set_initialized(initialized);
      h->set_uptime_ms(
          static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now() - started)
                                         .count()));
      const auto busy = busy_since.load();
      h->set_recovery_required(degraded || (busy && now() - busy > 30000));
      return response.SerializeAsString();
    });
    std::mutex ledger_mutex;
    auto serve = [&](asterion::service::Connection& channel, std::stop_token stop) {
      while (!stop.stop_requested()) {
        // Clients own no ledger lock while idle or sending a response.
        // Bound idle reads so pool shutdown cannot wait indefinitely.
        std::string frame;
        try {
          frame = channel.receive(30s);
        } catch (const asterion::Error&) {
          break;
        }
        wire::Request request;
        wire::Response response;
        response.set_version(1);
        response.set_session_id(session_id);
        {
          std::lock_guard lock(ledger_mutex);
          busy_since = now();
          try {
            if (!request.ParseFromString(frame))
              throw std::invalid_argument("malformed Protobuf request");
            asterion::protocol::validate_message(request);
            response.set_correlation_id(request.correlation_id());
            asterion::validate_id(request.correlation_id());
            if (request.version() != 1 || request.session_id() != session_id)
              throw std::invalid_argument("protocol version or session mismatch");
            if (request.has_heartbeat()) {
              auto* health = response.mutable_health();
              health->set_instance_id(instance);
              health->set_version("0.1.0");
              health->set_uptime_ms(
                  static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                 std::chrono::steady_clock::now() - started)
                                                 .count()));
              health->set_initialized(ready());
              health->set_recovery_required(needs_recovery());
            } else if (request.has_shutdown()) {
              throw std::invalid_argument("service stop is a Node Agent operation");
            } else if (request.has_attach() && !ready())
              response.mutable_uninitialized();
            else {
              if (request.has_live_create() || request.has_recover()) {
                if (live_session)
                  throw std::invalid_argument("session already initialized");
                const auto manifest =
                    request.has_live_create()
                        ? asterion::protocol::decode_live_input(request.live_create())
                        : asterion::Json(nullptr);
                live_session =
                    std::make_unique<asterion::trading::LiveSession>(journal, sdk, manifest);
              } else if (!live_session) {
                throw std::invalid_argument("session is not initialized");
              } else if (request.has_live_connect()) {
                // Moved into the trader; never logged or stored.
                auto* credentials = request.mutable_live_connect();
                live_session->connect(std::move(*credentials->mutable_password()),
                                      std::move(*credentials->mutable_auth_code()));
              } else if (request.has_live_disconnect()) {
                live_session->disconnect();
              } else if (request.has_live_costs()) {
                live_session->query_costs();
              } else if (request.has_command()) {
                live_session->execute(asterion::protocol::decode_command(request.command()));
              } else if (!request.has_snapshot() && !request.has_attach())
                throw std::invalid_argument("missing live session operation");
              *response.mutable_live() =
                  asterion::protocol::encode_live_snapshot(live_session->snapshot());
            }
          } catch (const std::exception& error) {
            response.mutable_error()->set_code(
                std::string(asterion::error_name(asterion::classify(error))));
            response.mutable_error()->set_message(error.what());
          }
          // Drop any credentials before the next frame.
          request.Clear();
          std::fill(frame.begin(), frame.end(), '\0');
          initialized = ready();
          degraded = needs_recovery();
          busy_since = 0;
        }
        channel.send(response.SerializeAsString(), 10s);
      }
    };
    // One ledger owner; clients are served by the shared host (bounded pool,
    // mutual TLS off the accept thread, graceful drain on stop).
    asterion::service::ServiceHost host(transport, serve);
    if (!host.run())
      std::_Exit(0);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Trading process failed: " << error.what() << '\n';
    asterion::log_process_event("trading", asterion::LogLevel::error, "service.failed",
                                {{"message", error.what()}});
    return 1;
  }
}
