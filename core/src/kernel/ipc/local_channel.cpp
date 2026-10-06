#include <algorithm>
#include <array>
#include <asterion/foundation/error.hpp>
#include <asterion/kernel/ipc/local_channel.hpp>
#include <cstring>
#include <filesystem>
#include <limits>
#include <thread>
#include <stdexcept>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include "local_security.hpp"
namespace asterion::ipc {
namespace {
using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;
[[noreturn]] void failed(const char* message) {
  throw Error(ErrorCode::unavailable, message);
}
Deadline deadline(std::chrono::milliseconds timeout) {
  if (timeout.count() < 0)
    return Deadline::max();
  return Clock::now() + timeout;
}
int remaining(Deadline end) {
  if (end == Deadline::max())
    return -1;
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - Clock::now()).count();
  if (ms <= 0)
    failed("IPC timeout; command outcome may be unknown");
  return static_cast<int>(std::min<std::int64_t>(ms, std::numeric_limits<int>::max()));
}
// Connection establishment has not sent a command. Capacity waits share this
// single deadline; frame failures never enter the connection retry loop.
int connection_remaining(Deadline end) {
  if (end == Deadline::max())
    return -1;
  const auto left = end - Clock::now();
  if (left <= Clock::duration::zero())
    failed("IPC connection timeout; no command was sent");
  const auto ms = std::chrono::ceil<std::chrono::milliseconds>(left).count();
  return static_cast<int>(std::min<std::int64_t>(ms, std::numeric_limits<int>::max()));
}
using Handle = int;
constexpr Handle invalid = -1;
void release(Handle h) {
  if (h >= 0)
    ::close(h);
}
void configure(Handle fd) {
  if (::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0 || ::fcntl(fd, F_SETFL, O_NONBLOCK) < 0)
    failed("cannot configure IPC socket");
#ifdef __APPLE__
  int enabled = 1;
  if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) < 0)
    failed("cannot configure IPC socket");
#endif
}
sockaddr_un address(const std::string& endpoint) {
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (endpoint.empty() || endpoint.front() != '/' || endpoint.find('\0') != std::string::npos ||
      endpoint.size() >= sizeof(addr.sun_path))
    throw std::invalid_argument("invalid local socket path");
  std::memcpy(addr.sun_path, endpoint.c_str(), endpoint.size() + 1);
  return addr;
}

void ready(Handle fd, short events, Deadline end) {
  pollfd descriptor{fd, events, 0};
  for (;;) {
    const int result = ::poll(&descriptor, 1, remaining(end));
    if (result < 0 && errno == EINTR)
      continue;
    if (result <= 0)
      failed("IPC timeout or transport failure; command outcome may be unknown");
    if (descriptor.revents & POLLNVAL)
      failed("IPC channel closed");
    return;
  }
}
void transfer(Handle fd, char* data, std::size_t size, bool writing, Deadline end) {
  while (size) {
    ready(fd, writing ? POLLOUT : POLLIN, end);
#ifdef MSG_NOSIGNAL
    constexpr int send_flags = MSG_NOSIGNAL;
#else
    constexpr int send_flags = 0;
#endif
    const auto count = writing ? ::send(fd, data, size, send_flags) : ::recv(fd, data, size, 0);
    if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
      continue;
    if (count <= 0)
      failed("IPC peer disconnected");
    data += count;
    size -= static_cast<std::size_t>(count);
  }
}
} // namespace
struct Channel::Impl {
  Handle handle = invalid;
  ~Impl() { release(handle); }
};
Channel::Channel() : impl_(std::make_unique<Impl>()) {}
Channel::~Channel() = default;
Channel::Channel(Channel&&) noexcept = default;
Channel& Channel::operator=(Channel&&) noexcept = default;
void Channel::close() noexcept {
  if (impl_) {
    release(impl_->handle);
    impl_->handle = invalid;
  }
}
Channel Channel::connect(const std::string& endpoint, std::chrono::milliseconds timeout) {
  const auto end = deadline(timeout);
  Channel result;
  const auto addr = address(endpoint);
  for (;;) {
    (void)connection_remaining(end);
    result.impl_->handle = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (result.impl_->handle < 0)
      failed("cannot create IPC socket");
    configure(result.impl_->handle);
    if (::connect(result.impl_->handle, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) ==
        0)
      break;
    int error = errno;
    if (error == EINPROGRESS) {
      ready(result.impl_->handle, POLLOUT, end);
      socklen_t length = sizeof(error);
      if (::getsockopt(result.impl_->handle, SOL_SOCKET, SO_ERROR, &error, &length) < 0)
        failed("IPC connection failed");
      if (error == 0)
        break;
    }
    // A full AF_UNIX listen queue is EAGAIN on Linux, ECONNREFUSED on
    // macOS. Refused endpoints may also be stale: wait only to the caller's
    // deadline. Invalid paths, permissions and identity errors never retry.
    if (error != EAGAIN && error != EWOULDBLOCK && error != ECONNREFUSED && error != EINTR)
      failed("IPC endpoint is not ready");
    result.close();
    const auto left = connection_remaining(end);
    std::this_thread::sleep_for(std::chrono::milliseconds(left < 0 ? 10 : std::min(left, 10)));
    // A failed socket's state is unspecified. Recreate rather than reuse it.
  }
  detail::verify_local_peer(result.impl_->handle);
  return result;
}
std::string Channel::receive(std::chrono::milliseconds timeout) {
  if (!impl_ || impl_->handle == invalid)
    failed("IPC channel closed");
  auto end = deadline(timeout);
  std::array<unsigned char, 4> header{};
  try {
    transfer(impl_->handle, reinterpret_cast<char*>(header.data()), 1, false, end);
    if (timeout.count() < 0)
      end = Clock::now() + std::chrono::seconds(10);
    transfer(impl_->handle, reinterpret_cast<char*>(header.data()) + 1, 3, false, end);
    const std::size_t size = (std::size_t(header[0]) << 24) | (std::size_t(header[1]) << 16) |
                             (std::size_t(header[2]) << 8) | header[3];
    if (!size || size > max_frame)
      throw Error(ErrorCode::resource_exhausted, "invalid IPC frame length");
    std::string result(size, '\0');
    transfer(impl_->handle, result.data(), size, false, end);
    return result;
  } catch (...) {
    close();
    throw;
  }
}
void Channel::send(const std::string& payload, std::chrono::milliseconds timeout) {
  if (payload.empty() || payload.size() > max_frame)
    throw Error(ErrorCode::resource_exhausted, "IPC frame too large or empty");
  if (!impl_ || impl_->handle == invalid)
    failed("IPC channel closed");
  const auto size = static_cast<std::uint32_t>(payload.size());
  std::array<unsigned char, 4> header{
      static_cast<unsigned char>(size >> 24), static_cast<unsigned char>(size >> 16),
      static_cast<unsigned char>(size >> 8), static_cast<unsigned char>(size)};
  const auto end = deadline(timeout);
  try {
    transfer(impl_->handle, reinterpret_cast<char*>(header.data()), header.size(), true, end);
    transfer(impl_->handle, const_cast<char*>(payload.data()), payload.size(), true, end);
  } catch (...) {
    close();
    throw;
  }
}
} // namespace asterion::ipc
