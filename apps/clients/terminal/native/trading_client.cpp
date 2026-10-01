#include "trading_client.hpp"
#include "node_client.hpp"
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/protocol/trading.hpp>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <optional>
#include <stdexcept>
#ifndef _WIN32
#include <sys/stat.h>
#endif
namespace asterion::terminal {
using namespace std::chrono_literals;
namespace wire = protocol::v1;
struct TradingClient::Impl {
  TradingMode mode;
  std::optional<ServiceEndpoint> remote;
  ipc::TlsChannel tcp;
  std::string session_id = "paper." + unique_process_id();

  ipc::Channel channel;
  std::uint64_t sequence = 0;
  Json last_snapshot = nullptr;
  bool failed = false;
  Json health = nullptr;
  std::int64_t last_heartbeat_ms = 0;
  std::int64_t latency_ms = 0;
  Impl(const ServiceEndpoint& config, TradingMode trading_mode)
      : mode(trading_mode), remote(config) {
    validate_id(config.session);
    session_id = config.session;
    if (config.endpoint.empty())
      tcp = ipc::TlsChannel::connect(config.host, config.port, config.tls, 10s);
    else {
      const auto deadline = std::chrono::steady_clock::now() + 10s;
      for (;;) {
        try {
          channel = ipc::Channel::connect(config.endpoint, 500ms);
          break;
        } catch (const Error&) {
          if (std::chrono::steady_clock::now() >= deadline)
            throw;
          std::this_thread::sleep_for(50ms);
        }
      }
    }
  }
  void close() {
    channel.close();
    tcp.close();
  }
  ~Impl() { close(); }
  wire::Mode wire_mode() const { return mode == TradingMode::live ? wire::LIVE : wire::PAPER; }
  Json call(wire::Request request) {
    if (failed)
      throw Error(ErrorCode::unavailable, "trading connection lost; reconnect remote sessions in "
                                          "Settings, recover local sessions from their directory");
    request.set_version(1);
    request.set_session_id(session_id);
    request.set_mode(wire_mode());
    request.set_correlation_id("rpc." + std::to_string(++sequence));
    wire::Response response;
    const auto sent = std::chrono::steady_clock::now();
    // Reads bound how long a status poll can wait; mutations keep a longer
    // deadline because their outcome becomes unknown on timeout.
    const auto timeout = request.has_command() || request.has_create() || request.has_recover() ||
                                 request.has_live_create() || request.has_live_connect() ||
                                 request.has_live_disconnect()
                             ? 10s
                             : 3s;
    try {
      if (remote->endpoint.empty())
        tcp.send(request.SerializeAsString(), timeout);
      else
        channel.send(request.SerializeAsString(), timeout);
      if (!response.ParseFromString(remote->endpoint.empty() ? tcp.receive(timeout)
                                                             : channel.receive(timeout)))
        throw Error(ErrorCode::unavailable, "invalid trading response");
      protocol::validate_message(response);
      if (response.version() != 1 || response.session_id() != session_id ||
          response.mode() != wire_mode() || response.correlation_id() != request.correlation_id())
        throw Error(ErrorCode::unavailable, "trading response identity mismatch");
      if (!response.has_error() &&
          !(mode == TradingMode::paper ? response.has_snapshot() : response.has_live()) &&
          !(request.has_attach() && response.has_uninitialized()) &&
          !(request.has_heartbeat() && response.has_health()))
        throw Error(ErrorCode::unavailable, "missing trading response");
    } catch (...) {
      failed = true;
      close();
      throw;
    }
    if (response.has_health()) {
      const auto& h = response.health();
      if (h.instance_id().empty() || h.version().empty()) {
        failed = true;
        close();
        throw Error(ErrorCode::unavailable, "invalid service health");
      }
      last_heartbeat_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count();
      latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - sent)
                       .count();
      health = {{"instance_id", h.instance_id()},
                {"version", h.version()},
                {"uptime_ms", h.uptime_ms()},
                {"phase", h.recovery_required() ? "degraded"
                          : h.initialized()     ? "ready"
                                                : "awaiting_input"}};
      return last_snapshot;
    }
    if (response.has_error())
      throw_remote_error(response.error().code(), response.error().message());
    if (response.has_uninitialized()) {
      last_snapshot = nullptr;
      return nullptr;
    }
    try {
      last_snapshot = mode == TradingMode::live ? protocol::decode_live_snapshot(response.live())
                                                : protocol::decode_snapshot(response.snapshot());
    } catch (...) {
      failed = true;
      close();
      throw Error(ErrorCode::unavailable, "invalid trading snapshot; recover the session");
    }
    return last_snapshot;
  }
};
namespace {
wire::Request create_request(TradingMode mode, const Json& manifest) {
  wire::Request request;
  if (mode == TradingMode::live)
    *request.mutable_live_create() = protocol::encode_live_input(manifest);
  else
    *request.mutable_create() = protocol::encode_input(manifest);
  return request;
}
} // namespace
TradingClient::TradingClient(const std::filesystem::path& directory, TradingMode mode,
                             const Json& manifest) {
  node_ = std::make_unique<NodeClient>(local_node());
  impl_ = std::make_unique<Impl>(node_->local_session(directory, mode == TradingMode::live
                                                                     ? node::v1::LIVE_TRADING
                                                                     : node::v1::PAPER_TRADING),
                                 mode);
  wire::Request attach;
  attach.mutable_attach();
  impl_->call(std::move(attach));
  if (!manifest.is_null())
    impl_->call(create_request(mode, manifest));
  else if (impl_->last_snapshot.is_null())
    throw std::invalid_argument("trading record is not initialized");
  monitor();
}
TradingClient::TradingClient(const ServiceEndpoint& config, TradingMode mode)
    : impl_(std::make_unique<Impl>(config, mode)) {
  wire::Request request;
  request.mutable_attach();
  impl_->call(std::move(request));
  monitor();
}
void TradingClient::monitor() {
  wire::Request ping;
  ping.mutable_heartbeat();
  impl_->call(std::move(ping));
  heartbeat_ = std::jthread([this](std::stop_token stop) {
    std::unique_lock lock(mutex_);
    while (!wake_.wait_for(lock, 5s, [&] { return stop.stop_requested(); })) {
      if (impl_->failed) {
        if (impl_->remote) {
          if (reconnect_attempts_ >= 3)
            continue;
          ++reconnect_attempts_;
          ++reconnects_;
          try {
            auto next = std::make_unique<Impl>(
                node_ ? node_->service_endpoint(impl_->session_id, impl_->mode == TradingMode::live
                                                                       ? node::v1::LIVE_TRADING
                                                                       : node::v1::PAPER_TRADING)
                      : *impl_->remote,
                impl_->mode);
            wire::Request attach;
            attach.mutable_attach();
            next->call(std::move(attach));
            if (!impl_->last_snapshot.is_null() && next->last_snapshot.is_null())
              throw Error(ErrorCode::unavailable, "remote ledger is no longer initialized");
            wire::Request ping;
            ping.mutable_heartbeat();
            next->call(std::move(ping));
            impl_ = std::move(next);
            reconnect_attempts_ = 0;
          } catch (...) { /* Never resend a trading command during reconnect. */
          }
          continue;
        }
      }
      try {
        wire::Request ping;
        ping.mutable_heartbeat();
        impl_->call(std::move(ping));
      } catch (...) {
        impl_->failed = true;
        impl_->close();
      }
    }
  });
}
TradingClient::~TradingClient() {
  heartbeat_.request_stop();
  wake_.notify_all();
  if (heartbeat_.joinable())
    heartbeat_.join();
}
void TradingClient::create(const Json& manifest) {
  std::lock_guard lock(mutex_);
  impl_->call(create_request(impl_->mode, manifest));
}
void TradingClient::connect_broker(std::string password, std::string auth_code) {
  std::lock_guard lock(mutex_);
  if (impl_->mode != TradingMode::live)
    throw std::invalid_argument("broker connections belong to live sessions");
  wire::Request request;
  request.mutable_live_connect()->set_password(std::move(password));
  request.mutable_live_connect()->set_auth_code(std::move(auth_code));
  impl_->call(std::move(request));
}
void TradingClient::disconnect_broker() {
  std::lock_guard lock(mutex_);
  if (impl_->mode != TradingMode::live)
    throw std::invalid_argument("broker connections belong to live sessions");
  wire::Request request;
  request.mutable_live_disconnect();
  impl_->call(std::move(request));
}
void TradingClient::reconnect() {
  std::lock_guard lock(mutex_);
  if (!impl_->remote)
    throw std::invalid_argument("local sessions must be reopened from their journal");
  const auto config = *impl_->remote;
  impl_->close();
  impl_->failed = true;
  auto next = std::make_unique<Impl>(config, impl_->mode);
  wire::Request request;
  request.mutable_attach();
  next->call(std::move(request));
  wire::Request ping;
  ping.mutable_heartbeat();
  next->call(std::move(ping));
  impl_ = std::move(next);
  reconnect_attempts_ = 0;
}
Json TradingClient::connection() const {
  std::lock_guard lock(mutex_);
  Json status = {{"reconnects", reconnects_},
                 {"restarts", restarts_},
                 {"health", impl_->health},
                 {"last_heartbeat_ms", impl_->last_heartbeat_ms},
                 {"latency_ms", impl_->latency_ms}};
  if (node_) {
    const auto n = node_->status();
    if (!n.at("health").is_null())
      for (const auto& s : n.at("health").at("services"))
        if (s.at("id") == impl_->session_id)
          status["restarts"] = s.at("restarts");
  }
  if (!impl_->remote->endpoint.empty()) {
    status.update({{"transport", "local"},
                   {"session", impl_->session_id},
                   {"state", impl_->failed ? "disconnected" : "connected"}});
    return status;
  }
  status.update({{"transport", "tcp_tls"},
                 {"state", impl_->failed ? "disconnected" : "connected"},
                 {"host", impl_->remote->host},
                 {"port", impl_->remote->port},
                 {"session", impl_->session_id},
                 {"mode", impl_->mode == TradingMode::live ? "live" : "paper"}});
  return status;
}
std::uint64_t TradingClient::process_id() const {
  std::lock_guard lock(mutex_);
  if (node_) {
    const auto status = node_->status();
    if (!status.at("health").is_null())
      for (const auto& s : status.at("health").at("services"))
        if (s.at("id") == impl_->session_id)
          return s.at("pid").get<std::uint64_t>();
  }
  return 0;
}
ServiceEndpoint TradingClient::endpoint() const {
  std::lock_guard lock(mutex_);
  return *impl_->remote;
}
void TradingClient::execute(const Json& command) {
  std::lock_guard lock(mutex_);
  wire::Request request;
  *request.mutable_command() = protocol::encode_command(command);
  impl_->call(std::move(request));
}
Json TradingClient::snapshot() {
  std::lock_guard lock(mutex_);
  try {
    wire::Request request;
    if (impl_->remote)
      request.mutable_attach();
    else
      request.mutable_snapshot();
    return impl_->call(std::move(request));
  } catch (const Error&) {
    if (impl_->last_snapshot.is_null()) {
      if (impl_->remote)
        return nullptr;
      throw;
    }
    auto snapshot = impl_->last_snapshot;
    snapshot["storage_state"] = "recovery_required";
    snapshot["connection_state"] = "disconnected";
    return snapshot;
  }
}
} // namespace asterion::terminal
