#include "paper_session.hpp"
#include <CLI/CLI.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/service_host.hpp>
#include <asterion/protocol/trading.hpp>
#include <atomic>
#include <iostream>
#include <cstdlib>
#include <mutex>
using namespace std::chrono_literals;
int main(int argc, char** argv) {
  CLI::App app{"Asterion trading session host (one account ledger per process)"};
  app.set_version_flag("--version", "Asterion Trading 0.1.0");
  std::string mode, session_id, directory, health_endpoint;
  app.add_option("--health-endpoint", health_endpoint, "Private supervisor health channel");
  std::uint64_t owner_pid = 0;
  app.add_option("--owner-pid", owner_pid, "Managed service supervisor identity");
  asterion::service::Transport transport;
  app.add_option("--mode", mode, "Immutable execution mode")
      ->required()
      ->check(CLI::IsMember({"paper", "live"}));
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
  argv = app.ensure_utf8(argv);
  CLI11_PARSE(app, argc, argv);
  try {
    asterion::validate_id(session_id);
    if (mode == "live") {
      std::cerr << "Live execution unavailable: broker, account authorization "
                   "and risk capabilities are not configured.\n";
      return 3;
    }
    transport.validate();
    asterion::service::install_stop_signals();
    asterion::service::OwnerWatch owner(owner_pid);
    const std::filesystem::path journal(std::u8string(directory.begin(), directory.end()));
    std::unique_ptr<asterion::trading::PaperSession> session;
    // A remote service recovers its server-owned ledger before accepting
    // clients.
    if (std::filesystem::exists(journal / "00000000.json"))
      session = std::make_unique<asterion::trading::PaperSession>(journal);
    const auto started = std::chrono::steady_clock::now();
    const auto instance = asterion::unique_process_id();
    std::atomic<bool> initialized{bool(session)}, degraded{false};
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
      if (request.version() != 1 || request.session_id() != session_id ||
          request.mode() != asterion::protocol::v1::PAPER || !request.has_heartbeat())
        return std::string();
      asterion::protocol::v1::Response response;
      response.set_version(1);
      response.set_session_id(session_id);
      response.set_mode(asterion::protocol::v1::PAPER);
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
        namespace wire = asterion::protocol::v1;
        wire::Request request;
        wire::Response response;
        response.set_version(1);
        response.set_session_id(session_id);
        response.set_mode(wire::PAPER);
        {
          std::lock_guard lock(ledger_mutex);
          busy_since = now();
          try {
            if (!request.ParseFromString(frame))
              throw std::invalid_argument("malformed Protobuf request");
            asterion::protocol::validate_message(request);
            response.set_correlation_id(request.correlation_id());
            asterion::validate_id(request.correlation_id());
            if (request.version() != 1 || request.session_id() != session_id ||
                request.mode() != wire::PAPER)
              throw std::invalid_argument("protocol version, session or mode mismatch");
            if (request.has_heartbeat()) {
              auto* health = response.mutable_health();
              health->set_instance_id(instance);
              health->set_version("0.1.0");
              health->set_uptime_ms(
                  static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                 std::chrono::steady_clock::now() - started)
                                                 .count()));
              health->set_initialized(bool(session));
              health->set_recovery_required(session && session->recovery_required());
            } else if (request.has_shutdown()) {
              throw std::invalid_argument("service stop is a Node Agent operation");
            } else if (request.has_attach() && !session)
              response.mutable_uninitialized();
            else {
              if (request.has_create() || request.has_recover()) {
                if (session)
                  throw std::invalid_argument("session already initialized");
                const auto manifest = request.has_create()
                                          ? asterion::protocol::decode_input(request.create())
                                          : asterion::Json(nullptr);
                session = std::make_unique<asterion::trading::PaperSession>(
                    std::filesystem::path(std::u8string(directory.begin(), directory.end())),
                    manifest);
              } else if (request.has_command()) {
                if (!session)
                  throw std::invalid_argument("session is not initialized");
                session->execute(asterion::protocol::decode_command(request.command()));
              } else if (!request.has_snapshot() && !request.has_attach())
                throw std::invalid_argument("missing session operation");
              if (!session)
                throw std::invalid_argument("session is not initialized");
              *response.mutable_snapshot() =
                  asterion::protocol::encode_snapshot(session->snapshot());
            }
          } catch (const std::exception& error) {
            response.mutable_error()->set_code(
                std::string(asterion::error_name(asterion::classify(error))));
            response.mutable_error()->set_message(error.what());
          }
          initialized = bool(session);
          degraded = session && session->recovery_required();
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
    return 1;
  }
}
