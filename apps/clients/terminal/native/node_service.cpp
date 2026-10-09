#include "node_service.hpp"
#include <asterion/kernel/environment.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/file_lock.hpp>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <unistd.h>
namespace asterion::terminal {
namespace fs = std::filesystem;
namespace {
std::string utf8(const fs::path& p) {
  const auto s = p.u8string();
  return {s.begin(), s.end()};
}
// Used by the launchd path; systemd uses require_command only.
[[maybe_unused]] bool command(const fs::path& binary, const std::vector<std::string>& args) {
  ChildProcess child(binary, args, true);
  if (!child.wait(std::chrono::seconds(15)))
    throw std::runtime_error("service manager timed out");
  return child.exit_code() == 0;
}
// Runs a service-manager command; on failure the error names the program, its
// first argument, the exit code and the start of its combined output.
void require_command(const fs::path& binary, const std::vector<std::string>& args) {
  const auto capture = fs::temp_directory_path() / ("asterion-service-" + unique_process_id());
  struct Remove {
    fs::path file;
    ~Remove() {
      std::error_code ignored;
      fs::remove(file, ignored);
    }
  } cleanup{capture};
  ChildProcess child(binary, args, true, capture, true);
  if (!child.wait(std::chrono::seconds(15)))
    throw std::runtime_error("service manager timed out: " + utf8(binary.filename()));
  if (child.exit_code() == 0)
    return;
  std::ifstream in(capture, std::ios::binary);
  std::string output(512, '\0');
  in.read(output.data(), static_cast<std::streamsize>(output.size()));
  output.resize(static_cast<std::size_t>(in.gcount()));
  throw std::runtime_error("OS service manager rejected the operation or identity verification (" +
                           utf8(binary.filename()) + " " + (args.empty() ? "" : args.front()) +
                           ", exit " + std::to_string(child.exit_code()) + "): " + output);
}
void write(const fs::path& file, const std::string& text, bool require_existing) {
  if (fs::is_symlink(file))
    throw std::invalid_argument("service configuration cannot be a symlink");
  if (fs::exists(file)) {
    std::ifstream in(file);
    std::string old{std::istreambuf_iterator<char>(in), {}};
    if (old != text)
      throw std::runtime_error("existing service configuration differs; "
                               "explicit service upgrade required");
    return;
  }
  if (require_existing)
    throw std::runtime_error("owned service definition is missing");
  // Service definitions are read by the OS service manager, not only this user.
  replace_file_durably(file, text, false);
}
#ifdef __APPLE__
constexpr auto definitions = "Library/LaunchAgents";
constexpr auto extension = ".plist";
#else
constexpr auto definitions = ".config/systemd/user";
constexpr auto extension = ".service";
#endif
} // namespace
fs::path node_service_definition(const std::string& name) {
  const auto home = environment_path("HOME");
  if (!home)
    throw std::runtime_error("HOME is unavailable");
  return *home / definitions / ((name.empty() ? "me.asterion.node-agent" : name) + extension);
}
void manage_node_service(const fs::path& executable, const fs::path& root,
                         const std::string& endpoint, bool stopping, std::uint64_t expected_pid,
                         std::string name, bool verifying = false) {
  const auto invalid_text = [](const std::string& value) {
    return value.find_first_of("\r\n") != std::string::npos ||
           value.find('\0') != std::string::npos;
  };
  if (invalid_text(utf8(executable)) || invalid_text(utf8(root)) || invalid_text(endpoint))
    throw std::invalid_argument("invalid managed service arguments");
  if (!executable.is_absolute() || !root.is_absolute() || fs::is_symlink(executable) ||
      fs::is_symlink(root) || endpoint.empty() ||
      endpoint.find_first_of("\r\n") != std::string::npos)
    throw std::invalid_argument("invalid managed service identity");
  if (!name.empty() && (name.size() > 120 ||
                        name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUV"
                                               "WXYZ0123456789.-_") != std::string::npos ||
                        name.front() == '-'))
    throw std::invalid_argument("invalid service name");
  if (stopping && !verifying && !expected_pid)
    throw std::invalid_argument("Agent PID is required");

