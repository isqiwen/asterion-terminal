#include <asterion/kernel/process/owner.hpp>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#include <csignal>
#include <cerrno>
#include <limits>
#endif
namespace asterion {
struct ProcessOwner::Impl {
  std::uint64_t pid;
#ifdef _WIN32
  HANDLE handle = nullptr;
  ~Impl() {
    if (handle)
      CloseHandle(handle);
  }
#endif
};
std::uint64_t current_process_id() noexcept {
#ifdef _WIN32
  return GetCurrentProcessId();
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}
bool process_running(std::uint64_t pid) {
#ifdef _WIN32
  if (!pid || pid > MAXDWORD)
    throw std::invalid_argument("invalid process identity");
  const auto handle = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
  if (!handle) {
    if (GetLastError() == ERROR_INVALID_PARAMETER)
      return false;
    throw std::runtime_error("cannot inspect process identity");
  }
  const auto result = WaitForSingleObject(handle, 0);
  CloseHandle(handle);
  if (result == WAIT_FAILED)
    throw std::runtime_error("cannot inspect process state");
  return result == WAIT_TIMEOUT;
#else
  if (!pid || pid > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max()))
    throw std::invalid_argument("invalid process identity");
  if (::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM)
    return true;
  if (errno == ESRCH)
    return false;
  throw std::runtime_error("cannot inspect process identity");
#endif
}
ProcessOwner::ProcessOwner(std::uint64_t pid) : impl_(std::make_unique<Impl>()) {
  if (!pid || pid == current_process_id())
    throw std::invalid_argument("invalid supervisor identity");
  impl_->pid = pid;
#ifdef _WIN32
  if (pid > MAXDWORD)
    throw std::invalid_argument("invalid supervisor identity");
  impl_->handle = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
  if (!impl_->handle)
    throw std::runtime_error("supervisor is not available");
#else
  if (static_cast<std::uint64_t>(::getppid()) != pid)
    throw std::runtime_error("supervisor is not the parent process");
#endif
}
ProcessOwner::~ProcessOwner() = default;
bool ProcessOwner::alive() const noexcept {
#ifdef _WIN32
  return WaitForSingleObject(impl_->handle, 0) == WAIT_TIMEOUT;
#else
  return static_cast<std::uint64_t>(::getppid()) == impl_->pid;
#endif
}
} // namespace asterion
