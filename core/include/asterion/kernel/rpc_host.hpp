#pragma once
#include <asterion/kernel/ipc/tls_channel.hpp>
#include <asterion/kernel/progress.hpp>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
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
// Process-wide cooperative stop request. install_stop_signals() routes
// SIGINT/SIGTERM to it; owner loss and applications may request it too.
// Services ignore SIGPIPE so broken writes (including inside vendor
// libraries) report EPIPE instead of exit.
void install_stop_signals();
void request_stop() noexcept;
bool stop_requested() noexcept;
// Clears a stop request. Only for hosts that run several service lifetimes in
// one process, such as tests.
void reset_stop_request() noexcept;
// Asks the I/O owner to advance now. A thread calls it when it has finished
// work that a pending reply waits for; the owner would otherwise notice at its
// next periodic check. Safe from any thread, and without effect when no host
// is running.
void wake_io_owner();
// Request/reply transport with one bounded, nonblocking I/O owner. A business
// request may outlive a socket; closing a connection never cancels or replays it.
// The owner advances pending replies and the service's own state when a
// request arrives, when a request's last reply leaves, when wake_io_owner() is
// called, and otherwise every 10 ms, which is also how it observes deadlines
// and stop requests and paces the frames of a stream.
class RpcHost {
public:
  struct Peer {
    ipc::PeerRole role;
    std::string address;
  };
  // Called only on the I/O owner. Return no value while business work is pending.
  // Both functions must return promptly: the business state owner performs work.
  struct Message {
    std::string payload;
    // Keep polling this reply after the frame is sent. Only one frame is
    // produced at a time; a slow reader cannot accumulate queued publications.
    bool more = false;
    Message(std::string payload, bool more = false) : payload(std::move(payload)), more(more) {}
  };
  using Reply = std::function<std::optional<Message>()>;
  using Handler = std::function<Reply(const Peer&, std::string)>;
  enum class Stage { running, draining_replies, stopping_resources };
  struct LocalEndpoint {
    std::string path;
    Handler handler;
    std::size_t connections = 4;
    bool during_drain = false;
    // Private control lanes do not inherit a public bulk-input frame limit.
    std::size_t request_bytes = 1024 * 1024;
    std::size_t payload_bytes = 4 * 1024 * 1024;
  };
  struct Options {
    // Runs only on the I/O owner. Close business admission in draining_replies;
    // stop owned resources in stopping_resources and return true once they stopped.
    std::function<bool(Stage)> advance;
    std::uint64_t owner_pid = 0;
    // Independent same-user admission lanes share this host's I/O owner.
    std::vector<LocalEndpoint> local_endpoints;
    std::size_t connections = 80;
    std::size_t handshakes = 16;
    std::size_t request_bytes = 1024 * 1024;
    // Aggregate wire payload allowance for this endpoint. Request reservations
    // last through business completion; reply buffers last through socket write.
    // Private endpoints have independent allowances so bulk traffic cannot use
    // their control/completion capacity. Parsed domain objects are additional.
    std::size_t payload_bytes = 64 * 1024 * 1024;
    // Instrumented builds run several times slower; draining gets the same
    // margin there, so a clean stop is not cut short by the instrumentation.
#ifdef ASTERION_SANITIZED
    Milliseconds handshake{10000}, receive{30000}, send{10000}, drain{8000};
#else
    Milliseconds handshake{10000}, receive{30000}, send{10000}, drain{400};
#endif
    std::vector<ipc::PeerRole> roles{ipc::PeerRole::admin, ipc::PeerRole::client,
                                     ipc::PeerRole::service};
  };
  RpcHost(Transport transport, Handler handler, Options options, Progress& progress);
  ~RpcHost();
  RpcHost(const RpcHost&) = delete;
  RpcHost& operator=(const RpcHost&) = delete;
  // On incomplete drain, pending replies stay owned by this host. The caller
  // must exit without destroying it or the resource owners those replies use.
  [[nodiscard]] bool run();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace asterion::service
