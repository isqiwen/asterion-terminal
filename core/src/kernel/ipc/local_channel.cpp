#include <algorithm>
#include <array>
#include <asterion/foundation/error.hpp>
#include <asterion/kernel/ipc/local_channel.hpp>
#include <cstring>
#include <filesystem>
#include <limits>
#include <thread>
#include <stdexcept>
#ifdef _WIN32
// windows.h must precede sddl.h, which depends on its declarations.
#include <windows.h>
#include <sddl.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif
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
#ifdef _WIN32
using Handle = HANDLE;
const Handle invalid = INVALID_HANDLE_VALUE;
std::wstring pipe_name(const std::string& name) {
  if (!name.starts_with("asterion.") || name.size() > 100 ||
      name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-") !=
          std::string::npos)
    throw std::invalid_argument("invalid local pipe name");
  return L"\\\\.\\pipe\\" + std::wstring(name.begin(), name.end());
}
void release(Handle h) {
  if (h != invalid)
    CloseHandle(h);
}
void wait_operation(Handle file, OVERLAPPED& operation, Deadline end, DWORD& transferred) {
  DWORD wait = INFINITE;
  if (end != Deadline::max())
    wait = static_cast<DWORD>(std::clamp<std::int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end - Clock::now()).count(), 0,
        std::numeric_limits<int>::max()));
  if (WaitForSingleObject(operation.hEvent, wait) != WAIT_OBJECT_0) {
    CancelIoEx(file, &operation);
    GetOverlappedResult(file, &operation, &transferred, TRUE);
    failed("IPC timeout; command outcome may be unknown");
  }
  if (!GetOverlappedResult(file, &operation, &transferred, FALSE))
    failed("IPC peer disconnected");
}
void transfer(Handle file, char* data, std::size_t size, bool writing, Deadline end) {
  while (size) {
    (void)remaining(end);
    OVERLAPPED op{};
    op.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!op.hEvent)
      failed("cannot allocate IPC event");
    DWORD count = 0;
    try {
      const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(size, 65536));
      const BOOL result = writing ? WriteFile(file, data, chunk, &count, &op)
                                  : ReadFile(file, data, chunk, &count, &op);
      if (!result) {
        if (GetLastError() != ERROR_IO_PENDING)
          failed("IPC peer disconnected");
        wait_operation(file, op, end, count);
      }
      if (!count)
        failed("IPC peer disconnected");
    } catch (...) {
      CloseHandle(op.hEvent);
      throw;
    }
    CloseHandle(op.hEvent);
    data += count;
    size -= count;
  }
}
struct Security {
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
  Security() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
      failed("cannot identify IPC user");
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::string buffer(size, '\0');
    const BOOL ok = GetTokenInformation(token, TokenUser, buffer.data(), size, &size);
    CloseHandle(token);
    if (!ok)
      failed("cannot identify IPC user");
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid))
      failed("cannot encode IPC identity");
    const std::wstring acl = L"D:P(A;;GA;;;" + std::wstring(sid) + L")";
    LocalFree(sid);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl.c_str(), SDDL_REVISION_1,
                                                              &descriptor, nullptr))
      failed("cannot secure IPC pipe");
    attributes.lpSecurityDescriptor = descriptor;
  }
  ~Security() {
    if (descriptor)
      LocalFree(descriptor);
  }
};
#else
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
void same_user(Handle fd) {
#ifdef __APPLE__
  uid_t uid = 0;
  gid_t gid = 0;
  if (::getpeereid(fd, &uid, &gid) != 0 || uid != ::geteuid())
    failed("IPC peer identity rejected");
#else
  struct ucred credential{};
  socklen_t size = sizeof(credential);
  if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credential, &size) != 0 ||
      credential.uid != ::geteuid())
    failed("IPC peer identity rejected");
