#include <asterion/kernel/process/child.hpp>
#include <asterion/foundation/error.hpp>
#include <array>
#include <random>
#include <thread>
#include <stdexcept>
#include <cerrno>
#include <fcntl.h>
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
extern char** environ;
namespace asterion {
struct ChildProcess::Impl {
  pid_t pid = -1;
  bool finished = false;
  int code = -1;
};
ChildProcess::ChildProcess(const std::filesystem::path& executable,
                           const std::vector<std::string>& arguments, bool independent,
                           const std::filesystem::path& stdout_file, bool merge_stderr)
    : impl_(std::make_unique<Impl>()) {
  if (!executable.is_absolute() || !std::filesystem::is_regular_file(executable))
    throw Error(ErrorCode::unavailable, "trading executable is missing");
  for (const auto& argument : arguments)
    if (argument.find('\0') != std::string::npos)
      throw std::invalid_argument("invalid process argument");
  std::vector<std::string> storage{executable.string()};
  storage.insert(storage.end(), arguments.begin(), arguments.end());
  std::vector<char*> argv;
  for (auto& arg : storage)
    argv.push_back(arg.data());
  argv.push_back(nullptr);
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawnattr_t attrs;
  posix_spawnattr_init(&attrs);
  // Never inherit the desktop host's IPC/debug pipes or unrelated sockets.
  // Explicit file actions below still supply the child's standard streams.
#ifdef __APPLE__
  short flags = POSIX_SPAWN_CLOEXEC_DEFAULT;
#else
  short flags = 0;
  if (posix_spawn_file_actions_addclosefrom_np(&actions, 3)) {
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attrs);
    throw Error(ErrorCode::unavailable, "cannot isolate child process descriptors");
  }
#endif
  if (independent) {
    flags |= POSIX_SPAWN_SETPGROUP;
    posix_spawnattr_setpgroup(&attrs, 0);
    for (int descriptor = 0; descriptor < 3; ++descriptor)
      posix_spawn_file_actions_addopen(&actions, descriptor, "/dev/null",
                                       descriptor == 0 ? O_RDONLY : O_WRONLY, 0);
  }
  if (!stdout_file.empty()) {
    if (!stdout_file.is_absolute()) {
      posix_spawn_file_actions_destroy(&actions);
      posix_spawnattr_destroy(&attrs);
      throw std::invalid_argument("capture path must be absolute");
    }
    int result = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, stdout_file.c_str(),
                                                  O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (!result && merge_stderr)
      result = posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    if (result) {
      posix_spawn_file_actions_destroy(&actions);
      posix_spawnattr_destroy(&attrs);
      throw Error(ErrorCode::unavailable, "cannot prepare process output");
    }
  }
  if (posix_spawnattr_setflags(&attrs, flags)) {
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attrs);
    throw Error(ErrorCode::unavailable, "cannot isolate child process descriptors");
  }
  const int error =
      ::posix_spawn(&impl_->pid, executable.c_str(), &actions, &attrs, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attrs);
  if (error) {
    impl_->pid = -1;
    throw Error(ErrorCode::unavailable, "cannot start trading process");
  }
}
std::uint64_t ChildProcess::id() const noexcept {
  return static_cast<std::uint64_t>(impl_->pid);
}
bool ChildProcess::exited() {
  if (impl_->finished)
    return true;
  int status = 0;
  const auto result = ::waitpid(impl_->pid, &status, WNOHANG);
  if (result == impl_->pid)
    impl_->code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  if (result == impl_->pid || (result < 0 && errno == ECHILD))
    impl_->finished = true;
  return impl_->finished;
}
int ChildProcess::exit_code() {
  if (!exited())
    throw std::logic_error("process has not exited");
  return impl_->code;
}
bool ChildProcess::wait(std::chrono::milliseconds timeout) {
  const auto end = std::chrono::steady_clock::now() + timeout;
  while (!exited()) {
    if (std::chrono::steady_clock::now() >= end)
      return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return true;
}
void ChildProcess::release() {
  const auto pid = impl_->pid;
  std::thread([pid] {
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
  }).detach();
  impl_->finished = true;
}
void ChildProcess::request_stop() noexcept {
  if (!impl_->finished && impl_->pid > 0)
    ::kill(impl_->pid, SIGTERM);
}
ChildProcess::~ChildProcess() {
  if (wait(std::chrono::milliseconds(500)))
    return;
  ::kill(impl_->pid, SIGTERM);
  if (!wait(std::chrono::milliseconds(500))) {
    ::kill(impl_->pid, SIGKILL);
    int status = 0;
    while (::waitpid(impl_->pid, &status, 0) < 0 && errno == EINTR) {
    }
  }
}
std::filesystem::path current_executable() {
#if defined(__APPLE__)
  std::uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string result(size, '\0');
  if (_NSGetExecutablePath(result.data(), &size) != 0)
    throw Error(ErrorCode::unavailable, "cannot locate application");
  return std::filesystem::canonical(result.c_str());
#else
  return std::filesystem::read_symlink("/proc/self/exe");
#endif
}
std::string unique_process_id() {
  std::random_device random;
  const char* digits = "0123456789abcdef";
  std::string result;
  for (int i = 0; i < 16; ++i) {
    const auto byte = random() & 255;
    result += digits[byte >> 4];
    result += digits[byte & 15];
  }
  return result;
}
} // namespace asterion
