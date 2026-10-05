#include <algorithm>
#include <asterion/foundation/error.hpp>
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/process/owner.hpp>
#include <asterion/kernel/service_host.hpp>
#include <asterion/kernel/logger.hpp>
#include <asterion/foundation/serialization.hpp>
#include <asterion/kernel/thread_pool.hpp>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#endif

namespace asterion::service {
namespace {
using Clock = std::chrono::steady_clock;
std::atomic<bool> stop_flag{false};
static_assert(std::atomic<bool>::is_always_lock_free, "stop flag is set from a signal handler");
#ifdef _WIN32
BOOL WINAPI console_stop(DWORD event) {
  switch (event) {
  case CTRL_C_EVENT:
  case CTRL_BREAK_EVENT:
  case CTRL_CLOSE_EVENT:
  case CTRL_SHUTDOWN_EVENT:
    stop_flag.store(true);
    return TRUE;
  default:
    return FALSE;
  }
}
#else
extern "C" void signal_stop(int) {
  stop_flag.store(true);
}
#endif
// Sleeps up to `duration`, returning early when `stop` is requested.
void pause(std::stop_token stop, Milliseconds duration) {
  std::mutex mutex;
  std::condition_variable_any wake;
  std::unique_lock lock(mutex);
  wake.wait_for(lock, stop, duration, [] { return false; });
}
template <class C> class ChannelConnection final : public Connection {
public:
  ChannelConnection(C channel, Clock::time_point accepted)
      : channel_(std::move(channel)), accepted_(accepted) {}
  std::string receive(Milliseconds timeout) override { return channel_.receive(timeout); }
  void send(const std::string& payload, Milliseconds timeout) override {
    channel_.send(payload, timeout);
  }
  Clock::time_point accepted_at() const noexcept override { return accepted_; }
  std::string peer_address() const override {
    if constexpr (requires { channel_.peer_address(); })
      return channel_.peer_address();
    else
      return {};
  }
  ipc::PeerRole peer_role() const override {
    if constexpr (requires { channel_.peer_role(); })
      return channel_.peer_role();
    else
      return ipc::PeerRole::local;
  }

private:
  C channel_;
  Clock::time_point accepted_;
};
} // namespace

void Transport::validate() const {
  const bool tls_complete =
      !tls.ca_file.empty() && !tls.certificate_file.empty() && !tls.private_key_file.empty();
  const bool tls_empty =
      tls.ca_file.empty() && tls.certificate_file.empty() && tls.private_key_file.empty();
  if (remote() ? (!endpoint.empty() || !port || !tls_complete)
               : (endpoint.empty() || port || !tls_empty))
    throw std::invalid_argument("choose --endpoint OR --bind/--port with all three TLS files");
}

void install_stop_signals() {
#ifdef _WIN32
  if (!SetConsoleCtrlHandler(console_stop, TRUE))
    throw std::runtime_error("cannot install console stop handler");
#else
  struct sigaction action{};
  action.sa_handler = signal_stop;
  sigemptyset(&action.sa_mask);
  if (::sigaction(SIGINT, &action, nullptr) != 0 || ::sigaction(SIGTERM, &action, nullptr) != 0)
    throw std::runtime_error("cannot install stop signal handlers");
  // Vendor libraries own sockets that our IPC wrappers cannot configure.
  // A disconnected peer must produce EPIPE, not terminate the whole service.
  action.sa_handler = SIG_IGN;
  if (::sigaction(SIGPIPE, &action, nullptr) != 0)
    throw std::runtime_error("cannot install stop signal handlers");
#endif
}
void request_stop() noexcept {
  stop_flag.store(true);
}
bool stop_requested() noexcept {
  return stop_flag.load();
}
void reset_stop_request() noexcept {
  stop_flag.store(false);
}

struct OwnerWatch::Impl {
  std::unique_ptr<ProcessOwner> owner;
  std::jthread thread;
};
OwnerWatch::OwnerWatch(std::uint64_t pid, Milliseconds grace) : impl_(std::make_unique<Impl>()) {
  if (!pid)
    return;
  impl_->owner = std::make_unique<ProcessOwner>(pid);
  impl_->thread = std::jthread([owner = impl_->owner.get(), grace](std::stop_token stop) {
    while (!stop.stop_requested()) {
      if (!owner->alive()) {
        request_stop();
        pause(stop, grace);
        // Destroying the watch requests stop; reaching here means the
        // process did not finish its shutdown within the grace period.
        if (!stop.stop_requested())
          std::_Exit(4);
        return;
      }
      pause(stop, Milliseconds{200});
    }
  });
}
OwnerWatch::~OwnerWatch() = default;

struct HealthChannel::Impl {
  std::unique_ptr<ipc::Listener> listener;
  std::jthread thread;
};
HealthChannel::HealthChannel(std::string endpoint, Responder respond)
    : impl_(std::make_unique<Impl>()) {
  if (endpoint.empty())
    return;
  if (!respond)
    throw std::invalid_argument("health channel requires a responder");
  impl_->listener = std::make_unique<ipc::Listener>(std::move(endpoint));
  impl_->thread = std::jthread([listener = impl_->listener.get(),
                                respond = std::move(respond)](std::stop_token stop) {
    std::uint64_t failures = 0;
    while (!stop.stop_requested()) {
      bool accepted = false;
      const auto started = Clock::now();
      try {
        auto channel = listener->accept(Milliseconds{200});
        accepted = true;
        const auto reply = respond(channel.receive(Milliseconds{1000}));
        if (!reply.empty())
          channel.send(reply, Milliseconds{1000});
      } catch (const std::exception& error) {
        if (accepted || Clock::now() - started < Milliseconds{10}) {
          log_process_failure("transport", "health.connection_failed", classify(error), ++failures);
          if (!accepted)
            pause(stop, Milliseconds{50});
        }
      } catch (...) {
        log_process_failure("transport", "health.connection_failed", ErrorCode::internal_error,
                            ++failures);
      }
    }
  });
}
HealthChannel::~HealthChannel() {
  // Join before the listener it borrows is destroyed.
  if (impl_)
    impl_->thread = {};
}

struct ServiceHost::Impl {
  Transport transport;
  Handler handler;
  HostOptions options;
  std::stop_source stop;
  std::atomic<std::size_t> active{0};
  // Accepted connections submitted to the pool but not yet started.
  std::atomic<std::size_t> pending{0};
  std::unique_ptr<ThreadPool> pool;
  std::unique_ptr<ipc::TlsListener> tcp;
  std::unique_ptr<ipc::Listener> local;
  struct Admission {
    ipc::TlsPendingConnection peer;
    Clock::time_point accepted;
  };
  std::vector<Admission> handshakes;
  std::atomic<std::uint64_t> handler_failures{0}, handshake_failures{0}, accept_failures{0},
      admission_failures{0};

