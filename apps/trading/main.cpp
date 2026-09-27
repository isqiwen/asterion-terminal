#include "paper_session.hpp"
#include <CLI/CLI.hpp>
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/ipc/tls_channel.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/owner.hpp>
#include <asterion/kernel/thread_pool.hpp>
#include <asterion/protocol/trading.hpp>
#include <atomic>
#include <iostream>
#include <mutex>
#include <thread>
using namespace std::chrono_literals;
int main(int argc, char** argv) {
  CLI::App app{"Asterion trading session host (one account ledger per process)"};
  app.set_version_flag("--version", "Asterion Trading 0.1.0");
  std::string mode, session_id, endpoint, directory, bind_address, health_endpoint;
  app.add_option("--health-endpoint", health_endpoint, "Private supervisor health channel");
  unsigned short port = 0;
  std::uint64_t owner_pid = 0;
  app.add_option("--owner-pid", owner_pid, "Managed service supervisor identity");
  asterion::ipc::TlsIdentity tls;
  app.add_option("--mode", mode, "Immutable execution mode")
      ->required()
      ->check(CLI::IsMember({"paper", "live"}));
  app.add_option("--session", session_id, "Unique session identity")->required();
  app.add_option("--endpoint", endpoint, "Private local IPC endpoint (Agent-managed)");
  app.add_option("--bind", bind_address, "TCP bind IP address (independent service)");
  app.add_option("--port", port, "TCP listen port")->check(CLI::Range(1, 65535));
  app.add_option("--tls-ca", tls.ca_file, "Dedicated client CA PEM");
  app.add_option("--tls-cert", tls.certificate_file, "Server certificate chain PEM");
  app.add_option("--tls-key", tls.private_key_file, "Server private key PEM");
  app.add_option("--directory", directory, "Existing dedicated account journal directory")
      ->required()
      ->check(CLI::ExistingDirectory);
  argv = app.ensure_utf8(argv);
  CLI11_PARSE(app, argc, argv);
  try {
    asterion::validate_id(session_id);
    std::unique_ptr<asterion::ProcessOwner> owner;
    std::jthread owner_watch;
    if (owner_pid) {
      owner = std::make_unique<asterion::ProcessOwner>(owner_pid);
      owner_watch = std::jthread([&](std::stop_token token) {
        while (!token.stop_requested()) {
          if (!owner->alive())
            std::_Exit(4);
          std::this_thread::sleep_for(200ms);
        }
      });
    }
    if (mode == "live") {
      std::cerr << "Live execution unavailable: broker, account authorization "
                   "and risk capabilities are not configured.\n";
      return 3;
    }
    const bool remote = !bind_address.empty();
    if (remote ? (!endpoint.empty() || !port || tls.ca_file.empty() ||
                  tls.certificate_file.empty() || tls.private_key_file.empty())
               : (endpoint.empty() || port || !tls.ca_file.empty() ||
                  !tls.certificate_file.empty() || !tls.private_key_file.empty()))
      throw std::invalid_argument("choose --endpoint OR --bind/--port with all three TLS files");
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
    std::unique_ptr<asterion::ipc::Listener> health_listener;
    std::jthread health_worker;
    if (!health_endpoint.empty()) {
      health_listener = std::make_unique<asterion::ipc::Listener>(health_endpoint);
      health_worker = std::jthread([&](std::stop_token token) {
        while (!token.stop_requested()) {
          try {
            auto channel = health_listener->accept(200ms);
            asterion::protocol::v1::Request request;
            if (!request.ParseFromString(channel.receive(1s)))
              continue;
            asterion::protocol::validate_message(request);
            if (request.version() != 1 || request.session_id() != session_id ||
                request.mode() != asterion::protocol::v1::PAPER || !request.has_heartbeat())
              continue;
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
            channel.send(response.SerializeAsString(), 1s);
          } catch (const std::exception&) { /* Peer failure does not own service lifetime. */
          }
        }
      });
    }
    std::mutex ledger_mutex;
    auto serve = [&](auto& channel, std::stop_token stop) {
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
            response.mutable_error()->set_code("operation_failed");
            response.mutable_error()->set_message(error.what());
          }
          initialized = bool(session);
          degraded = session && session->recovery_required();
          busy_since = 0;
        }
        channel.send(response.SerializeAsString(), 10s);
      }
    };
    asterion::ThreadPool clients(8, 8);
    auto dispatch = [&](auto channel) {
      auto peer = std::make_shared<decltype(channel)>(std::move(channel));
      static_cast<void>(clients.submit([&, peer](std::stop_token stop) {
        try {
          serve(*peer, stop);
        } catch (const std::exception&) { /* Disconnect affects only this client. */
        }
      }));
    };
    if (remote) {
      asterion::ipc::TlsListener listener(bind_address, port, tls);
      for (;;) {
        try {
          auto channel = listener.accept(10s);
          dispatch(std::move(channel));
        } catch (const std::exception&) { /* A failed client never terminates
                                             the ledger owner. */
        }
      }
    } else {
      asterion::ipc::Listener listener(endpoint);
      for (;;) {
        try {
          auto channel = listener.accept(1s);
          dispatch(std::move(channel));
        } catch (const asterion::Error&) {
        }
      }
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Trading process failed: " << error.what() << '\n';
    return 1;
  }
}
