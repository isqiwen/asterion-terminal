#include "development_programs.hpp"
#include "managed_paths.hpp"
#include "plugin_artifacts.hpp"
#include "service_configuration.hpp"
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/file_lock.hpp>
#include <asterion/protocol/trading.hpp>
#include <fstream>
#include <map>
#include <iostream>

namespace asterion::agent {
namespace fs = std::filesystem;
void prepare_development_programs(const fs::path& root, const fs::path& manifest) {
  if (!root.is_absolute() || !fs::is_directory(root))
    throw std::invalid_argument("agent requires an existing absolute directory");
  require_managed_path(root);
  FileLock stopped(root, "agent.lock");
  require_managed_path(manifest);
  if (fs::file_size(manifest) > 1024 * 1024)
    throw std::invalid_argument("development program manifest is too large");
  node::v1::DevelopmentPrograms sources;
  std::ifstream input(manifest, std::ios::binary);
  if (!sources.ParseFromIstream(&input) || sources.version() != 1)
    throw std::invalid_argument("unsupported development program manifest");
  protocol::validate_message(sources);
  create_directories_durably(root / "artifacts");
  create_directories_durably(root / "services");
  std::map<std::string, ServiceConfiguration> configurations;
  for (const auto& entry : fs::directory_iterator(root / "services")) {
    validate_service_id(entry.path().filename().string());
    configurations.emplace(entry.path().filename().string(),
                           load_service_configuration(entry.path(), true, 0));
  }
  // Validate and stage the complete selection before changing any service reference.
  // A failed preparation never starts a child; a later run can repeat it safely.
  const auto stage = root / ("development-stage-" + unique_process_id());
  create_directories_durably(stage);
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code ignored;
      fs::remove_all(path, ignored);
    }
  } cleanup{stage};
  std::map<std::string, std::string> staged;
  auto install = [&](const std::string& name) -> std::string {
    if (name.empty())
      return {};
    if (const auto found = staged.find(name); found != staged.end())
      return found->second;
    const fs::path source(name);
    if (!source.is_absolute() || !fs::is_regular_file(source) || fs::is_symlink(source))
      throw std::invalid_argument("invalid development program source");
    const auto target = artifact_platform(source), host = current_platform();
    if (target.os != host.os || target.arch != host.arch)
      throw std::invalid_argument("development program platform mismatch");
    const auto hash = sha256_file(source);
    const auto destination = root / "artifacts" / (hash + ".bin");
    require_managed_path(destination);
    if (fs::exists(destination) && sha256_file(destination) != hash)
      throw std::invalid_argument("development program artifact is corrupted");
    if (!fs::exists(destination)) {
      const auto temporary = stage / hash;
      fs::copy_file(source, temporary);
      if (sha256_file(temporary) != hash)
        throw std::invalid_argument("development program changed during preparation");
      publish_file_durably(temporary, destination);
    }
    staged.emplace(name, hash);
    return hash;
  };
  std::map<node::v1::ServiceKind, ServiceConfiguration> programs;
  for (const auto& source : sources.services()) {
    const bool market = source.kind() == node::v1::MARKET_DATA;
    const bool trading = source.kind() == node::v1::LIVE_TRADING;
    const bool task = source.kind() == node::v1::TASK_SERVICE;
    if ((!market && !trading && !task && source.kind() != node::v1::DATA_SERVICE) ||
        source.executable().empty() || source.provider().empty() == market ||
        source.catalog().empty() == (market || trading) || source.worker().empty() == task ||
        source.factor().empty() == task || source.data().empty() == task)
      throw std::invalid_argument("invalid development service program selection");
    ServiceConfiguration p;
    p.kind = source.kind();
    p.artifact = install(source.executable());
    p.provider_artifact = install(source.provider());
    p.catalog_artifact = install(source.catalog());
    p.worker_artifact = install(source.worker());
    p.factor_artifact = install(source.factor());
    p.data_artifact = install(source.data());
    if (!programs.emplace(p.kind, std::move(p)).second)
      throw std::invalid_argument("invalid development service program selection");
  }
  PluginArtifacts plugins(root, current_executable());
  std::map<std::string, std::string> bundled;
  for (const auto& source : sources.bundled_plugins()) {
    const auto hash = install(source);
    if (!bundled.emplace(plugins.identity(root / "artifacts" / (hash + ".bin")), hash).second)
      throw std::invalid_argument("duplicate development plugin identity");
  }
  std::map<std::string, ServiceConfiguration> changed;
  for (auto& [name, config] : configurations) {
    auto next = programs.at(config.kind);
    next.directory = config.directory;
    next.port = config.port;
    next.desired = config.desired;
    next.task_service = config.task_service;
    next.data_service = config.data_service;
    plugins.verify(config.plugin_artifacts);
    next.plugin_artifacts = config.plugin_artifacts;
    for (auto& hash : next.plugin_artifacts) {
      const auto found = bundled.find(plugins.identity(root / "artifacts" / (hash + ".bin")));
      if (found != bundled.end())
        hash = found->second;
    }
    if (service_revision(next) != service_revision(config))
      changed.emplace(name, std::move(next));
  }
  for (const auto& [name, config] : changed)
    save_service_configuration(root / "services" / name, config);
  std::cout << "Development service programs synchronized: " << changed.size() << '\n';
}
} // namespace asterion::agent
