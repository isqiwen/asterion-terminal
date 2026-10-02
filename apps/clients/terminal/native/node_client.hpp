#pragma once
#include "service_endpoint.hpp"
#include "plugin_catalog.hpp"
#include "service_programs.hpp"
#include "node_snapshot.hpp"
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/foundation/serialization.hpp>
#include <asterion/kernel/ipc/tls_channel.hpp>
#include <filesystem>
#include <memory>
#include <optional>
namespace asterion::terminal {
struct NodeEndpoint {
  std::string id, host;
  std::uint16_t port;
  ipc::TlsIdentity tls;
  std::string endpoint{};
};
NodeEndpoint local_node();
// Only the normal development profile; test directories and production are excluded.
void shutdown_development_node(bool recover = false);
std::filesystem::path local_node_directory();
std::filesystem::path node_enrollment_directory();
std::filesystem::path new_account_directory(const std::string& name);
std::filesystem::path keychain_helper();
Json local_node_program_status();
NodeEndpoint upgrade_local_node(const std::string& expected);
struct ServiceDeployment {
  std::string service;
  node::v1::ServiceKind kind = node::v1::UNSPECIFIED_SERVICE;
  HostPlatform platform;
  ServicePrograms programs;
  std::uint16_t port = 0;
  std::string directory;
  // nullopt uses the bundled catalog; an empty vector explicitly disables all plugins.
  std::optional<std::vector<PluginArtifact>> plugins;
};
struct ServiceUpdate {
  std::string service, expected_revision;
  HostPlatform platform;
  ServicePrograms programs;
};
struct HistoryService {
  node::v1::ServiceKind kind;
  std::string directory, state;
  ServiceEndpoint address;
};
class NodeClient {
public:
  explicit NodeClient(NodeEndpoint endpoint);
  ~NodeClient();
  Json status() const;
  NodeSnapshot inspect_status() const;
  // Explicit read-only inventory refresh; does not start or deploy services.
  std::vector<HistoryService> history_inventory();
  void configure_plugins(const std::string& service, const std::string& revision,
                         const std::vector<std::string>& hashes);
  Json coordinate_upgrade(const std::string& operation, const std::string& action);
  void maintenance(bool enter, const std::string& operation, const std::string& instance);
  ServiceEndpoint service_endpoint(const std::string& service, node::v1::ServiceKind kind);
  void deploy(const ServiceDeployment& deployment);
  void update(const ServiceUpdate& update);
  // The service of one CTP trading account record.
  ServiceEndpoint local_session(const std::filesystem::path& directory);
  ServiceEndpoint local_market();
  ServiceEndpoint
  local_research(const std::optional<std::vector<std::string>>& selected_plugins = std::nullopt);
  Json firewall(const std::string& service, const std::string& action,
                const std::string& token = {});
  void action(const std::string& service, const std::string& operation);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace asterion::terminal
