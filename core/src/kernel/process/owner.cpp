#include <asterion/kernel/process/owner.hpp>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
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
