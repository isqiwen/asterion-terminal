#pragma once
#include <asterion/kernel/ipc/tls_channel.hpp>
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/payload_budget.hpp>
#include <future>
#include <functional>
namespace asterion::ipc {
// One caller owns this client and pumps its I/O. No application threads, request
// retries or unbounded queue: each admitted request owns one connection.
class RpcClient {
public:
  RpcClient(std::string endpoint, std::size_t capacity, PayloadBudget payloads,
            std::size_t frame_bytes = Channel::max_frame);
  RpcClient(std::string host, std::uint16_t port, const TlsIdentity& identity, std::size_t capacity,
            PayloadBudget payloads, std::size_t frame_bytes = Channel::max_frame);
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
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace asterion::ipc
