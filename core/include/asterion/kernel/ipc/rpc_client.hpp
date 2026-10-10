#pragma once
#include <asterion/kernel/ipc/tls_channel.hpp>
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/payload_budget.hpp>
#include <future>
#include <functional>
#include <optional>
namespace asterion::ipc {
// The event loop of one owner thread. Clients created on it share it, so their
// owner waits for any of them here instead of sleeping between polls.
class Reactor {
public:
  Reactor();
  ~Reactor();
  Reactor(const Reactor&) = delete;
  Reactor& operator=(const Reactor&) = delete;
  // Owner thread only. Returns once an I/O handler of any client has run, once
  // wake() has been called since the previous wait began, or when `limit` has
  // passed; without a limit it waits for one of the first two.
  void wait(std::optional<std::chrono::milliseconds> limit);
  // Ends the current wait, or the next one. Safe from any thread.
  void wake();

private:
  friend class RpcClient;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
// One caller owns this client and pumps its I/O. No application threads, request
// retries or unbounded queue: each admitted request owns one connection.
class RpcClient {
public:
  RpcClient(std::string endpoint, std::size_t capacity, PayloadBudget payloads,
            std::size_t frame_bytes = Channel::max_frame);
  RpcClient(std::string host, std::uint16_t port, const TlsIdentity& identity, std::size_t capacity,
            PayloadBudget payloads, std::size_t frame_bytes = Channel::max_frame);
  // The same clients on their owner's reactor, which must outlive them.
  RpcClient(Reactor& reactor, std::string endpoint, std::size_t capacity, PayloadBudget payloads,
            std::size_t frame_bytes = Channel::max_frame);
  RpcClient(Reactor& reactor, std::string host, std::uint16_t port, const TlsIdentity& identity,
            std::size_t capacity, PayloadBudget payloads,
            std::size_t frame_bytes = Channel::max_frame);
  ~RpcClient();
  RpcClient(const RpcClient&) = delete;
  RpcClient& operator=(const RpcClient&) = delete;
  std::future<Payload> request(
      std::string payload, std::chrono::milliseconds reply_timeout,
      std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max(),
      std::chrono::milliseconds send_timeout = std::chrono::seconds(5));
  // One frame per stream per poll; handler runs on the polling owner. Completion
  // reports closure, cancellation or a handler failure. No frame queue or retry.
  std::future<void> watch(std::string payload, std::chrono::milliseconds frame_timeout,
                          std::function<void(Payload)> frame);
  void cancel();
  // Zero polls without waiting. A worker may wait briefly for I/O instead of
  // sleeping between polls; callbacks and stream frames still run on its owner.
  void poll(std::chrono::milliseconds wait = std::chrono::milliseconds::zero());

private:
  RpcClient(Reactor* reactor, std::string endpoint, std::size_t capacity, PayloadBudget payloads,
            std::size_t frame_bytes);
  RpcClient(Reactor* reactor, std::string host, std::uint16_t port, const TlsIdentity& identity,
            std::size_t capacity, PayloadBudget payloads, std::size_t frame_bytes);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace asterion::ipc
