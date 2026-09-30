#pragma once
#include <asterion/v1/node.pb.h>
#include <filesystem>
#include <string>
#include <vector>
namespace asterion::agent {
struct ServiceConfiguration {
  node::v1::ServiceKind kind = node::v1::UNSPECIFIED_SERVICE;
  std::string artifact, provider_artifact, catalog_artifact, worker_artifact, factor_artifact,
      data_artifact;
  std::vector<std::string> plugin_artifacts;
  std::string directory;
  unsigned short port = 0;
  bool desired = true;
};
std::string service_revision(const ServiceConfiguration& configuration);
void save_service_configuration(const std::filesystem::path& folder,
                                const ServiceConfiguration& configuration);
ServiceConfiguration load_service_configuration(const std::filesystem::path& folder, bool local,
                                                unsigned short control_port);
} // namespace asterion::agent
