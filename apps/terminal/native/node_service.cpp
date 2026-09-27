#include "node_service.hpp"
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/file_lock.hpp>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
// windows.h must precede sddl.h, which depends on its declarations.
#include <windows.h>
#include <sddl.h>
#else
#include <unistd.h>
#endif
namespace asterion::terminal {
namespace fs = std::filesystem;
namespace {
std::string utf8(const fs::path& p) {
  const auto s = p.u8string();
  return {s.begin(), s.end()};
}
bool command(const fs::path& binary, const std::vector<std::string>& args) {
  ChildProcess child(binary, args, true);
  if (!child.wait(std::chrono::seconds(15)))
    throw std::runtime_error("service manager timed out");
  return child.exit_code() == 0;
}
void require_command(const fs::path& binary, const std::vector<std::string>& args) {
  if (!command(binary, args))
    throw std::runtime_error("OS service manager rejected the operation or identity verification");
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
  write_file_durably(file, text, false);
}
} // namespace
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
#ifdef __APPLE__
  const auto* home = std::getenv("HOME");
  if (!home)
    throw std::runtime_error("HOME is unavailable");
  const auto directory = fs::path(home) / "Library/LaunchAgents";
  if (!stopping)
    fs::create_directories(directory);
  if (name.empty())
    name = "me.asterion.node-agent";
  const auto file = directory / (name + ".plist");
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
#elif defined(_WIN32)
  // A per-user scheduled task preserves the same identity and private-pipe ACL.
  // It needs no stored password and survives Terminal exit (not user logout).
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    throw std::runtime_error("cannot read service identity");
  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<unsigned char> buffer(size);
  const bool read = GetTokenInformation(token, TokenUser, buffer.data(), size, &size) != FALSE;
  CloseHandle(token);
  LPWSTR sid = nullptr;
  if (!read ||
      !ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid))
    throw std::runtime_error("cannot read service identity");
  const auto sid_path = fs::path(sid);
  LocalFree(sid);
  const auto user = utf8(sid_path);
  const auto arguments = "--directory &quot;" + xml(utf8(root)) + "&quot; --endpoint &quot;" +
                         xml(endpoint) + "&quot;";
  const auto definition = root / "scheduled-task.xml";
  write(definition,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?><Task version=\"1.2\" "
        "xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/"
        "task\"><Triggers><LogonTrigger><Enabled>true</Enabled><UserId>" +
            user +
            "</UserId></LogonTrigger></Triggers><Principals><Principal "
            "id=\"Author\"><UserId>" +
            user +
            "</UserId><LogonType>InteractiveToken</"
            "LogonType><RunLevel>LeastPrivilege</RunLevel></Principal></"
            "Principals><Settings><MultipleInstancesPolicy>IgnoreNew</"
            "MultipleInstancesPolicy><DisallowStartIfOnBatteries>false</"
            "DisallowStartIfOnBatteries><StopIfGoingOnBatteries>false</"
            "StopIfGoingOnBatteries><ExecutionTimeLimit>PT0S</"
            "ExecutionTimeLimit><RestartOnFailure><Interval>PT1M</"
            "Interval><Count>3</Count></RestartOnFailure></Settings><Actions "
            "Context=\"Author\"><Exec><Command>" +
            xml(utf8(executable)) + "</Command><Arguments>" + arguments +
            "</Arguments></Exec></Actions></Task>",
        stopping);
  wchar_t system[MAX_PATH];
  const auto length = GetSystemDirectoryW(system, MAX_PATH);
  if (!length || length >= MAX_PATH)
    throw std::runtime_error("cannot locate task scheduler");
  const auto scheduler = fs::path(system) / "schtasks.exe";
  if (name.empty())
    name = "AsterionNodeAgent";
  if (stopping) {
    auto literal = [](const std::string& value) {
      std::string s = "'";
      for (char c : value) {
        s += c;
        if (c == '\'')
          s += '\'';
      }
      return s + "'";
    };
    const auto expected_args = "--directory \"" + utf8(root) + "\" --endpoint \"" + endpoint + "\"";
    const auto check =
        verifying ? "$ErrorActionPreference='Stop';$s=New-Object -ComObject "
                    "Schedule.Service;$s.Connect();$t=$s.GetFolder('\\').GetTask(" +
                        literal(name) +
                        ");$a=$t.Definition.Actions;if($t.Enabled -or "
                        "$t.GetInstances(0).Count -ne 0 -or $a.Count -ne 1 -or "
                        "$a.Item(1).Path -ne " +
                        literal(utf8(executable)) + " -or $a.Item(1).Arguments -cne " +
                        literal(expected_args) + "){exit 1}"
                  : "$ErrorActionPreference='Stop';$t=Get-ScheduledTask -TaskPath "
                    "'\\' "
                    "-TaskName " +
                        literal(name) + ";$p=Get-Process -Id " + std::to_string(expected_pid) +
                        ";if(@($t.Actions).Count -ne 1 -or $t.Actions[0].Execute "
                        "-ne " +
                        literal(utf8(executable)) + " -or $t.Actions[0].Arguments -cne " +
                        literal(expected_args) + " -or $p.Path -ne " + literal(utf8(executable)) +
                        "){exit 1}";
    require_command(fs::path(system) / "WindowsPowerShell/v1.0/powershell.exe",
                    {"-NoProfile", "-NonInteractive", "-Command", check});
    if (!verifying) {
      require_command(scheduler, {"/Change", "/TN", name, "/DISABLE"});
      require_command(scheduler, {"/End", "/TN", name});
    }
  } else {
    require_command(scheduler, {"/Create", "/TN", name, "/XML", utf8(definition), "/F"});
    require_command(scheduler, {"/Run", "/TN", name});
  }

#else
  const auto* home = std::getenv("HOME");
  if (!home)
    throw std::runtime_error("HOME is unavailable");
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
  const auto folder = fs::path(home) / ".config/systemd/user";
  if (!stopping)
    fs::create_directories(folder);
  if (name.empty())
    name = "asterion-node-agent";
  const auto file = folder / (name + ".service");
  write(file,
        "[Unit]\nDescription=Asterion Node "
        "Agent\nStartLimitIntervalSec=60\nStartLimitBurst=3\n[Service]"
        "\nExecStart=" +
            quote(utf8(executable)) + " --directory " + quote(utf8(root)) + " --endpoint " +
            quote(endpoint) +
            "\nRestart=on-failure\nRestartSec=5\n[Install]\nWantedBy=default."
            "target\n",
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
} // namespace asterion::terminal
