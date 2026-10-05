#pragma once
#include <asterion/kernel/service_host.hpp>
#include <optional>
#include <asterion/kernel/progress.hpp>
namespace asterion::service {
// Request/reply transport with one bounded, nonblocking I/O owner. A business
// request may outlive a socket; closing a connection never cancels or replays it.
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
    Milliseconds handshake{10000}, receive{30000}, send{10000}, drain{400};
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
