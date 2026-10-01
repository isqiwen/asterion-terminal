#include "node_program.hpp"
#include "node_client.hpp"
#include "market_client.hpp"
#include "node_service.hpp"
#include <asterion/foundation/error.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <asterion/foundation/serialization.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/file_lock.hpp>
#include <fstream>
#include <thread>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif
namespace asterion::terminal {
namespace fs = std::filesystem;
namespace {
std::string text(const fs::path& p) {
  const auto value = p.u8string();
  return {value.begin(), value.end()};
}
void plain(const fs::path& p) {
  if (fs::is_symlink(p))
    throw std::runtime_error("Agent update path cannot be a symlink");
}
void publish(const fs::path& source, const fs::path& destination) {
#ifdef _WIN32
  if (!MoveFileExW(source.c_str(), destination.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    throw std::runtime_error("cannot publish Agent update");
#else
  fs::rename(source, destination);
  sync_directory(destination.parent_path());
#endif
}
} // namespace
Json inspect_node_program(const fs::path& source, const fs::path& installed, const fs::path& root) {
  if (!source.is_absolute() || !installed.is_absolute() || !root.is_absolute() ||
      installed.parent_path() != root / "bin")
    throw std::invalid_argument("invalid Agent inspection paths");
  for (const auto& path : {root, root / "bin", source, installed})
    plain(path);
  if (!fs::is_regular_file(source))
    throw std::runtime_error("bundled Agent program is missing");
  const auto platform = artifact_platform(source), host = current_platform();
  if (platform.os != host.os || platform.arch != host.arch)
    throw std::runtime_error("bundled Agent platform mismatch");
  const auto bundled = sha256_file(source);
  std::string installed_digest;
  if (fs::exists(installed)) {
    if (!fs::is_regular_file(installed))
      throw std::runtime_error("installed Agent is not a regular file");
    installed_digest = sha256_file(installed);
  }
  bool pending = false;
  for (const auto& path : {root / "agent-service-upgrade.json",
                           root / "agent-service-upgrade.pending", root / "agent-upgrade.json",
                           root / "agent-upgrade.pending", root / "bin" / "agent-upgrade.staged"})
    pending = pending || fs::exists(path) || fs::is_symlink(path);
  std::string expected = pending ? std::string{} : installed_digest;
  if (pending) {
    const auto record = root / "agent-service-upgrade.json";
    try {
      plain(record);
      if (fs::is_regular_file(record) && fs::file_size(record) <= 4096 &&
          !fs::exists(root / "agent-service-upgrade.pending") &&
          !fs::is_symlink(root / "agent-service-upgrade.pending")) {
        std::ifstream input(record);
        std::string bytes{std::istreambuf_iterator<char>(input), {}};
        const auto value = parse_json(bytes);
        require_fields(value,
                       {"version", "installed", "endpoint", "name", "before", "after", "phase"});
        const auto before = value.at("before").get<std::string>();
        if (value.at("version") == 1 && value.at("installed") == text(installed) &&
            value.at("after") == bundled && before.size() == 64 &&
            before.find_first_not_of("0123456789abcdef") == std::string::npos)
          expected = before;
      }
    } catch (const std::exception&) { /* Inspection reports a retained recovery state. */
    }
  }
  return Json{{"state", pending                       ? "recovery_required"
                        : installed_digest.empty()    ? "not_installed"
                        : installed_digest == bundled ? "current"
                                                      : "update_available"},
              {"expected_digest", expected},
              {"installed_digest", installed_digest},
              {"bundled_digest", bundled}};
}
void replace_node_program(const fs::path& source, const fs::path& installed, const fs::path& root,
                          const std::string& expected_digest) {
  if (!source.is_absolute() || !installed.is_absolute() || !root.is_absolute() ||
      installed.parent_path() != root / "bin" || source == installed ||
      expected_digest.size() != 64 ||
      expected_digest.find_first_not_of("0123456789abcdef") != std::string::npos)
    throw std::invalid_argument("invalid Agent update identity");
  for (const auto& path : {source, installed, root, root / "bin"})
    plain(path);
  if (!fs::is_regular_file(source) || !fs::is_regular_file(installed))
    throw std::runtime_error("Agent programs must be regular files");
  FileLock stopped(root, "agent.lock");
  const auto platform = artifact_platform(source), host = current_platform();
  if (platform.os != host.os || platform.arch != host.arch)
    throw std::invalid_argument("Agent update platform mismatch");
  const auto target = sha256_file(source);
  const auto journal = root / "agent-upgrade.json";
  const auto pending = root / "agent-upgrade.pending";
  const auto candidate = root / "bin" / "agent-upgrade.staged";
  for (const auto& path : {journal, pending, candidate})
    plain(path);
  if (fs::exists(pending))
    throw std::runtime_error("unfinished Agent update record requires inspection");
  const Json transaction = {{"version", 1},
                            {"installed", text(installed)},
                            {"before", expected_digest},
                            {"after", target}};
  if (fs::exists(journal)) {
    if (fs::file_size(journal) > 4096)
      throw std::runtime_error("Agent update record is too large");
    std::ifstream in(journal, std::ios::binary);
    std::string bytes{std::istreambuf_iterator<char>(in), {}};
    const auto saved = parse_json(bytes);
    require_fields(saved, {"version", "installed", "before", "after"});
    if (saved != transaction)
      throw std::runtime_error("Agent update transaction differs");
  } else {
    if (sha256_file(installed) != expected_digest)
      throw std::runtime_error("installed Agent changed; inspect before updating");
    if (fs::exists(candidate))
      throw std::runtime_error("unrecorded staged Agent requires inspection");
    if (target == expected_digest)
      return;
    write_file_durably(pending, transaction.dump());
    publish(pending, journal);
  }
  const auto observed = sha256_file(installed);
  if (observed != expected_digest && observed != target)
    throw std::runtime_error("Agent program does not match recorded update");
  if (observed == expected_digest) {
    if (fs::exists(candidate)) {
      if (!fs::is_regular_file(candidate) || sha256_file(candidate) != target)
        throw std::runtime_error("staged Agent differs; inspection required");
    } else {
      fs::copy_file(source, candidate);
      if (sha256_file(candidate) != target)
        throw std::runtime_error("Agent source changed during staging");
    }
    const auto staged = artifact_platform(candidate);
    if (staged.os != host.os || staged.arch != host.arch)
      throw std::runtime_error("staged Agent platform mismatch");
    publish(candidate, installed);
  } else if (fs::exists(candidate)) {
    throw std::runtime_error("unexpected staged Agent after publication");
  }
  if (sha256_file(installed) != target)
    throw std::runtime_error("published Agent integrity check failed");
  // Only transaction metadata is removed; all identity, service and ledger
  // data remain intact. Restart/health confirmation is the caller's next step.
  fs::remove(journal);
}

void upgrade_node_service(const fs::path& source, const fs::path& installed, const fs::path& root,
                          const std::string& endpoint, const std::string& expected_digest,
                          const std::string& name) {
  if (!source.is_absolute() || !installed.is_absolute() || !root.is_absolute() ||
      installed.parent_path() != root / "bin" || source == installed ||
      expected_digest.size() != 64 ||
      expected_digest.find_first_not_of("0123456789abcdef") != std::string::npos)
    throw std::invalid_argument("invalid Agent update identity");
  for (const auto& path : {root, root / "bin", source, installed})
    plain(path);
  // Serialize against ordinary local_node bootstrap and other upgrade calls.
  FileLock bootstrap(root, "bootstrap.lock");
  const auto platform = artifact_platform(source), host = current_platform();
  if (platform.os != host.os || platform.arch != host.arch)
    throw std::invalid_argument("Agent update platform mismatch");
  const auto target = sha256_file(source), observed = sha256_file(installed);
  if (observed != expected_digest && observed != target)
    throw std::runtime_error("installed Agent changed; inspect before updating");
  const auto record_path = root / "agent-service-upgrade.json";
  const auto pending_path = root / "agent-service-upgrade.pending";
  plain(record_path);
  plain(pending_path);
  if (fs::exists(pending_path))
    throw std::runtime_error("unfinished Agent service update record requires inspection");
  // A second Terminal can arrive after the first has completed and removed its
  // journal. Repeating the same publication is a no-op, including a retry with
  // the original before digest. Retained publication artifacts still require
  // recovery and must never be mistaken for a completed update.
  if (observed == target && !fs::exists(record_path)) {
    if (inspect_node_program(source, installed, root).at("state") != "current")
      throw std::runtime_error("unfinished Agent publication requires inspection");
    return;
  }
  Json record;
  if (fs::exists(record_path)) {
    if (fs::file_size(record_path) > 4096)
      throw std::runtime_error("Agent service update record is too large");
    std::ifstream input(record_path, std::ios::binary);
    std::string bytes{std::istreambuf_iterator<char>(input), {}};
    record = parse_json(bytes);
    require_fields(record,
                   {"version", "installed", "endpoint", "name", "before", "after", "phase"});
    if (record.at("version") != 1 || record.at("installed") != text(installed) ||
        record.at("endpoint") != endpoint || record.at("name") != name ||
        record.at("before") != expected_digest || record.at("after") != target ||
        (record.at("phase") != "draining" && record.at("phase") != "quiesced" &&
         record.at("phase") != "stopped" && record.at("phase") != "published"))
      throw std::runtime_error("Agent service update transaction differs");
  } else {
    if (observed != expected_digest)
      throw std::runtime_error("installed Agent changed before update");
    record = {{"version", 1},       {"installed", text(installed)}, {"endpoint", endpoint},
              {"name", name},       {"before", expected_digest},    {"after", target},
              {"phase", "draining"}};
  }
  auto save = [&](const std::string& phase) {
    if (fs::exists(pending_path))
      throw std::runtime_error("unfinished Agent service update record requires inspection");
    record["phase"] = phase;
    write_file_durably(pending_path, record.dump());
    publish(pending_path, record_path);
  };
  NodeEndpoint config{"local", "localhost", 0, {}, endpoint};
  const auto operation = "upgrade." + target.substr(0, 32);
  if (record.at("phase") == "draining") {
    NodeClient control(config);
    // Capture clients before prepare: idle services may exit as soon as the
    // Agent accepts the maintenance plan. No session is changed before validation.
    std::vector<std::unique_ptr<MarketClient>> markets;
    const auto services = control.status().at("health").at("services");
    for (const auto& service : services) {
      if (service.at("kind") == "market" && service.at("state") == "running")
        markets.push_back(std::make_unique<MarketClient>(control.service_endpoint(
            service.at("id").get<std::string>(), asterion::node::v1::MARKET_DATA)));
    }
    control.coordinate_upgrade(operation, "prepare");
    save("draining");
    for (auto& market : markets) {
      const auto state = market->snapshot();
      if ((state.at("phase") != "disconnected" && state.at("phase") != "sdk_unavailable") ||
          state.at("catalog").at("phase") == "loading")
        market->disconnect();
    }
    markets.clear();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
      const auto progress = control.coordinate_upgrade(operation, "prepare");
      if (progress.at("phase") == "ready")
        break;
      if (std::chrono::steady_clock::now() >= deadline)
        throw Error(ErrorCode::unavailable,
                    "Agent upgrade is waiting for a recoverable service boundary: " +
                        progress.at("detail").get<std::string>());
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    save("quiesced");
  }
  if (record.at("phase") == "quiesced") {
    std::unique_ptr<NodeClient> control;
    try {
      control = std::make_unique<NodeClient>(config);
    } catch (const Error&) {
      if (!fs::exists(record_path))
        throw;
      verify_node_service_stopped(installed, root, endpoint, name);
    }
    if (control) {
      const auto status = control->status().at("health");
      const auto progress = control->coordinate_upgrade(operation, "prepare");
      if (progress.at("phase") != "ready")
        throw std::runtime_error("Agent upgrade has not reached its recovery boundary");
      const auto pid = status.at("pid").get<std::uint64_t>();
      if (!pid)
        throw std::runtime_error("Agent did not provide its process identity");
      save("quiesced");
      stop_node_service(installed, root, endpoint, pid, name);
    }
    save("stopped");
  }
  if (record.at("phase") == "stopped") {
    if (sha256_file(installed) != target || fs::exists(root / "agent-upgrade.json"))
      replace_node_program(source, installed, root, expected_digest);
    else {
      FileLock stopped(root, "agent.lock");
      if (fs::exists(root / "agent-upgrade.pending") ||
          fs::exists(root / "bin" / "agent-upgrade.staged"))
        throw std::runtime_error("unfinished Agent publication requires inspection");
    }
    save("published");
  }
  if (sha256_file(installed) != target)
    throw std::runtime_error("published Agent differs from update record");
  install_node_service(installed, root, endpoint, name);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  for (;;) {
    try {
      NodeClient probe(config);
      if (sha256_file(installed) != target)
        throw std::runtime_error("Agent program changed after restart");
      const auto progress = probe.coordinate_upgrade(operation, "resume");
      if (progress.at("phase") != "restoring" && progress.at("phase") != "complete")
        throw std::runtime_error("Agent upgrade restoration has not started");
      if (progress.at("phase") != "complete")
        probe.coordinate_upgrade(operation, "complete");
      fs::remove(record_path);
      sync_directory(root);
      return;
    } catch (const Error&) {
      if (std::chrono::steady_clock::now() >= deadline)
        throw;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
}
} // namespace asterion::terminal
