#pragma once
#include <chrono>
#include <memory>
#include <string>
namespace asterion::ipc {
// One owner per stream, no concurrent reads/writes. 4-byte network-order length
// prefix, bounded payloads, whole-frame deadlines. Payload format is
// independent.
class Channel {
public:
  // Task datasets of up to 200000 bars travel as one message.
  static constexpr std::size_t max_frame = 64 * 1024 * 1024;
  Channel();
  ~Channel();
  Channel(Channel&&) noexcept;
  Channel& operator=(Channel&&) noexcept;
  Channel(const Channel&) = delete;
  Channel& operator=(const Channel&) = delete;
  // Busy local endpoints wait within one deadline. No application bytes are
  // sent here, and send/receive failures never reconnect or replay frames.
  static Channel connect(const std::string& endpoint, std::chrono::milliseconds timeout);
  std::string receive(std::chrono::milliseconds timeout);
  void send(const std::string& payload, std::chrono::milliseconds timeout);
  void close() noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  friend class Listener;
};
class Listener {
public:
  explicit Listener(std::string endpoint, int pending_connections = 1);
  ~Listener();
  Listener(const Listener&) = delete;
  Listener& operator=(const Listener&) = delete;
  Channel accept(std::chrono::milliseconds timeout);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace asterion::ipc
