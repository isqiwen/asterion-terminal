#pragma once
#include "service_endpoint.hpp"
#include "service_io.hpp"
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
class Application;
struct NodeEndpoint {
  std::string id, host;
  std::uint16_t port;
  ipc::TlsIdentity tls;
  std::string endpoint{};
};
NodeEndpoint local_node(ServiceIo&);
// Only the normal development profile; test directories and production are excluded.
void shutdown_development_node(ServiceIo&, bool recover = false);
std::filesystem::path local_node_directory();
std::filesystem::path node_enrollment_directory();
// Record directory of one CTP account under the local node; created on request.
std::filesystem::path ctp_account_directory(const std::string& account, bool create);
std::filesystem::path keychain_helper();
Json local_node_program_status();
NodeEndpoint upgrade_local_node(ServiceIo&, const std::string& expected);
struct ServiceDeployment {
  std::string service;
  node::v1::ServiceKind kind = node::v1::UNSPECIFIED_SERVICE;
  HostPlatform platform;
  ServicePrograms programs;
  std::uint16_t port = 0;
  std::string directory;
  // nullopt uses the bundled catalog; an empty vector explicitly disables all plugins.
  std::optional<std::vector<PluginArtifact>> plugins;
  std::string task_service, data_service;
};
struct ServiceUpdate {
  std::string service, expected_revision;
  HostPlatform platform;
  ServicePrograms programs;
};
struct HistoryService {
  node::v1::ServiceKind kind;
  std::string directory, state, data_service;
  ServiceEndpoint address;
};
struct DataTaskEndpoints {
  ServiceEndpoint task, data;
};
class NodeClient {
public:
  [[nodiscard]] static std::future<std::shared_ptr<NodeClient>> open(ServiceIo&, NodeEndpoint);
  ~NodeClient();
  NodeClient(const NodeClient&) = delete;
  NodeClient& operator=(const NodeClient&) = delete;
  [[nodiscard]] std::future<Json> status() const;
  [[nodiscard]] std::future<NodeSnapshot> inspect_status() const;
  // Explicit read-only inventory refresh; does not start or deploy services.
  [[nodiscard]] std::future<std::vector<HistoryService>> history_inventory();
  [[nodiscard]] std::future<void> configure_plugins(const std::string& service,
                                                    const std::string& revision,
                                                    const std::vector<std::string>& hashes);
  [[nodiscard]] std::future<Json> coordinate_upgrade(const std::string& operation,
                                                     const std::string& action);
  [[nodiscard]] std::future<void> maintenance(bool enter, const std::string& operation,
                                              const std::string& instance);
  [[nodiscard]] std::future<ServiceEndpoint> service_endpoint(const std::string& service,
                                                              node::v1::ServiceKind kind);
  // Resolves the configured pair even if either process is offline.
  [[nodiscard]] std::future<DataTaskEndpoints> data_task_endpoints(const std::string& task_service);
  [[nodiscard]] std::future<void> deploy(const ServiceDeployment& deployment);
  [[nodiscard]] std::future<void> update(const ServiceUpdate& update);
  // The service of one CTP trading account record.
  [[nodiscard]] std::future<ServiceEndpoint> local_session(const std::filesystem::path& directory);
  [[nodiscard]] std::future<ServiceEndpoint> local_market();
  [[nodiscard]] std::future<ServiceEndpoint>
  local_data_tasks(const std::optional<std::vector<std::string>>& selected_plugins = std::nullopt);
  [[nodiscard]] std::future<Json> firewall(const std::string& service, const std::string& action,
                                           const std::string& token = {});
  [[nodiscard]] std::future<void> action(const std::string& service, const std::string& operation);

private:
  friend class Application;
  // Application collects selected service views in one I/O owner turn.
  NodeSnapshot owner_view() const;
  NodeClient(ServiceIo&, NodeEndpoint);
  struct Impl;
  std::shared_ptr<Impl> impl_;
};
} // namespace asterion::terminal
