#include "service_configuration.hpp"
#include "managed_paths.hpp"
#include <asterion/foundation/serialization.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <fstream>
namespace asterion::agent {
namespace fs = std::filesystem;
namespace wire = node::v1;
namespace {
Json configuration_json(const ServiceConfiguration& configuration) {
  Json value{{"version", 5},
             {"kind", static_cast<int>(configuration.kind)},
             {"provider_artifact", configuration.provider_artifact},
             {"artifact", configuration.artifact},
             {"port", configuration.port},
             {"desired", configuration.desired},
             {"directory", configuration.directory}};
  value["plugin_artifacts"] = configuration.plugin_artifacts;
  if (!configuration.catalog_artifact.empty())
    value["catalog_artifact"] = configuration.catalog_artifact;
  if (configuration.kind == wire::TASK_SERVICE) {
    value["data_service"] = configuration.data_service;
    value["worker_artifact"] = configuration.worker_artifact;
    value["factor_artifact"] = configuration.factor_artifact;
    value["data_artifact"] = configuration.data_artifact;
  }
  if (configuration.kind == wire::DATA_SERVICE)
    value["task_service"] = configuration.task_service;
  return value;
}
} // namespace
std::string service_revision(const ServiceConfiguration& configuration) {
  return sha256_bytes(configuration_json(configuration).dump());
}
void save_service_configuration(const fs::path& folder,
                                const ServiceConfiguration& configuration_value) {
  const auto path = folder / "service.json";
  const auto pending = path.parent_path() / "service.pending";
  require_managed_path(path);
  require_managed_path(pending);
  if (fs::exists(pending))
    throw std::runtime_error("unfinished service configuration requires explicit recovery");
  const auto configuration = configuration_json(configuration_value);
  write_file_durably(pending, configuration.dump());
  fs::rename(pending, path);
  sync_directory(path.parent_path());
}
ServiceConfiguration load_service_configuration(const fs::path& folder, bool local,
                                                unsigned short control_port) {
  const auto path = folder / "service.json";
  require_managed_path(path);
  require_managed_path(folder / "service.pending");
  if (fs::exists(folder / "service.pending"))
    throw std::runtime_error("unfinished service configuration requires explicit recovery");
  if (!fs::is_regular_file(path) || fs::file_size(path) > 65536)
    throw std::invalid_argument("invalid managed configuration file");
  std::ifstream input(path);
  std::string raw{std::istreambuf_iterator<char>(input), {}};
  auto document = parse_json(raw, 65536);
  if (document.at("version") != 5)
    throw std::invalid_argument("unsupported managed service configuration version: " +
                                path.string());
  if (document.size() !=
          (document.at("kind") == static_cast<int>(wire::TASK_SERVICE) ? 12U : 8U) +
              (document.contains("catalog_artifact") ? 1U : 0U) +
              (document.at("kind") == static_cast<int>(wire::DATA_SERVICE) ? 1U : 0U) ||
      !document.at("desired").is_boolean() || !document.at("port").is_number_unsigned())
    throw std::invalid_argument("invalid managed service configuration");
  ServiceConfiguration configuration;
  configuration.kind = static_cast<wire::ServiceKind>(document.at("kind").get<int>());
  configuration.provider_artifact = document.at("provider_artifact").get<std::string>();
  configuration.plugin_artifacts = document.at("plugin_artifacts").get<std::vector<std::string>>();
  if (document.contains("catalog_artifact")) {
    if (configuration.kind != wire::MARKET_DATA && configuration.kind != wire::LIVE_TRADING)
      throw std::invalid_argument("CTP trader library only belongs to market data or live trading");
    configuration.catalog_artifact = document.at("catalog_artifact").get<std::string>();
    validate_artifact_digest(configuration.catalog_artifact);
  }
  if (configuration.kind != wire::MARKET_DATA && configuration.kind != wire::TASK_SERVICE &&
      configuration.kind != wire::LIVE_TRADING && configuration.kind != wire::DATA_SERVICE)
    throw std::invalid_argument("unsupported service kind");
  if (!configuration.provider_artifact.empty()) {
    if (configuration.kind != wire::MARKET_DATA)
      throw std::invalid_argument("provider library only belongs to market data");
    validate_artifact_digest(configuration.provider_artifact);
  }
  if (configuration.kind == wire::DATA_SERVICE) {
    configuration.task_service = document.at("task_service").get<std::string>();
    validate_service_id(configuration.task_service);
  }
  if (configuration.kind == wire::TASK_SERVICE) {
    configuration.data_service = document.at("data_service").get<std::string>();
    validate_service_id(configuration.data_service);
    configuration.worker_artifact = document.at("worker_artifact").get<std::string>();
    validate_artifact_digest(configuration.worker_artifact);
    configuration.factor_artifact = document.at("factor_artifact").get<std::string>();
    validate_artifact_digest(configuration.factor_artifact);
    configuration.data_artifact = document.at("data_artifact").get<std::string>();
    validate_artifact_digest(configuration.data_artifact);
    if (!configuration.provider_artifact.empty())
      throw std::invalid_argument("task service does not load a market provider");
  }
  if (configuration.kind == wire::LIVE_TRADING && configuration.catalog_artifact.empty())
    throw std::invalid_argument("live trading requires the CTP trader library");
  configuration.artifact = document.at("artifact").get<std::string>();
  validate_artifact_digest(configuration.artifact);
  const auto port = document.at("port").get<unsigned int>();
  if (port > 65535 || (local ? port != 0 : (!port || port == control_port)))
    throw std::invalid_argument("invalid managed port");
  configuration.port = static_cast<unsigned short>(port);
  configuration.desired = document.at("desired").get<bool>();
  configuration.directory = document.at("directory").get<std::string>();
  const fs::path ledger(
      std::u8string(configuration.directory.begin(), configuration.directory.end()));
  if (!ledger.is_absolute())
    throw std::invalid_argument("ledger must be absolute");
  require_managed_path(ledger);
  if (!fs::is_directory(ledger))
    throw std::invalid_argument("managed ledger is missing");
  return configuration;
}
} // namespace asterion::agent
