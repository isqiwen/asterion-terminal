#pragma once
#include <asterion/foundation/error.hpp>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
// The accepting side of a local IPC endpoint for tests. Services accept through
// RpcHost; this peer speaks the same framing (a 4-byte network-order length,
// then the payload) written independently of ipc::Channel.
namespace asterion::testing_support {
namespace local_detail {
using Clock = std::chrono::steady_clock;
[[noreturn]] inline void failed(const char* message) {
  throw Error(ErrorCode::unavailable, message);
}
inline void ready(int fd, short events, Clock::time_point end) {
  pollfd descriptor{fd, events, 0};
  for (;;) {
    const auto left = std::chrono::ceil<std::chrono::milliseconds>(end - Clock::now()).count();
    if (left <= 0)
      failed("test IPC timeout");
    const int result = ::poll(&descriptor, 1, static_cast<int>(std::min<long long>(left, 60000)));
    if (result < 0 && errno == EINTR)
      continue;
    if (result < 0 || (descriptor.revents & POLLNVAL))
      failed("test IPC channel closed");
    if (result > 0)
      return;
  }
}
inline void configure(int fd) {
  if (::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 || ::fcntl(fd, F_SETFL, O_NONBLOCK) < 0)
    failed("cannot configure test IPC socket");
#ifdef __APPLE__
  int enabled = 1;
  if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) < 0)
    failed("cannot configure test IPC socket");
#endif
}
} // namespace local_detail
class LocalPeer {
public:
  explicit LocalPeer(int fd) : fd_(fd) {}
  LocalPeer(LocalPeer&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
  LocalPeer& operator=(LocalPeer&& other) noexcept {
    if (this != &other) {
      close();
      fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
  }
  LocalPeer(const LocalPeer&) = delete;
  ~LocalPeer() { close(); }
  void close() noexcept {
    if (fd_ >= 0)
      ::close(fd_);
    fd_ = -1;
  }
  std::string receive(std::chrono::milliseconds timeout) {
    const auto end = local_detail::Clock::now() + timeout;
    try {
      std::array<unsigned char, 4> header{};
      transfer(reinterpret_cast<char*>(header.data()), header.size(), false, end);
      const std::size_t size = (std::size_t{header[0]} << 24) | (std::size_t{header[1]} << 16) |
                               (std::size_t{header[2]} << 8) | header[3];
      std::string payload(size, '\0');
      transfer(payload.data(), size, false, end);
      return payload;
    } catch (...) {
      close();
      throw;
    }
  }
  void send(const std::string& payload, std::chrono::milliseconds timeout) {
    const auto end = local_detail::Clock::now() + timeout;
    try {
      const auto size = payload.size();
      std::array<unsigned char, 4> header{
          static_cast<unsigned char>(size >> 24), static_cast<unsigned char>(size >> 16),
          static_cast<unsigned char>(size >> 8), static_cast<unsigned char>(size)};
      transfer(reinterpret_cast<char*>(header.data()), header.size(), true, end);
      auto bytes = payload;
      transfer(bytes.data(), bytes.size(), true, end);
    } catch (...) {
      close();
      throw;
    }
  }

private:
  void transfer(char* data, std::size_t size, bool writing, local_detail::Clock::time_point end) {
    if (fd_ < 0)
      local_detail::failed("test IPC channel closed");
#ifdef MSG_NOSIGNAL
    constexpr int send_flags = MSG_NOSIGNAL;
#else
    constexpr int send_flags = 0;
#endif
    while (size) {
      local_detail::ready(fd_, writing ? POLLOUT : POLLIN, end);
      const auto count = writing ? ::send(fd_, data, size, send_flags) : ::recv(fd_, data, size, 0);
      if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
        continue;
      if (count <= 0)
        local_detail::failed("test IPC peer disconnected");
      data += count;
      size -= static_cast<std::size_t>(count);
    }
  }
  int fd_;
};
class LocalListener {
public:
  explicit LocalListener(std::string endpoint, int pending_connections = 1)
      : endpoint_(std::move(endpoint)) {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (endpoint_.empty() || endpoint_.size() >= sizeof(address.sun_path))
      local_detail::failed("invalid test IPC endpoint");
    std::memcpy(address.sun_path, endpoint_.c_str(), endpoint_.size() + 1);
    fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0)
      local_detail::failed("cannot create test IPC listener");
    try {
      local_detail::configure(fd_);
      if (::bind(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) < 0)
        local_detail::failed("test IPC endpoint already exists or is inaccessible");
      bound_ = true;
      if (::chmod(endpoint_.c_str(), 0600) < 0 || ::listen(fd_, pending_connections) < 0)
        local_detail::failed("cannot listen on test IPC socket");
    } catch (...) {
      release();
      throw;
    }
  }
  LocalListener(const LocalListener&) = delete;
  LocalListener& operator=(const LocalListener&) = delete;
  ~LocalListener() { release(); }
  LocalPeer accept(std::chrono::milliseconds timeout) {
    const auto end = local_detail::Clock::now() + timeout;
    for (;;) {
      local_detail::ready(fd_, POLLIN, end);
      const int accepted = ::accept(fd_, nullptr, nullptr);
      if (accepted >= 0) {
        LocalPeer peer(accepted);
        local_detail::configure(accepted);
        return peer;
      }
      if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
        local_detail::failed("test IPC accept failed");
    }
  }

private:
  void release() noexcept {
    if (fd_ >= 0)
      ::close(fd_);
    fd_ = -1;
    if (bound_) {
      std::error_code ignored;
      std::filesystem::remove(endpoint_, ignored);
    }
    bound_ = false;
  }
  std::string endpoint_;
  int fd_ = -1;
  bool bound_ = false;
};
} // namespace asterion::testing_support
