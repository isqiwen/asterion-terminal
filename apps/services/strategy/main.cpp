#include <asterion/kernel/logger.hpp>
#include "replay.hpp"
#include "session.hpp"
#include <CLI/CLI.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/service_host.hpp>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>
#include <stdexcept>
using namespace std::chrono_literals;
int main(int argc, char** argv) {
  CLI::App app{"Asterion trusted strategy host: durable events and target intents"};
  app.set_version_flag("--version", "asterion-strategy " ASTERION_PRODUCT_VERSION);
  std::string directory, session_id, health_endpoint;
  std::uint64_t owner_pid = 0;
  asterion::service::Transport transport;
  app.add_option("--directory", directory, "Existing dedicated strategy journal directory")
      ->required()
      ->check(CLI::ExistingDirectory);
  app.add_option("--session", session_id, "Immutable strategy session identity")->required();
  app.add_option("--endpoint", transport.endpoint, "Private local IPC endpoint");
  app.add_option("--bind", transport.bind, "TCP bind address");
  app.add_option("--port", transport.port, "TCP port")->check(CLI::Range(1, 65535));
  app.add_option("--tls-ca", transport.tls.ca_file, "Dedicated client CA PEM");
  app.add_option("--tls-cert", transport.tls.certificate_file, "Server certificate PEM");
  app.add_option("--tls-key", transport.tls.private_key_file, "Server private key PEM");
  app.add_option("--health-endpoint", health_endpoint, "Private supervisor health channel");
  app.add_option("--owner-pid", owner_pid, "Agent process identity");
  argv = app.ensure_utf8(argv);
  CLI11_PARSE(app, argc, argv);
  try {
    using namespace asterion;
    namespace wire = strategy::v1;
    validate_id(session_id);
    transport.validate();
    if (!health_endpoint.empty() && health_endpoint == transport.endpoint)
      throw std::invalid_argument("health and event endpoints must be distinct");
    const std::filesystem::path path(std::u8string(directory.begin(), directory.end()));
    if (!path.is_absolute() || std::filesystem::is_symlink(path))
      throw std::invalid_argument("strategy directory must be absolute");
    service::install_stop_signals();
    service::OwnerWatch owner(owner_pid);
    std::unique_ptr<strategy::Session> session;
    if (std::filesystem::exists(path / "journal.sqlite"))
      session = std::make_unique<strategy::Session>(path, session_id);
    const auto instance = unique_process_id();
    const auto started = std::chrono::steady_clock::now();
    std::atomic<bool> initialized{bool(session)}, degraded{false};
    auto health = [&](wire::Response& response) {
      auto* h = response.mutable_health();
      h->set_instance_id(instance);
      h->set_version(ASTERION_PRODUCT_VERSION);
      h->set_initialized(initialized);
      h->set_recovery_required(degraded);
      h->set_uptime_ms(
          static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now() - started)
                                         .count()));
    };
    auto validate = [&](const wire::Request& request, wire::Response& response) {
      protocol::validate_message(request);
      validate_id(request.correlation_id());
      if (request.version() != 1 || request.session_id() != session_id)
        throw std::invalid_argument("strategy protocol or session mismatch");
      response.set_version(1);
      response.set_session_id(session_id);
      response.set_correlation_id(request.correlation_id());
    };
    service::HealthChannel health_channel(health_endpoint, [&](const std::string& frame) {
      wire::Request request;
      wire::Response response;
      if (!request.ParseFromString(frame))
        return std::string();
      validate(request, response);
      if (!request.has_heartbeat())
        return std::string();
      health(response);
      return response.SerializeAsString();
    });
    std::mutex session_mutex;
    std::string replay_phase, replay_error;
    std::unique_ptr<strategy::Replay> replay;
    bool replay_probed = false;
    auto probe_deadline = std::chrono::steady_clock::now() + 20s;
    std::jthread replay_worker([&](std::stop_token stop) {
      while (!stop.stop_requested()) {
        std::this_thread::sleep_for(50ms);
        std::lock_guard lock(session_mutex);
        if (!session || !session->config().has_replay() || replay_phase == "completed" ||
            replay_phase == "blocked")
          continue;
        try {
          if (!replay) {
            replay = std::make_unique<strategy::Replay>(*session);
            probe_deadline = std::chrono::steady_clock::now() + 20s;
          }
          if (!replay_probed) {
            replay_phase = "waiting";
            replay->probe();
            replay_probed = true;
          }
          replay_phase = replay->step() ? "completed" : "running";
          replay_error.clear();
          degraded = false;
        } catch (const std::exception& error) {
          replay_error = error.what();
          if (replay_probed || std::chrono::steady_clock::now() >= probe_deadline) {
            replay_phase = "blocked";
            degraded = true;
          }
        }
      }
    });
    auto serve = [&](service::Connection& channel, std::stop_token) {
      // One bounded request per connection; an idle client cannot hold the
      // event writer forever. Health uses its own channel and never touches the
      // plugin.
      wire::Request request;
      wire::Response response;
      response.set_version(1);
      response.set_session_id(session_id);
      try {
        if (!request.ParseFromString(channel.receive(5s)))
          throw std::invalid_argument("malformed strategy Protobuf");
        validate(request, response);
        std::lock_guard lock(session_mutex);
        if (request.has_heartbeat())
          health(response);
        else if (request.has_create()) {
          if (session)
            session->verify_config(request.create());
          else
            session = std::make_unique<strategy::Session>(path, session_id, &request.create());
          *response.mutable_snapshot() = session->snapshot();
        } else if (request.has_event()) {
          if (!session)
            throw std::invalid_argument("strategy session is not initialized");
          if (session->config().has_replay())
            throw std::invalid_argument("replay session owns its source events");
          *response.mutable_receipt() = session->apply(request.event());
        } else if (request.has_snapshot()) {
          if (session)
            *response.mutable_snapshot() = session->snapshot();
          else
            response.mutable_uninitialized();
        } else
          throw std::invalid_argument("missing strategy operation");
        if (response.has_snapshot() && session && session->config().has_replay()) {
          response.mutable_snapshot()->mutable_replay()->set_phase(
              replay_phase.empty() ? "waiting" : replay_phase);
          response.mutable_snapshot()->mutable_replay()->set_error(replay_error);
        }
        initialized = bool(session);
        degraded = (session && session->recovery_required()) || replay_phase == "blocked";
      } catch (const std::exception& error) {
        std::lock_guard lock(session_mutex);
        initialized = bool(session);
        degraded = (session && session->recovery_required()) || replay_phase == "blocked";
        response.mutable_error()->set_code(
            std::string(asterion::error_name(asterion::classify(error))));
        response.mutable_error()->set_message(error.what());
      }

      channel.send(response.SerializeAsString(), 5s);
    };
    // One bounded request per connection; serve() takes the session lock only
    // after a frame arrives, so handshakes and slow peers never hold it.
    service::HostOptions options;
    options.workers = 4;
    options.handshake = 1s;
    service::ServiceHost host(transport, serve, options);
    if (!host.run())
      std::_Exit(0);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "Strategy process failed: " << error.what() << '\n';
    asterion::log_process_event("strategy", asterion::LogLevel::error, "service.failed",
                                {{"message", error.what()}});
    return 1;
  }
}
