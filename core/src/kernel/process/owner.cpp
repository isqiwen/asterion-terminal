#include <asterion/kernel/process/owner.hpp>
#include <stdexcept>
#include <unistd.h>
#include <csignal>
#include <cerrno>
#include <limits>
namespace asterion {
struct ProcessOwner::Impl {
  std::uint64_t pid;
};
std::uint64_t current_process_id() noexcept {
  return static_cast<std::uint64_t>(::getpid());
}
bool process_running(std::uint64_t pid) {
  if (!pid || pid > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max()))
    throw std::invalid_argument("invalid process identity");
  if (::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM)
    return true;
  if (errno == ESRCH)
    return false;
  throw std::runtime_error("cannot inspect process identity");
}
ProcessOwner::ProcessOwner(std::uint64_t pid) : impl_(std::make_unique<Impl>()) {
  if (!pid || pid == current_process_id())
    throw std::invalid_argument("invalid supervisor identity");
  impl_->pid = pid;
  if (static_cast<std::uint64_t>(::getppid()) != pid)
    throw std::runtime_error("supervisor is not the parent process");
}
ProcessOwner::~ProcessOwner() = default;
bool ProcessOwner::alive() const noexcept {
  return static_cast<std::uint64_t>(::getppid()) == impl_->pid;
}
} // namespace asterion
