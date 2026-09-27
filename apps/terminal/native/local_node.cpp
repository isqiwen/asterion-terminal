#include "node_client.hpp"
#include "node_program.hpp"
#include "node_service.hpp"
#include <asterion/foundation/error.hpp>
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/file_lock.hpp>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>
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
fs::path environment(const char* name) {
  const auto* p = std::getenv(name);
  return p ? fs::path(std::u8string(p, p + std::strlen(p))) : fs::path{};
}
std::string utf8(const fs::path& p) {
  const auto s = p.u8string();
  return {s.begin(), s.end()};
}
fs::path local_root() {
  auto root = environment("ASTERION_NODE_DIRECTORY");
  if (root.empty()) {
#ifdef _WIN32
    root = environment("LOCALAPPDATA") / "Asterion" / "node";
#elif defined(__APPLE__)
    root = environment("HOME") / "Library" / "Application Support" / "Asterion" / "node";
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
Json local_node_program_status() {
  if (std::getenv("ASTERION_NODE_DIRECTORY"))
    return Json{{"state", "isolated"},
                {"expected_digest", ""},
                {"installed_digest", ""},
                {"bundled_digest", ""}};
  const auto root = local_root(), source = bundled_agent();
  return inspect_node_program(source, root / "bin" / source.filename(), root);
}
NodeEndpoint upgrade_local_node(const std::string& expected) {
  if (std::getenv("ASTERION_NODE_DIRECTORY"))
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
  upgrade_node_service(source, root / "bin" / source.filename(), root, endpoint, expected);
  return NodeEndpoint{"local", "localhost", 0, {}, endpoint};
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
    std::ofstream out(identity_file);
    out << identity;
    out.close();
    if (!out)
      throw std::runtime_error("cannot persist Agent identity");
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
  if (std::getenv("ASTERION_NODE_DIRECTORY")) {
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
    install_node_service(installed, root, endpoint);
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
