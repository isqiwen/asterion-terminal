#include <asterion/kernel/logger.hpp>
#include <asterion/kernel/native_plugin.hpp>
#include "live_session.hpp"
#include <CLI/CLI.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/rpc_host.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/protocol/health.hpp>
#include <asterion/protocol/rpc_diagnostics.hpp>
#include <asterion/kernel/trace.hpp>
#include <algorithm>
#include <iostream>
#include <cstdlib>

#include <stdexcept>
#include <pwd.h>
#include <unistd.h>
using namespace std::chrono_literals;
namespace {
std::filesystem::path account_ownership_directory() {
  // Existing isolated test sessions never claim ownership of real local accounts.
  const auto* isolated = std::getenv("ASTERION_TEST_NODE_ISOLATED");
  if (isolated && std::string_view(isolated) == "1") {
    const auto* node = std::getenv("ASTERION_NODE_DIRECTORY");
    if (!node || !std::filesystem::path(node).is_absolute())
      throw std::invalid_argument("isolated trading tests require an absolute node directory");
    return std::filesystem::path(node) / "account-owners";
  }
  // Resolve the OS user, not HOME, XDG, the Node directory or a Terminal profile.
  // Development and installed applications must contend for the same account.
  const auto* user = ::getpwuid(::geteuid());
  if (!user || !user->pw_dir || !std::filesystem::path(user->pw_dir).is_absolute())
    throw std::runtime_error("local account ownership directory is unavailable");
#ifdef __APPLE__
  return std::filesystem::path(user->pw_dir) /
         "Library/Application Support/Asterion/account-owners";
#else
  return std::filesystem::path(user->pw_dir) / ".local/share/asterion/account-owners";
#endif
}
} // namespace
int main(int argc, char** argv) {
  CLI::App app{"Asterion CTP account service (one execution owner per account)"};
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
    const std::filesystem::path journal(std::u8string(directory.begin(), directory.end()));
    const auto ownership_directory = account_ownership_directory();
    // I/O owns initialization, publication and shutdown; the account owns its resources.
    std::unique_ptr<asterion::trading::LiveSession> account_owner;
    asterion::trading::LiveSession* account = nullptr;
    std::shared_future<void> initializing;
    bool recovering = false;
    asterion::Progress initialization, io_progress;
    const auto started = std::chrono::steady_clock::now();
    const auto instance = asterion::unique_process_id();
    const auto fill_health = [&](wire::Health& h) {
      h.set_instance_id(instance);
      h.set_version("0.1.0");
      h.set_uptime_ms(
          static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now() - started)
                                         .count()));
      auto* e = h.mutable_execution();
      *e->mutable_io() = asterion::protocol::encode_progress(io_progress.observe());
      *e->mutable_initialization() = asterion::protocol::encode_progress(initialization.observe());
      const auto current = account_owner.get();
      h.set_initialized(bool(account));
      h.set_recovery_required(current && current->recovery_required());
      if (current) {
        const auto observed = current->health();
        *e->mutable_state() = asterion::protocol::encode_progress(observed.state);
        *e->mutable_persistence() = asterion::protocol::encode_progress(observed.persistence);
        *e->mutable_command() = asterion::protocol::encode_progress(observed.command);
        e->set_business_ready(observed.business_ready);
      }
    };
    struct Pending {
      wire::Request request;
      wire::Response response;
      asterion::trading::LiveSession* session = nullptr;
      std::shared_future<void> initialization;
      std::future<void> action;
      std::future<asterion::Json> snapshot;
      bool needs_snapshot = false;
      void fail(const std::exception& error) {
        response.mutable_error()->set_code(
            std::string(asterion::error_name(asterion::classify(error))));
        response.mutable_error()->set_message(error.what());
        needs_snapshot = false;
      }
    };
    auto serve = [&](const asterion::service::RpcHost::Peer&, std::string frame) {
      auto work = std::make_shared<Pending>();
      auto& request = work->request;
      auto& response = work->response;
      response.set_version(1);
      response.set_session_id(session_id);
      work->session = account;
      try {
        if (!request.ParseFromString(frame))
          throw std::invalid_argument("malformed Protobuf request");
        asterion::protocol::validate_message(request);
        response.set_correlation_id(request.correlation_id());
        asterion::validate_id(request.correlation_id());
        asterion::TraceScope trace(request.correlation_id());
        if (request.version() != 1 || request.session_id() != session_id)
          throw std::invalid_argument("protocol version or session mismatch");
        if (request.has_command())
          asterion::validate_id(request.account_id());
        else if (!request.account_id().empty())
          throw std::invalid_argument("account identity is only valid for trading commands");
        if (request.has_command())
          asterion::validate_id(request.policy_revision());
        else if (!request.policy_revision().empty())
          throw std::invalid_argument("policy revision is only valid for trading commands");
        if (request.has_heartbeat()) {
          fill_health(*response.mutable_health());
        } else if (request.has_shutdown()) {
          throw std::invalid_argument("service stop is a Node Agent operation");
        } else if ((request.has_attach() || request.has_snapshot()) && !work->session &&
                   account_owner) {
          work->session = account_owner.get();
          work->initialization = account_owner->initialized();
          work->needs_snapshot = true;
        } else if (request.has_attach() && !work->session) {
          response.mutable_uninitialized();
        } else {
          if (request.has_live_create() || request.has_recover()) {
            if (account_owner)
              throw std::invalid_argument("session already initialized");
            const auto manifest = request.has_live_create()
                                      ? asterion::protocol::decode_live_input(request.live_create())
                                      : asterion::Json(nullptr);
            account_owner = std::make_unique<asterion::trading::LiveSession>(
                journal, sdk, ownership_directory, manifest);
            initialization.begin();
            work->session = account_owner.get();
            initializing = account_owner->initialized();
            work->initialization = initializing;
          } else if (!work->session) {
            throw std::invalid_argument("session is not initialized");
          } else if (request.has_live_connect()) {
            auto* credentials = request.mutable_live_connect();
            work->action = work->session->connect(std::move(*credentials->mutable_password()),
                                                  std::move(*credentials->mutable_auth_code()));
          } else if (request.has_live_disconnect()) {
            work->action = work->session->disconnect();
          } else if (request.has_live_costs()) {
            work->action = work->session->query_costs();
          } else if (request.has_command()) {
            work->action =
                work->session->execute(request.account_id(), request.policy_revision(),
                                       asterion::protocol::decode_command(request.command()));

          } else if (!request.has_snapshot() && !request.has_attach()) {
            throw std::invalid_argument("missing live session operation");
          }
          work->needs_snapshot = true;
        }
      } catch (const std::exception& error) {
        work->fail(error);
      }
      std::fill(frame.begin(), frame.end(), '\0');
      return [&, work]() -> std::optional<std::string> {
        auto& request = work->request;
        auto& response = work->response;
        asterion::TraceScope trace(request.correlation_id());
        try {
          if (work->initialization.valid()) {
            if (work->initialization.wait_for(0ms) != std::future_status::ready)
              return {};
            work->initialization.get();
            if (account != work->session)
              return {}; // The I/O coordinator publishes initialization once.
            work->initialization = {};
          }
          if (work->action.valid()) {
            if (work->action.wait_for(0ms) != std::future_status::ready)
              return {};
            work->action.get();
          }
          if (work->needs_snapshot) {
            if (!work->snapshot.valid())
              work->snapshot = work->session->snapshot();
            if (work->snapshot.wait_for(0ms) != std::future_status::ready)
              return {};
            *response.mutable_live() =
                asterion::protocol::encode_live_snapshot(work->snapshot.get());
          }
        } catch (const std::exception& error) {
          work->fail(error);
        }
        const auto& command = request.command();
        const std::string_view order = command.has_submit()   ? command.submit().order_id()
                                       : command.has_cancel() ? command.cancel().order_id()
                                       : command.has_live_resolve()
                                           ? command.live_resolve().order_id()
                                           : std::string_view{};
        asterion::protocol::log_rpc_result("trading", request, response,
                                           request.has_snapshot() || request.has_heartbeat() ||
                                               request.has_attach(),
                                           {{"session_id", session_id},
                                            {"account_id", request.account_id()},
                                            {"request_id", command.request_id()},
                                            {"order_id", order}});
        request.Clear();
        return response.SerializeAsString();
      };
    };
    using Host = asterion::service::RpcHost;
    Host::Options options;
    options.owner_pid = owner_pid;
    if (!health_endpoint.empty()) {
      Host::LocalEndpoint health{health_endpoint, {}, 4, true};
      health.handler = [&](const Host::Peer&, std::string frame) -> Host::Reply {
        wire::Request request;
        if (!request.ParseFromString(frame))
          throw std::invalid_argument("invalid service health");
        asterion::protocol::validate_message(request);
        if (request.version() != 1 || request.session_id() != session_id ||
            !request.has_heartbeat())
          throw std::invalid_argument("invalid service health");
        wire::Response response;
        response.set_version(1);
        response.set_session_id(session_id);
        response.set_correlation_id(request.correlation_id());
        fill_health(*response.mutable_health());
        return [reply = response.SerializeAsString()] { return std::optional<std::string>(reply); };
      };
      options.local_endpoints.push_back(std::move(health));
    }
    options.advance = [&](Host::Stage stage) {
      if (!account_owner)
        return true;
      if (stage != Host::Stage::running)
        account_owner->close_admission();
      if (stage == Host::Stage::stopping_resources) {
        account_owner->stop();
        return account_owner->stopped().wait_for(0ms) == std::future_status::ready;
      }
      if (initializing.valid() && initializing.wait_for(0ms) == std::future_status::ready) {
        try {
          initializing.get();
          account = account_owner.get();
        } catch (...) {
          account_owner.reset();
          if (recovering)
            throw;
          // Creation callers retain the original future and receive its failure.
        }
        initializing = {};
        initialization.finish();
      }
      return false;
    };
    Host host(transport, serve, std::move(options), io_progress);
    // Socket setup completes before starting owners that require an asynchronous stop.
    if (std::filesystem::exists(journal / "journal.sqlite")) {
      initialization.begin();
      account_owner =
          std::make_unique<asterion::trading::LiveSession>(journal, sdk, ownership_directory);
      recovering = true;
      initializing = account_owner->initialized();
    }
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
