#include "node_client.hpp"
#include "application_environment.hpp"
#include "node_program.hpp"
#include "node_service.hpp"
#include "service_programs.hpp"
#include "plugin_catalog.hpp"
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
#include <sys/stat.h>
#include <unistd.h>
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
  const bool development = development_environment();
  auto root = environment("ASTERION_NODE_DIRECTORY");
  if (root.empty()) {
#if defined(__APPLE__)
    root = environment("HOME") / "Library" / "Application Support" /
           (development ? "Asterion Development" : "Asterion") / "node";
#else
    root = environment("XDG_DATA_HOME");
    if (root.empty())
      root = environment("HOME") / ".local" / "share";
    root /= development ? "asterion-development/node" : "asterion/node";
#endif
  }
  return root;
}
fs::path bundled_agent() {
  auto executable = environment("ASTERION_NODE_AGENT_EXECUTABLE");
  if (executable.empty())
    executable = current_executable().parent_path() / "asterion-node-agent";
  return executable;
}

void prepare_development(const fs::path& root, const fs::path& agent) {
  node::v1::DevelopmentPrograms manifest;
  manifest.set_version(1);
  for (const auto kind : {node::v1::MARKET_DATA, node::v1::DATA_SERVICE, node::v1::TASK_SERVICE,
                          node::v1::LIVE_TRADING}) {
    const auto programs = local_service_programs(kind);
    auto& source = *manifest.add_services();
    source.set_kind(kind);
    source.set_executable(utf8(programs.executable));
    source.set_provider(utf8(programs.provider));
    source.set_catalog(utf8(programs.catalog));
    source.set_worker(utf8(programs.worker));
    source.set_factor(utf8(programs.factor));
    source.set_data(utf8(programs.data));
  }
  for (const auto& plugin : local_plugin_catalog().entries) {
    if (plugin.managed)
      continue;
    if (plugin.availability != PluginAvailability::available)
      throw std::runtime_error("bundled development plugin is unavailable");
    manifest.add_bundled_plugins(utf8(plugin.artifact.path));
  }
  const auto file = root / "development-programs.pb";
  replace_file_durably(file, manifest.SerializeAsString());
  create_directories_durably(root / "logs");
  ChildProcess preparation(
      agent, {"--directory", utf8(root), "--prepare-development", utf8(file)}, false,
      root / "logs" / ("development-programs-" + unique_process_id() + ".log"), true);
  if (!preparation.wait(60s) || preparation.exit_code() != 0)
    throw std::runtime_error("development program synchronization failed; inspect node logs");
  fs::remove(file);
  sync_directory(root);
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
  const auto home = environment_path("HOME");
  if (!home)
    throw std::runtime_error("local user data directory unavailable");
  return *home / ".asterion" / "nodes";
}
std::filesystem::path ctp_account_directory(const std::string& account, bool create) {
  // The account id is the directory name: one trading record per CTP account.
  if (account.empty() || account.size() > 64 ||
      account.find_first_not_of(
          "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos)
    throw std::invalid_argument("invalid CTP connection identity");
  const auto root = local_root() / "accounts";
  const auto group = root / "ctp";
  const auto directory = group / account;
  if (!create)
    return directory;
  for (const auto& path : {root, group, directory}) {
    if (fs::is_symlink(path))
      throw std::invalid_argument("invalid managed account directory");
    create_directories_durably(path);
    fs::permissions(path, fs::perms::owner_all, fs::perm_options::replace);
  }
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
NodeEndpoint upgrade_local_node(ServiceIo& io, const std::string& expected) {
  if (environment_variable("ASTERION_NODE_DIRECTORY"))
    throw std::runtime_error("system Agent upgrade is unavailable in isolated development");
  // Development bootstrap synchronizes services before the updated Agent can
  // recover them. The production upgrade path resumes saved programs directly.
  if (development_environment())
    return local_node(io);
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
  const auto endpoint = (fs::path("/tmp") / ("ast-node-" + identity) / "node.sock").string();
  upgrade_node_service(io, source, root / "bin" / source.filename(), root, endpoint, expected,
                       local_node_service_name());
  return NodeEndpoint{"local", "localhost", 0, {}, endpoint};
}
void shutdown_development_node(ServiceIo& io, bool recover) {
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
  std::shared_ptr<NodeClient> control;
  try {
    control = NodeClient::open(io, NodeEndpoint{"local", "localhost", 0, {}, endpoint}).get();
  } catch (const Error&) {
    // A disconnected socket does not prove the Agent stopped.
    {
      FileLock stopped(root, "agent.lock");
    }
    if (!fs::exists(node_service_definition(local_node_service_name())))
      return;
    verify_node_service_stopped(installed, root, endpoint, local_node_service_name());
  }
  if (control) {
    const auto status = control->inspect_status().get();
    if (!status.online || !status.health)
      throw Error(ErrorCode::unavailable,
                  "development Agent status is unavailable during shutdown");
    // Agent owns service shutdown, including during initialization or recovery.
    // Ending this environment must not rewrite persisted service intentions.
    stop_node_service(installed, root, endpoint, status.health->pid(), local_node_service_name());
  }
  // A stopped development environment must not be launched again at login.
  // stop/verify above checks the exact owned definition before removal.
  remove_node_service(local_node_service_name());
}
NodeEndpoint local_node(ServiceIo& io) {
  static std::mutex bootstrap;
  std::lock_guard lock(bootstrap);
  const auto root = local_root();
  if (!root.is_absolute() || fs::is_symlink(root))
    throw std::invalid_argument("invalid local Agent directory");
  create_directories_durably(root);
  fs::permissions(root, fs::perms::owner_all);
  const bool managed_development =
      development_environment() && !environment_variable("ASTERION_NODE_DIRECTORY");
  static bool development_prepared = false;
  const bool prepare = managed_development && !development_prepared;
  if (managed_development) {
    own_development(root);
    if (prepare)
      shutdown_development_node(io, true);
  }
  FileLock ownership(root, "bootstrap.lock");
  for (const auto& file : {"agent-upgrade.json", "agent-upgrade.pending",
                           "agent-service-upgrade.json", "agent-service-upgrade.pending"}) {
    const auto pending = root / file;
    if (fs::exists(pending) || fs::is_symlink(pending))
      throw std::runtime_error("unfinished Agent update requires explicit recovery");
  }
  const auto executable = bundled_agent();
  if (prepare) {
    prepare_development(root, executable);
    const auto installed = root / "bin" / executable.filename();
    if (fs::exists(installed))
      replace_node_program(executable, installed, root, sha256_file(installed));
    development_prepared = true;
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
    replace_file_durably(identity_file, identity);
  }
  if (identity.size() != 32 || identity.find_first_not_of("0123456789abcdef") != std::string::npos)
    throw std::invalid_argument("invalid Agent identity");
  const auto sockets = fs::path("/tmp") / ("ast-node-" + identity);
  if (::mkdir(sockets.c_str(), 0700) != 0 && errno != EEXIST)
    throw std::runtime_error("cannot create Agent socket directory");
  struct stat st{};
  if (::lstat(sockets.c_str(), &st) || !S_ISDIR(st.st_mode) || st.st_uid != ::getuid() ||
      (st.st_mode & 077) != 0)
    throw std::runtime_error("Agent socket directory is not private");
  const auto endpoint = (sockets / "node.sock").string();
  NodeEndpoint config{"local", "localhost", 0, {}, endpoint};
  try {
    auto probe = NodeClient::open(io, config).get();
    return config;
  } catch (const Error&) {
  }
  if (environment_variable("ASTERION_NODE_DIRECTORY")) {
    // Explicit development/test isolation never registers a login service.
    ChildProcess process(executable, {"--directory", utf8(root), "--endpoint", endpoint}, true);
    process.release();
  } else {
    const auto bin = root / "bin";
    if (fs::is_symlink(bin))
      throw std::invalid_argument("invalid Agent binary directory");
    const auto installed = bin / executable.filename();
    install_node_program(executable, installed, root);
    install_node_service(installed, root, endpoint, local_node_service_name());
  }
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  for (;;) {
    try {
      auto probe = NodeClient::open(io, config).get();
      return config;
    } catch (const Error&) {
      if (std::chrono::steady_clock::now() >= deadline)
        throw;
      std::this_thread::sleep_for(50ms);
    }
  }
}
} // namespace asterion::terminal
