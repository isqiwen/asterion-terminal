#include "node_client.hpp"
#include "application_environment.hpp"
#include "node_program.hpp"
#include "node_service.hpp"
#include <asterion/kernel/environment.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <asterion/foundation/error.hpp>
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/file_lock.hpp>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <algorithm>
#include <thread>
#include <stdexcept>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#else
#include <windows.h>
#endif
namespace asterion::terminal {
namespace fs = std::filesystem;
using namespace std::chrono_literals;
namespace {
std::unique_ptr<FileLock> development_owner;
void own_development(const fs::path& root) {
  if (development_owner)
    return;
  try {
    development_owner = std::make_unique<FileLock>(root, "development.lock");
  } catch (const std::runtime_error&) {
    throw Error(ErrorCode::conflict,
                "development environment is already open in another entry point");
  }
}
fs::path environment(const char* name) {
  return environment_path(name).value_or(fs::path{});
}
std::string utf8(const fs::path& p) {
  const auto s = p.u8string();
  return {s.begin(), s.end()};
}
fs::path local_root() {
  [[maybe_unused]] const bool development = development_environment();
  auto root = environment("ASTERION_NODE_DIRECTORY");
  if (root.empty()) {
#ifdef _WIN32
    root = environment("LOCALAPPDATA") / "Asterion" / "node";
#elif defined(__APPLE__)
    root = environment("HOME") / "Library" / "Application Support" /
           (development ? "Asterion Development" : "Asterion") / "node";
#else
    root = environment("XDG_DATA_HOME");
    if (root.empty())
      root = environment("HOME") / ".local" / "share";
    root /= "asterion/node";
#endif
  }
  return root;
}
fs::path bundled_agent() {
  auto executable = environment("ASTERION_NODE_AGENT_EXECUTABLE");
  if (executable.empty())
    executable =
        current_executable().parent_path() /
        (current_platform().os == "windows" ? "asterion-node-agent.exe" : "asterion-node-agent");
  return executable;
}

} // namespace
std::filesystem::path keychain_helper() {
  auto executable = environment("ASTERION_KEYCHAIN_EXECUTABLE");
  if (executable.empty())
    executable = current_executable().parent_path() / "asterion-keychain";
  return executable;
}
std::filesystem::path local_node_directory() {
  return local_root();
}
std::filesystem::path node_enrollment_directory() {
  if (development_environment() || environment_path("ASTERION_NODE_DIRECTORY"))
    return local_root() / "enrollments";
#ifdef _WIN32
  const auto home = environment_path("LOCALAPPDATA");
#else
  const auto home = environment_path("HOME");
#endif
  if (!home)
    throw std::runtime_error("local user data directory unavailable");
  return *home / ".asterion" / "nodes";
}
std::filesystem::path new_account_directory(const std::string& name) {
  if (name.empty() || name.size() > 120 || name == "." || name == ".." || name.front() == '.' ||
      name.find_first_of("/\\:\r\n") != std::string::npos || name.find('\0') != std::string::npos)
    throw std::invalid_argument("invalid managed account name");
  const auto root = local_root() / "accounts";
  const auto group = root / "ctp";
  for (const auto& directory : {root, group}) {
    if (fs::is_symlink(directory))
      throw std::invalid_argument("invalid managed account directory");
    fs::create_directories(directory);
    fs::permissions(directory, fs::perms::owner_all, fs::perm_options::replace);
  }
  const auto directory = group / fs::path(std::u8string(name.begin(), name.end()));
  if (!fs::create_directory(directory))
    throw Error(ErrorCode::conflict,
                "managed account name already exists; open the existing account");
  fs::permissions(directory, fs::perms::owner_all, fs::perm_options::replace);
  return directory;
}
Json local_node_program_status() {
  if (environment_variable("ASTERION_NODE_DIRECTORY"))
    return Json{{"state", "isolated"},
                {"expected_digest", ""},
                {"installed_digest", ""},
                {"bundled_digest", ""}};
  const auto root = local_root(), source = bundled_agent();
  return inspect_node_program(source, root / "bin" / source.filename(), root);
}
NodeEndpoint upgrade_local_node(const std::string& expected) {
  if (environment_variable("ASTERION_NODE_DIRECTORY"))
    throw std::runtime_error("system Agent upgrade is unavailable in isolated development");
  const auto root = local_root(), source = bundled_agent();
  if (!root.is_absolute() || fs::is_symlink(root))
    throw std::invalid_argument("invalid local Agent directory");
  const auto file = root / "ipc-id";
  if (fs::is_symlink(file) || !fs::is_regular_file(file) || fs::file_size(file) > 64)
    throw std::runtime_error("invalid Agent identity file");
  std::ifstream input(file);
  std::string identity;
  input >> identity;
  input >> std::ws;
  if (!input.eof() || identity.size() != 32 ||
      identity.find_first_not_of("0123456789abcdef") != std::string::npos)
    throw std::runtime_error("invalid Agent identity");
#ifdef _WIN32
  const auto endpoint = "asterion.node." + identity;
#else
  const auto endpoint = (fs::path("/tmp") / ("ast-node-" + identity) / "node.sock").string();
#endif
  upgrade_node_service(source, root / "bin" / source.filename(), root, endpoint, expected,
                       local_node_service_name());
  return NodeEndpoint{"local", "localhost", 0, {}, endpoint};
}
void shutdown_development_node(bool recover) {
  if (!development_environment() || environment_variable("ASTERION_NODE_DIRECTORY"))
    throw Error(ErrorCode::permission_denied,
                "automatic shutdown requires the managed development environment");
  const auto root = local_root();
  if (!development_owner && !recover)
    return;
  if (!fs::exists(root))
    return;
  if (!root.is_absolute() || fs::is_symlink(root))
    throw std::invalid_argument("invalid local Agent directory");
  own_development(root);
  FileLock bootstrap(root, "bootstrap.lock");
  const auto identity_file = root / "ipc-id";
  if (!fs::exists(identity_file))
    return;
  if (fs::is_symlink(identity_file) || !fs::is_regular_file(identity_file) ||
      fs::file_size(identity_file) > 64)
    throw std::runtime_error("invalid Agent identity file");
  std::ifstream input(identity_file);
  std::string identity;
  input >> identity;
  input >> std::ws;
  if (!input.eof() || identity.size() != 32 ||
      identity.find_first_not_of("0123456789abcdef") != std::string::npos)
    throw std::runtime_error("invalid Agent identity");
  const auto endpoint = (fs::path("/tmp") / ("ast-node-" + identity) / "node.sock").string();
  const auto installed = root / "bin" / "asterion-node-agent";
  std::unique_ptr<NodeClient> control;
  try {
    control = std::make_unique<NodeClient>(NodeEndpoint{"local", "localhost", 0, {}, endpoint});
  } catch (const Error&) {
    // A disconnected socket does not prove the Agent stopped.
    {
      FileLock stopped(root, "agent.lock");
    }
    const auto definition =
        environment("HOME") / "Library/LaunchAgents/me.asterion.node-agent.dev.plist";
    if (fs::exists(definition))
      verify_node_service_stopped(installed, root, endpoint, local_node_service_name());
    else
      return;
  }
  if (control) {
    const auto status = control->inspect_status();
    if (!status.online || !status.health || status.health->maintenance())
      throw Error(ErrorCode::conflict,
                  "development shutdown is waiting for Agent maintenance to finish");
    auto services = status.health->services();
    const auto priority = [](node::v1::ServiceKind kind) {
      if (kind == node::v1::TASK_SERVICE)
        return 0;
      if (kind == node::v1::LIVE_TRADING)
        return 1;
      return 2;
    };
    std::stable_sort(services.begin(), services.end(), [&](const auto& a, const auto& b) {
      return priority(a.kind()) < priority(b.kind());
    });
    // Same explicit stop operation as service management: workers are joined,
    // desired-running is cleared durably, and incomplete tasks recover as interrupted.
    for (const auto& service : services)
      if (service.desired_running() || service.state() != "stopped" || service.active_workers())
        control->action(service.id(), "stop");
    const auto stopped = control->inspect_status();
    if (!stopped.online || !stopped.health)
      throw Error(ErrorCode::unavailable,
                  "development Agent status is unavailable during shutdown");
    for (const auto& service : stopped.health->services())
      if (service.desired_running() || service.state() != "stopped" || service.active_workers())
        throw Error(ErrorCode::unavailable, "development service has not stopped: " + service.id());
    stop_node_service(installed, root, endpoint, stopped.health->pid(), local_node_service_name());
  }
  // A stopped development environment must not be launched again at login.
  // stop/verify above checks the exact owned definition before removal.
  const auto definition =
      environment("HOME") / "Library/LaunchAgents/me.asterion.node-agent.dev.plist";
  fs::remove(definition);
  sync_directory(definition.parent_path());
}
NodeEndpoint local_node() {
  static std::mutex bootstrap;
  std::lock_guard lock(bootstrap);
  const auto root = local_root();
  if (!root.is_absolute() || fs::is_symlink(root))
    throw std::invalid_argument("invalid local Agent directory");
  fs::create_directories(root);
#ifndef _WIN32
  fs::permissions(root, fs::perms::owner_all);
#endif
  if (development_environment() && !environment_variable("ASTERION_NODE_DIRECTORY"))
    own_development(root);
  FileLock ownership(root, "bootstrap.lock");
  for (const auto& file : {"agent-upgrade.json", "agent-upgrade.pending",
                           "agent-service-upgrade.json", "agent-service-upgrade.pending"}) {
    const auto pending = root / file;
    if (fs::exists(pending) || fs::is_symlink(pending))
      throw std::runtime_error("unfinished Agent update requires explicit recovery");
  }
  // Persist a random socket namespace; never trust a shared predictable socket.
  const auto identity_file = root / "ipc-id";
  if (fs::is_symlink(identity_file))
    throw std::invalid_argument("invalid Agent identity file");
  std::string identity;
  if (fs::exists(identity_file)) {
    std::ifstream input(identity_file);
    input >> identity;
  } else {
    identity = unique_process_id();
    write_file_durably(identity_file, identity);
  }
  if (identity.size() != 32 || identity.find_first_not_of("0123456789abcdef") != std::string::npos)
    throw std::invalid_argument("invalid Agent identity");
#ifdef _WIN32
  const auto endpoint = "asterion.node." + identity;
#else
  const auto sockets = fs::path("/tmp") / ("ast-node-" + identity);
  if (::mkdir(sockets.c_str(), 0700) != 0 && errno != EEXIST)
    throw std::runtime_error("cannot create Agent socket directory");
  struct stat st{};
  if (::lstat(sockets.c_str(), &st) || !S_ISDIR(st.st_mode) || st.st_uid != ::getuid() ||
      (st.st_mode & 077) != 0)
    throw std::runtime_error("Agent socket directory is not private");
  const auto endpoint = (sockets / "node.sock").string();
#endif
  NodeEndpoint config{"local", "localhost", 0, {}, endpoint};
  try {
    NodeClient probe(config);
    return config;
  } catch (const Error&) {
  }
  const auto executable = bundled_agent();
  if (environment_variable("ASTERION_NODE_DIRECTORY")) {
    // Explicit development/test isolation never registers a login service.
    ChildProcess process(executable, {"--directory", utf8(root), "--endpoint", endpoint}, true);
    process.release();
  } else {
    const auto bin = root / "bin";
    if (fs::is_symlink(bin))
      throw std::invalid_argument("invalid Agent binary directory");
    fs::create_directory(bin);
    const auto installed = bin / executable.filename();
    if (fs::exists(installed)) {
      if (sha256_file(installed) != sha256_file(executable))
        throw std::runtime_error(
            "local Agent version differs; upgrade the system service explicitly");
    } else
      fs::copy_file(executable, installed);
    install_node_service(installed, root, endpoint, local_node_service_name());
  }
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  for (;;) {
    try {
      NodeClient probe(config);
      return config;
    } catch (const Error&) {
      if (std::chrono::steady_clock::now() >= deadline)
        throw;
      std::this_thread::sleep_for(50ms);
    }
  }
}
} // namespace asterion::terminal
