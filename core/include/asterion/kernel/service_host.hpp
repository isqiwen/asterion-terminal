#pragma once
#include <asterion/kernel/ipc/tls_channel.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>
#include <stdexcept>

// Common process skeleton for Asterion services: transport selection, stop
// signals, supervisor liveness, the private health channel and a listener
// whose connections are served by a bounded pool. Business routing, request
// framing and protocol validation stay in each application.
namespace asterion::service {
using Milliseconds = std::chrono::milliseconds;

// Exactly one of a private local IPC endpoint or TCP with mutual TLS.
// Plaintext TCP is never offered.
struct Transport {
  std::string endpoint;
  std::string bind;
  std::uint16_t port = 0;
  ipc::TlsIdentity tls;
  bool remote() const noexcept { return !bind.empty(); }
  // Throws std::invalid_argument unless exactly one complete transport is set.
  void validate() const;
};

// Frame I/O of one accepted connection (already authenticated for TCP).
class Connection {
public:
  virtual ~Connection() = default;
  virtual std::string receive(Milliseconds timeout) = 0;
  virtual void send(const std::string& payload, Milliseconds timeout) = 0;
  // When the transport was accepted, before queueing and authentication, so
  // handlers can bound the whole admission rather than only their own reads.
  virtual std::chrono::steady_clock::time_point accepted_at() const noexcept = 0;
  // Remote IP for TCP peers; empty for local IPC.
  virtual std::string peer_address() const = 0;
  // Certificate role for TCP peers; local for same-user IPC peers.
  virtual ipc::PeerRole peer_role() const = 0;
};

// Process-wide cooperative stop request. install_stop_signals() routes
// SIGINT/SIGTERM (console control events on Windows) to it; owner loss and
// applications may request it too. POSIX services ignore SIGPIPE so broken
// writes (including inside vendor libraries) report EPIPE instead of exit.
void install_stop_signals();
void request_stop() noexcept;
bool stop_requested() noexcept;
// Clears a stop request. Only for hosts that run several service lifetimes in
// one process, such as tests.
void reset_stop_request() noexcept;

// Requests stop when the supervising process disappears. If the process has
// not exited `grace` later it terminates immediately with exit code 4: durable
// state never depends on a graceful shutdown. pid 0 disables the watch.
class OwnerWatch {
public:
  explicit OwnerWatch(std::uint64_t pid, Milliseconds grace = Milliseconds{2000});
  ~OwnerWatch();
  OwnerWatch(const OwnerWatch&) = delete;
  OwnerWatch& operator=(const OwnerWatch&) = delete;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Supervisor health: one bounded request per connection on a dedicated
// thread, independent of the business pool. respond() returns the reply
// frame, or an empty string to close without replying. Its exceptions close
// only that connection. An empty endpoint disables the channel.
class HealthChannel {
public:
  using Responder = std::function<std::string(const std::string& frame)>;
  HealthChannel(std::string endpoint, Responder respond);
  ~HealthChannel();
  HealthChannel(const HealthChannel&) = delete;
  HealthChannel& operator=(const HealthChannel&) = delete;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

struct HostOptions {
  // Concurrent handlers, and connections allowed to wait while all workers are
  // busy. Admission counts both, so a burst is never rejected merely because
  // idle workers have not yet picked up queued connections. queue may be 0.
  std::size_t workers = 8;
  std::size_t queue = 8;
  // Unauthenticated sockets are polled separately from business workers.
  // Admission beyond this bound closes immediately; new connections may retry
  // explicitly after capacity recovers. No trading command is replayed.
  std::size_t handshake_connections = 16;
  // Mutual TLS must complete within this bound, measured from TCP admission.
  Milliseconds handshake{10000};
  // Accept poll interval; bounds stop latency and the tick period.
  Milliseconds poll{200};
  // After stop, in-flight handlers get this long to finish before run()
  // reports an incomplete drain. Agent escalates SIGTERM after 500 ms.
  Milliseconds drain{400};
  // Optional housekeeping on the accept thread after every poll.
  std::function<void()> tick;
  // TCP peers whose certificate carries none of these roles are closed after
  // the handshake, before the handler runs. Local IPC peers are always
  // admitted (same OS user). Operation-level checks stay in the handler.
  std::vector<ipc::PeerRole> roles{ipc::PeerRole::admin, ipc::PeerRole::client,
                                   ipc::PeerRole::service};
};

// Binds the listener on construction, so a process can claim its endpoint
// before starting anything else. The accept thread progresses bounded nonblocking
// TLS handshakes; only authenticated connections consume the business pool.
// Overload drops the new connection without holding a worker.
// The handler owns framing and must bound every receive; its exceptions close
// only that connection.
class ServiceHost {
public:
  using Handler = std::function<void(Connection&, std::stop_token)>;
  ServiceHost(Transport transport, Handler handler, HostOptions options = {});
  ~ServiceHost();
  ServiceHost(const ServiceHost&) = delete;
  ServiceHost& operator=(const ServiceHost&) = delete;
  // Serves until stop_requested(). Returns true after a complete drain. On
  // false, handlers are still blocked in bounded reads: the caller must end
  // the process without running destructors (std::_Exit).
  [[nodiscard]] bool run();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace asterion::service
