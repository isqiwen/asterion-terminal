#pragma once
#include "local_listener.hpp"
#include <asterion/kernel/rpc_host.hpp>
#include <functional>
#include <list>
#include <stop_token>
#include <thread>
// Test peer for client tests: a local endpoint whose handlers may block, one
// thread per connection. Production services use RpcHost.
namespace asterion::testing_support {
class Connection {
public:
  explicit Connection(LocalPeer channel) : channel_(std::move(channel)) {}
  std::string receive(std::chrono::milliseconds timeout) { return channel_.receive(timeout); }
  void send(const std::string& payload, std::chrono::milliseconds timeout) {
    channel_.send(payload, timeout);
  }

private:
  LocalPeer channel_;
};
class BlockingService {
public:
  using Handler = std::function<void(Connection&, std::stop_token)>;
  BlockingService(const service::Transport& transport, Handler handler)
      : listener_(transport.endpoint), handler_(std::move(handler)) {}
  // Serves until service::stop_requested(), then joins every handler.
  bool run() {
    std::stop_source stop;
    std::list<std::jthread> handlers;
    while (!service::stop_requested()) {
      try {
        handlers.emplace_back(
            [this, token = stop.get_token(),
             connection = Connection(listener_.accept(std::chrono::milliseconds{20}))]() mutable {
              try {
                handler_(connection, token);
              } catch (const std::exception&) {
                // A failed handler closes only its connection.
              }
            });
      } catch (const std::exception&) {
        // Idle accept deadline.
      }
    }
    stop.request_stop();
    return true;
  }

private:
  LocalListener listener_;
  Handler handler_;
};
} // namespace asterion::testing_support