  [[maybe_unused]] auto xml = [](const std::string& value) {
    std::string result;
    for (const auto c : value) {
      if (c == '&')
        result += "&amp;";
      else if (c == '<')
        result += "&lt;";
      else if (c == '>')
        result += "&gt;";
      else
        result += c;
    }
    return result;
  };
  if (name.empty())
    name = "me.asterion.node-agent";
  const auto file = node_service_definition(name);
  if (!stopping)
    create_directories_durably(file.parent_path());
#ifdef __APPLE__
  std::string plist = "<?xml version=\"1.0\" encoding=\"UTF-8\"?><plist "
                      "version=\"1.0\"><dict><key>Label</key><string>" +
                      name + "</string><key>ProgramArguments</key><array>";
  for (const auto& arg : std::vector<std::string>{utf8(executable), "--directory", utf8(root),
                                                  "--endpoint", endpoint})
    plist += "<string>" + xml(arg) + "</string>";
  plist += "</array><key>RunAtLoad</key><true/><key>KeepAlive</key><true/"
           "><key>ThrottleInterval</key><integer>10</integer></dict></plist>";
  write(file, plist, stopping);
  const auto domain = "gui/" + std::to_string(::getuid());
  if (verifying) {
    // launchctl uses ESRCH (113) for an absent service in an existing domain.
    // First prove the user's domain itself is accessible.
    require_command("/bin/launchctl", {"print", domain});
    require_command("/bin/sh",
                    {"-c", "/bin/launchctl print \"$1\" >/dev/null 2>&1; test \"$?\" -eq 113",
                     "asterion-stopped-check", domain + "/" + name});
  } else if (stopping) {
    const std::string check =
        R"CHECK(test "$(/bin/launchctl print "$1" | /usr/bin/awk '/^[[:space:]]*path = / {sub(/^[[:space:]]*path = /, ""); print; exit}')" = "$2" && test "$(/bin/launchctl print "$1" | /usr/bin/awk '$1 == "pid" && $2 == "=" {print $3; exit}')" = "$3" && test "$(/bin/launchctl print "$1" | /usr/bin/awk '/^[[:space:]]*program = / {sub(/^[[:space:]]*program = /, ""); print; exit}')" = "$4")CHECK";
    require_command("/bin/sh", {"-c", check, "asterion-service-check", domain + "/" + name,
                                utf8(file), std::to_string(expected_pid), utf8(executable)});
    require_command("/bin/launchctl", {"bootout", domain + "/" + name});
  } else {
    if (!command("/bin/launchctl", {"print", domain + "/" + name}))
      require_command("/bin/launchctl", {"bootstrap", domain, utf8(file)});
    require_command("/bin/launchctl", {"kickstart", domain + "/" + name});
  }
#else
  auto quote = [](const std::string& value) {
    std::string out = "\"";
    for (const auto c : value) {
      if (c == '\n' || c == '\r')
        throw std::invalid_argument("invalid unit path");
      if (c == '\\' || c == '"')
        out += '\\';
      if (c == '%')
        out += '%';
      out += c;
    }
    return out + "\"";
  };
  // systemd counts the Terminal's own starts against a start limit and would
  // refuse the next one, so there is none: a failing Agent is retried every
  // ten seconds, as under launchd. The socket directory is under /tmp, which a
  // reboot empties: the Agent started at login needs it before a Terminal
  // opens to create it.
  write(file,
        "[Unit]\nDescription=Asterion Node Agent\nStartLimitIntervalSec=0\n[Service]"
        "\nExecStartPre=/usr/bin/mkdir -p -m 0700 " +
            quote(utf8(fs::path(endpoint).parent_path())) +
            "\nExecStart=" + quote(utf8(executable)) + " --directory " + quote(utf8(root)) +
            " --endpoint " + quote(endpoint) +
            "\nRestart=on-failure\nRestartSec=10\n[Install]\nWantedBy=default.target\n",
        stopping);
  if (verifying) {
    const std::string check =
        R"CHECK(test "$(/usr/bin/systemctl --user show --property=FragmentPath --value "$1")" = "$2" && test "$(/usr/bin/systemctl --user show --property=MainPID --value "$1")" = 0 && test "$(/usr/bin/systemctl --user show --property=ActiveState --value "$1")" = inactive)CHECK";
    require_command("/bin/sh",
                    {"-c", check, "asterion-stopped-check", name + ".service", utf8(file)});
  } else if (stopping) {
    const std::string check =
        R"CHECK(test "$(/usr/bin/systemctl --user show --property=FragmentPath --value "$1")" = "$2" && test "$(/usr/bin/systemctl --user show --property=MainPID --value "$1")" = "$3" && test "$(/usr/bin/readlink -f "/proc/$3/exe")" = "$4")CHECK";
    require_command("/bin/sh",
                    {"-c", check, "asterion-service-check", name + ".service", utf8(file),
                     std::to_string(expected_pid), utf8(fs::canonical(executable))});
    require_command("/usr/bin/systemctl", {"--user", "stop", name + ".service"});
  } else {
    require_command("/usr/bin/systemctl", {"--user", "daemon-reload"});
    require_command("/usr/bin/systemctl", {"--user", "enable", "--now", name + ".service"});
  }
#endif
  if (stopping) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
      try {
        FileLock stopped(root, "agent.lock");
        break;
      } catch (const std::runtime_error&) {
        if (std::chrono::steady_clock::now() >= deadline)
          throw std::runtime_error("Agent directory remains owned or unavailable; program "
                                   "replacement is forbidden");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    }
  }
}
void install_node_service(const fs::path& executable, const fs::path& root,
                          const std::string& endpoint, const std::string& name) {
  manage_node_service(executable, root, endpoint, false, 0, name);
}
void stop_node_service(const fs::path& executable, const fs::path& root,
                       const std::string& endpoint, std::uint64_t expected_pid,
                       const std::string& name) {
  manage_node_service(executable, root, endpoint, true, expected_pid, name);
}
void verify_node_service_stopped(const fs::path& executable, const fs::path& root,
                                 const std::string& endpoint, const std::string& name) {
  manage_node_service(executable, root, endpoint, true, 0, name, true);
}
void remove_node_service(const std::string& name) {
  const auto file = node_service_definition(name);
#ifndef __APPLE__
  // The login registration is a link systemd keeps beside the definition.
  require_command("/usr/bin/systemctl", {"--user", "disable", utf8(file.filename())});
#endif
  fs::remove(file);
  sync_directory(file.parent_path());
}
} // namespace asterion::terminal
