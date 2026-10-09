#include "data_credentials.hpp"
#include <asterion/foundation/error.hpp>
#include <asterion/kernel/environment.hpp>
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
namespace asterion::terminal {
namespace {
struct Descriptor {
  int value = -1;
  Descriptor() = default;
  Descriptor(const Descriptor&) = delete;
  Descriptor& operator=(const Descriptor&) = delete;
  ~Descriptor() { reset(); }
  void reset(int next = -1) noexcept {
    if (value >= 0)
      ::close(value);
    value = next;
  }
};
[[noreturn]] void unavailable() {
  throw Error(ErrorCode::unavailable, "credential store unavailable");
}
struct Pipe {
  Descriptor read, write;
  // The helper's input must fail with EPIPE, never SIGPIPE, when the helper
  // exits early. Linux can only ask for that per send(), which takes a socket.
  explicit Pipe(bool input) {
    int descriptors[2];
#ifdef __APPLE__
    const int created = ::pipe(descriptors);
#else
    const int created =
        input ? ::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) : ::pipe(descriptors);
#endif
    if (created != 0)
      unavailable();
    read.value = descriptors[0];
    write.value = descriptors[1];
    for (auto* descriptor : {&read, &write}) {
      // Child actions must not close a newly duplicated stdin/stdout when the
      // parent happened to start with one of its standard descriptors closed.
      if (descriptor->value < 3) {
        const int duplicate = ::fcntl(descriptor->value, F_DUPFD_CLOEXEC, 3);
        if (duplicate < 0)
          unavailable();
        descriptor->reset(duplicate);
      } else if (::fcntl(descriptor->value, F_SETFD, FD_CLOEXEC) < 0)
        unavailable();
    }
#ifdef __APPLE__
    if (input && ::fcntl(write.value, F_SETNOSIGPIPE, 1) < 0)
      unavailable();
#endif
  }
};
ssize_t send_input(int fd, const char* data, std::size_t size) {
#ifdef __APPLE__
  return ::write(fd, data, size);
#else
  return ::send(fd, data, size, MSG_NOSIGNAL);
#endif
}
void nonblocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL);
  if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    unavailable();
}
struct Actions {
  posix_spawn_file_actions_t value;
  Actions() {
    if (posix_spawn_file_actions_init(&value) != 0)
      unavailable();
  }
  ~Actions() { posix_spawn_file_actions_destroy(&value); }
};
struct Attributes {
  posix_spawnattr_t value;
  Attributes() {
    if (posix_spawnattr_init(&value) != 0)
      unavailable();
  }
  ~Attributes() { posix_spawnattr_destroy(&value); }
};
struct Process {
  pid_t pid = 0;
  int status = 0;
  ~Process() {
    if (pid > 0) {
      // This is the helper we spawned, never a user service or a Keychain daemon.
      ::kill(pid, SIGKILL);
      while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      }
    }
  }
  bool exited() {
    if (!pid)
      return true;
    const auto result = ::waitpid(pid, &status, WNOHANG);
    if (result == pid) {
      pid = 0;
      return true;
    }
    if (result < 0 && errno != EINTR) {
      if (errno == ECHILD)
        pid = 0; // Ownership ended; never signal a potentially reused process ID.
      unavailable();
    }
    return false;
  }
};
struct Output {
  int status = 1;
  std::string text;
};
// One deadline covers stdin, stdout and child completion. No credential ever
// enters argv, environment, a temporary file or a diagnostic message.
Output run(const std::filesystem::path& helper, const std::vector<std::string>& arguments,
           const std::string& input, std::chrono::milliseconds timeout) {
  if (input.size() > 4096 || input.find('\0') != std::string::npos)
    throw Error(ErrorCode::invalid_request, "invalid keychain credential length or content");
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  Pipe in(true), out(false);
  nonblocking(in.write.value);
  nonblocking(out.read.value);
  Actions actions;
  if (posix_spawn_file_actions_adddup2(&actions.value, in.read.value, STDIN_FILENO) != 0 ||
      posix_spawn_file_actions_adddup2(&actions.value, out.write.value, STDOUT_FILENO) != 0 ||
      posix_spawn_file_actions_addopen(&actions.value, STDERR_FILENO, "/dev/null", O_WRONLY, 0) !=
          0)
    unavailable();
  for (const auto fd : {in.read.value, in.write.value, out.read.value, out.write.value})
    if (posix_spawn_file_actions_addclose(&actions.value, fd) != 0)
      unavailable();
  // Nothing else this process has open reaches the helper.
  Attributes attributes;
#ifdef __APPLE__
  if (posix_spawnattr_setflags(&attributes.value, POSIX_SPAWN_CLOEXEC_DEFAULT) != 0)
    unavailable();
#else
  if (posix_spawn_file_actions_addclosefrom_np(&actions.value, 3) != 0)
    unavailable();
#endif
  std::vector<std::string> owned{helper.string()};
  owned.insert(owned.end(), arguments.begin(), arguments.end());
  std::vector<char*> argv;
  for (auto& item : owned)
    argv.push_back(item.data());
  argv.push_back(nullptr);
  // The helper inherits no environment. On Linux it is told only where the
  // user's session bus is, which is how the Secret Service is reached.
  std::vector<std::string> variables;
#ifndef __APPLE__
  for (const auto* name : {"DBUS_SESSION_BUS_ADDRESS", "XDG_RUNTIME_DIR"})
    if (const auto value = environment_variable(name))
      variables.push_back(std::string(name) + "=" + *value);
#endif
  std::vector<char*> environment;
  for (auto& item : variables)
    environment.push_back(item.data());
  environment.push_back(nullptr);
  Process process;
  if (posix_spawn(&process.pid, helper.c_str(), &actions.value, &attributes.value, argv.data(),
                  environment.data()) != 0) {
    process.pid = 0;
    unavailable();
  }
  in.read.reset();
  out.write.reset();
  Output result;
  std::size_t sent = 0;
  if (input.empty())
    in.write.reset();
  for (;;) {
    const bool exited = process.exited();
    if (exited && out.read.value < 0) {
      if (sent != input.size())
        unavailable();
      result.status = WIFEXITED(process.status) ? WEXITSTATUS(process.status) : 1;
      return result;
    }
    const auto remaining = deadline - std::chrono::steady_clock::now();
    if (remaining <= std::chrono::steady_clock::duration::zero())
      throw Error(ErrorCode::unavailable, "keychain helper timed out");
    pollfd descriptors[] = {{in.write.value, POLLOUT, 0}, {out.read.value, POLLIN, 0}};
    const auto wait = std::min(std::chrono::ceil<std::chrono::milliseconds>(remaining),
                               std::chrono::milliseconds(20));
    const int ready = ::poll(descriptors, 2, static_cast<int>(wait.count()));
    if (ready < 0) {
      if (errno == EINTR)
        continue;
      unavailable();
    }
    if (in.write.value >= 0 && descriptors[0].revents) {
      const auto count = send_input(in.write.value, input.data() + sent, input.size() - sent);
      if (count > 0) {
        sent += static_cast<std::size_t>(count);
        if (sent == input.size())
          in.write.reset();
      } else if (count == 0 || (errno != EINTR && errno != EAGAIN))
        unavailable();
    }
    if (out.read.value >= 0 && descriptors[1].revents) {
      char buffer[1024];
      for (;;) {
        const auto count = ::read(out.read.value, buffer, sizeof buffer);
        if (count == 0) {
          out.read.reset();
          break;
        }
        if (count < 0) {
          if (errno == EINTR)
            continue;
          if (errno == EAGAIN)
            break;
          unavailable();
        }
        if (result.text.size() + static_cast<std::size_t>(count) > 4096)
          throw Error(ErrorCode::resource_exhausted,
                      "keychain helper response exceeds the credential limit");
        result.text.append(buffer, static_cast<std::size_t>(count));
      }
    }
  }
}
class Keychain final : public CredentialStore {
public:
  Keychain(std::filesystem::path helper, std::chrono::milliseconds timeout)
      : helper_(std::move(helper)), timeout_(timeout) {}
  std::optional<std::string> load(const std::string& account) override {
    const auto output = run(helper_, {"get", account}, {}, timeout_);
    if (output.status == 3)
      return std::nullopt;
    if (output.status != 0)
      throw Error(ErrorCode::unavailable, "keychain credential is unavailable");
    return output.text;
  }
  void store(const std::string& account, const std::string& secret) override {
    if (run(helper_, {"set", account}, secret, timeout_).status != 0)
      throw Error(ErrorCode::unavailable, "cannot save credential to the keychain");
  }
  void erase(const std::string& account) override {
    if (run(helper_, {"delete", account}, {}, timeout_).status != 0)
      throw Error(ErrorCode::unavailable, "cannot remove credential from the keychain");
  }

private:
  std::filesystem::path helper_;
  std::chrono::milliseconds timeout_;
};
} // namespace
std::shared_ptr<CredentialStore> keychain_store(const std::filesystem::path& helper,
                                                std::chrono::milliseconds timeout) {
  if (timeout <= std::chrono::milliseconds::zero() || timeout > std::chrono::seconds(30))
    throw Error(ErrorCode::invalid_request, "invalid keychain helper deadline");
  if (!helper.is_absolute() || !std::filesystem::is_regular_file(helper) ||
      std::filesystem::is_symlink(helper))
    return nullptr;
#ifndef __APPLE__
  // A Linux session may have no Secret Service. Looking up an account that is
  // never stored neither prompts nor changes the keyring; a short bound keeps
  // an unresponsive session bus from holding the Terminal's start.
  try {
    (void)Keychain(helper, std::min(timeout, std::chrono::milliseconds(3000))).load("availability");
  } catch (const Error&) {
    return nullptr;
  }
#endif
  return std::make_shared<Keychain>(helper, timeout);
}
} // namespace asterion::terminal