#endif
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
#endif
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
#ifdef _WIN32
  const auto name = pipe_name(endpoint);
  for (;;) {
    (void)connection_remaining(end);
    result.impl_->handle = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                       OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (result.impl_->handle != invalid)
      break;
    if (GetLastError() != ERROR_PIPE_BUSY)
      failed("IPC endpoint is not ready");
    const auto left = connection_remaining(end);
    const DWORD wait = left < 0 ? NMPWAIT_WAIT_FOREVER : static_cast<DWORD>(left);
    if (!WaitNamedPipeW(name.c_str(), wait)) {
      if (GetLastError() == ERROR_SEM_TIMEOUT)
        failed("IPC connection timeout; no command was sent");
      failed("IPC endpoint became unavailable");
    }
    // Another client may take the instance after WaitNamedPipe succeeds.
  }
#else
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
#endif
#ifndef _WIN32
  same_user(result.impl_->handle);
#endif
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
struct Listener::Impl {
  Handle handle = invalid;
  std::string endpoint;
  bool bound = false;
  ~Impl() {
    release(handle);
#ifndef _WIN32
    if (bound) {
      std::error_code error;
      std::filesystem::remove(endpoint, error);
    }
#endif
  }
};
Listener::Listener(std::string endpoint) : impl_(std::make_unique<Impl>()) {
  impl_->endpoint = std::move(endpoint);
#ifdef _WIN32
  Security security;
  impl_->handle =
      CreateNamedPipeW(pipe_name(impl_->endpoint).c_str(),
                       PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                       PIPE_UNLIMITED_INSTANCES, 65536, 65536, 0, &security.attributes);
  if (impl_->handle == invalid)
    failed("cannot create exclusive local IPC pipe");
#else
  const auto addr = address(impl_->endpoint);
  impl_->handle = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (impl_->handle < 0)
    failed("cannot create IPC listener");
  configure(impl_->handle);
  if (::bind(impl_->handle, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0)
    failed("IPC endpoint already exists or is inaccessible");
  impl_->bound = true;
  if (::chmod(impl_->endpoint.c_str(), 0600) < 0)
    failed("cannot secure IPC endpoint");
  if (::listen(impl_->handle, 1) < 0)
    failed("cannot listen on IPC socket");
#endif
}
Listener::~Listener() = default;
Channel Listener::accept(std::chrono::milliseconds timeout) {
  Channel result;
  const auto end = deadline(timeout);
#ifdef _WIN32
  OVERLAPPED op{};
  op.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!op.hEvent)
    failed("cannot allocate IPC event");
  try {
    if (!ConnectNamedPipe(impl_->handle, &op)) {
      const auto error = GetLastError();
      if (error == ERROR_IO_PENDING) {
        DWORD ignored = 0;
        wait_operation(impl_->handle, op, end, ignored);
      } else if (error != ERROR_PIPE_CONNECTED)
        failed("IPC accept failed");
    }
  } catch (...) {
    CloseHandle(op.hEvent);
    throw;
  }
  CloseHandle(op.hEvent);
  // Keep a pending instance alive before handing the connected one off. This
  // permits repeated/concurrent clients without an ownership gap in which a
  // different listener could acquire FILE_FLAG_FIRST_PIPE_INSTANCE.
  Security security;
  const auto next = CreateNamedPipeW(
      pipe_name(impl_->endpoint).c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
      PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
      PIPE_UNLIMITED_INSTANCES, 65536, 65536, 0, &security.attributes);
  if (next == invalid) {
    DisconnectNamedPipe(impl_->handle);
    failed("cannot replenish local IPC listener");
  }
  result.impl_->handle = impl_->handle;
  impl_->handle = next;
#else
  for (;;) {
    ready(impl_->handle, POLLIN, end);
    result.impl_->handle = ::accept(impl_->handle, nullptr, nullptr);
    if (result.impl_->handle >= 0)
      break;
    if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
      failed("IPC accept failed");
  }
  configure(result.impl_->handle);
  same_user(result.impl_->handle);
#endif
  return result;
}
} // namespace asterion::ipc
