#include <asterion/kernel/process/child.hpp>
#include <asterion/foundation/error.hpp>
#include <array>
#include <random>
#include <thread>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#else
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
#endif
namespace asterion {
struct ChildProcess::Impl {
#ifdef _WIN32
  HANDLE handle = nullptr;
  ~Impl() {
    if (handle)
      CloseHandle(handle);
  }
#else
  pid_t pid = -1;
#endif
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
#ifdef _WIN32
  (void)independent;
  struct Redirect {
    HANDLE output = INVALID_HANDLE_VALUE, input = INVALID_HANDLE_VALUE,
           error = INVALID_HANDLE_VALUE;
    std::vector<unsigned char> storage;
    LPPROC_THREAD_ATTRIBUTE_LIST attributes = nullptr;
    ~Redirect() {
      if (attributes)
        DeleteProcThreadAttributeList(attributes);
      for (auto handle : {output, input, error})
        if (handle != INVALID_HANDLE_VALUE)
          CloseHandle(handle);
    }
  } redirect;
  auto quote = [](const std::wstring& value) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const auto c : value) {
      if (c == L'\\') {
        ++slashes;
        continue;
      }
      result.append(slashes * (c == L'\"' ? 2 : 1), L'\\');
      slashes = 0;
      if (c == L'\"')
        result += L'\\';
      result += c;
    }
    result.append(slashes * 2, L'\\');
    return result + L'\"';
  };
  std::wstring command = quote(executable.wstring());
  for (const auto& arg : arguments)
    command += L" " + quote(std::filesystem::path(std::u8string(arg.begin(), arg.end())).wstring());
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(STARTUPINFOW);
  PROCESS_INFORMATION info{};
  DWORD flags = CREATE_NO_WINDOW;
  if (!stdout_file.empty()) {
    if (!stdout_file.is_absolute())
      throw std::invalid_argument("capture path must be absolute");
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    redirect.output = CreateFileW(stdout_file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security,
                                  CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    redirect.input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 &security, OPEN_EXISTING, 0, nullptr);
    if (merge_stderr) {
      if (redirect.output == INVALID_HANDLE_VALUE ||
          !DuplicateHandle(GetCurrentProcess(), redirect.output, GetCurrentProcess(),
                           &redirect.error, 0, TRUE, DUPLICATE_SAME_ACCESS))
        redirect.error = INVALID_HANDLE_VALUE;
    } else
      redirect.error = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   &security, OPEN_EXISTING, 0, nullptr);
    if (redirect.output == INVALID_HANDLE_VALUE || redirect.input == INVALID_HANDLE_VALUE ||
        redirect.error == INVALID_HANDLE_VALUE)
      throw Error(ErrorCode::unavailable, "cannot prepare process output");
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    redirect.storage.resize(size);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(redirect.storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size))
      throw Error(ErrorCode::unavailable, "cannot prepare handle isolation");
    redirect.attributes = attributes;
    HANDLE handles[]{redirect.input, redirect.output, redirect.error};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
                                   sizeof(handles), nullptr, nullptr))
      throw Error(ErrorCode::unavailable, "cannot isolate process handles");
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = redirect.input;
    startup.StartupInfo.hStdOutput = redirect.output;
    startup.StartupInfo.hStdError = redirect.error;
    flags |= EXTENDED_STARTUPINFO_PRESENT;
  }
  if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, !stdout_file.empty(),
                      flags, nullptr, nullptr, &startup.StartupInfo, &info))
    throw Error(ErrorCode::unavailable, "cannot start process");
  CloseHandle(info.hThread);
  impl_->handle = info.hProcess;
#else
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
#endif
}
std::uint64_t ChildProcess::id() const noexcept {
#ifdef _WIN32
  return GetProcessId(impl_->handle);
#else
  return static_cast<std::uint64_t>(impl_->pid);
#endif
}
bool ChildProcess::exited() {
  if (impl_->finished)
    return true;
#ifdef _WIN32
  impl_->finished = WaitForSingleObject(impl_->handle, 0) == WAIT_OBJECT_0;
  if (impl_->finished) {
    DWORD code = 0;
    if (GetExitCodeProcess(impl_->handle, &code))
      impl_->code = static_cast<int>(code);
  }
#else
  int status = 0;
  const auto result = ::waitpid(impl_->pid, &status, WNOHANG);
  if (result == impl_->pid)
    impl_->code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  if (result == impl_->pid || (result < 0 && errno == ECHILD))
    impl_->finished = true;
#endif
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
#ifndef _WIN32
  const auto pid = impl_->pid;
  std::thread([pid] {
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
  }).detach();
#endif
  impl_->finished = true;
}
ChildProcess::~ChildProcess() {
  if (wait(std::chrono::milliseconds(500)))
    return;
#ifdef _WIN32
  TerminateProcess(impl_->handle, 1);
  WaitForSingleObject(impl_->handle, INFINITE);
#else
  ::kill(impl_->pid, SIGTERM);
  if (!wait(std::chrono::milliseconds(500))) {
    ::kill(impl_->pid, SIGKILL);
    int status = 0;
    while (::waitpid(impl_->pid, &status, 0) < 0 && errno == EINTR) {
    }
  }
#endif
}
std::filesystem::path current_executable() {
#ifdef _WIN32
  std::wstring result(32768, L'\0');
  const auto length = GetModuleFileNameW(nullptr, result.data(), static_cast<DWORD>(result.size()));
  if (!length || length == result.size())
    throw Error(ErrorCode::unavailable, "cannot locate application");
  result.resize(length);
  return result;
#elif defined(__APPLE__)
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