  template <class Pending> void dispatch(Pending pending_connection, Clock::time_point accepted) {
    auto peer = std::make_shared<Pending>(std::move(pending_connection));
    // Only the accept thread admits, so this bound cannot be overshot; a
    // rejected connection closes when `peer` is destroyed.
    if (active.load() + pending.load() >= options.workers + options.queue)
      throw Error(ErrorCode::resource_exhausted, "service is at capacity");
    ++pending;
    try {
      static_cast<void>(pool->submit([this, peer, accepted](std::stop_token) {
        // Count before checking stop so a drain never misses a running handler.
        ++active;
        --pending;
        struct Release {
          std::atomic<std::size_t>& count;
          ~Release() { --count; }
        } release{active};
        const auto token = stop.get_token();
        if (token.stop_requested())
          return;
        try {
          ChannelConnection<Pending> connection(std::move(*peer), accepted);
          handler(connection, token);
        } catch (const std::exception& error) {
          log_process_failure("transport", "connection.handler_failed", classify(error),
                              ++handler_failures);
        } catch (...) {
          log_process_failure("transport", "connection.handler_failed", ErrorCode::internal_error,
                              ++handler_failures);
        }
      }));
    } catch (...) {
      --pending;
      throw;
    }
  }
  void poll_handshakes() {
    std::erase_if(handshakes, [&](Admission& admitted) {
      try {
        auto channel = admitted.peer.poll_handshake();
        if (!channel)
          return false;
        if (std::ranges::find(options.roles, channel->peer_role()) == options.roles.end())
          throw Error(ErrorCode::permission_denied, "service peer role is not admitted");
        dispatch(std::move(*channel), admitted.accepted);
      } catch (const std::exception& error) {
        log_process_failure("transport", "connection.handshake_failed", classify(error),
                            ++handshake_failures);
      }
      return true;
    });
  }
  template <class Accept> void serve(Accept accept) {
    while (!stop_requested()) {
      const auto started = Clock::now();
      std::optional<decltype(accept())> connection;
      try {
        connection.emplace(accept());
      } catch (const std::exception& error) {
        // Idle poll ends after `poll`; immediate failures (for example
        // descriptor exhaustion) must be visible and must never spin.
        if (Clock::now() - started < Milliseconds{10}) {
          log_process_failure("transport", "connection.accept_failed", classify(error),
                              ++accept_failures);
          std::this_thread::sleep_for(Milliseconds{50});
        }
      }
      if (connection) {
        try {
          const auto accepted = Clock::now();
          if constexpr (std::is_same_v<decltype(*connection), ipc::TlsPendingConnection&>) {
            if (handshakes.size() >= options.handshake_connections)
              throw Error(ErrorCode::resource_exhausted, "TLS admission is at capacity");
            connection->start_handshake(options.handshake);
            handshakes.push_back({std::move(*connection), accepted});
          } else
            dispatch(std::move(*connection), accepted);
        } catch (const std::exception& error) {
          log_process_failure("transport", "connection.admission_failed", classify(error),
                              ++admission_failures);
        }
      }
      poll_handshakes();
      if (options.tick)
        options.tick();
    }
  }
};
ServiceHost::ServiceHost(Transport transport, Handler handler, HostOptions options)
    : impl_(std::make_unique<Impl>()) {
  transport.validate();
  if (!handler)
    throw std::invalid_argument("service host requires a handler");
  impl_->transport = std::move(transport);
  impl_->handler = std::move(handler);
  impl_->options = std::move(options);
  if (!impl_->options.workers)
    throw std::invalid_argument("service host requires at least one worker");
  if (!impl_->options.handshake_connections || impl_->options.handshake_connections > 256 ||
      impl_->options.handshake.count() <= 0)
    throw std::invalid_argument("service host requires bounded positive TLS admission");
  // Capacity for every admitted connection; admission enforces the bound.
  impl_->pool = std::make_unique<ThreadPool>(impl_->options.workers,
                                             impl_->options.workers + impl_->options.queue);
  if (impl_->transport.remote())
    impl_->tcp = std::make_unique<ipc::TlsListener>(impl_->transport.bind, impl_->transport.port,
                                                    impl_->transport.tls);
  else
    impl_->local = std::make_unique<ipc::Listener>(impl_->transport.endpoint);
}
ServiceHost::~ServiceHost() = default;
bool ServiceHost::run() {
  auto& impl = *impl_;
  if (impl.tcp)
    impl.serve([&] { return impl.tcp->accept_pending(impl.options.poll); });
  else
    impl.serve([&] { return impl.local->accept(impl.options.poll); });
  // Stop accepting first; destroying a local listener unlinks its socket.
  impl.handshakes.clear();
  impl.tcp.reset();
  impl.local.reset();
  impl.stop.request_stop();
  const auto deadline = Clock::now() + impl.options.drain;
  while (impl.active.load() && Clock::now() < deadline)
    std::this_thread::sleep_for(Milliseconds{5});
  if (impl.active.load())
    return false;
  impl.pool->shutdown();
  return true;
}
} // namespace asterion::service
