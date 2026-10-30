#pragma once
#include "service_endpoint.hpp"
#include "service_io.hpp"
#include "plugin_catalog.hpp"
#include "service_programs.hpp"
#include "node_snapshot.hpp"
#include "managed_service.hpp"
#include "node_paths.hpp"
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/foundation/serialization.hpp>
#include <filesystem>
#include <memory>
#include <optional>

namespace asterion::terminal {

struct NodeEndpoint
{
    std::string id, host;
    std::uint16_t port;
    std::string endpoint{};
};

NodeEndpoint local_node(ServiceIo&);
// Only the normal development profile; test directories and production are excluded.
void shutdown_development_node(ServiceIo&);
std::optional<NodeEndpoint> enrolled_local_node();

struct ServiceDeployment
{
    std::string service;
    node::proto::ServiceKind kind = node::proto::UNSPECIFIED_SERVICE;
    HostPlatform platform;
    ServicePrograms programs;
    std::uint16_t port = 0;
    std::string directory;
    // nullopt uses the bundled catalog; an empty vector explicitly disables all plugins.
    std::optional<std::vector<PluginArtifact>> plugins;
};

struct ServiceUpdate
{
    std::string service, expected_revision;
    HostPlatform platform;
    ServicePrograms programs;
};

class AgentClient
{
public:
    [[nodiscard]] static std::future<std::shared_ptr<AgentClient>>
    open(ServiceIo&, NodeEndpoint, bool observe = true);
    ~AgentClient();
    AgentClient(const AgentClient&) = delete;
    AgentClient& operator=(const AgentClient&) = delete;
    [[nodiscard]] std::future<Json> status() const;
    [[nodiscard]] std::future<void> refresh();
    [[nodiscard]] std::future<NodeSnapshot> inspect_status() const;
    // Explicit read-only inventory refresh; does not start or deploy services.
    [[nodiscard]] std::future<std::vector<HistoryService>> history_inventory();
    [[nodiscard]] std::future<void> configure_plugins(
        const std::string& service,
        const std::string& revision,
        const std::vector<std::string>& hashes);
    [[nodiscard]] std::future<Json>
    coordinate_upgrade(const std::string& operation, const std::string& action);
    [[nodiscard]] std::future<void>
    maintenance(bool enter, const std::string& operation, const std::string& instance);
    [[nodiscard]] std::future<ServiceEndpoint>
    service_endpoint(const std::string& service, node::proto::ServiceKind kind);
    [[nodiscard]] std::future<void> deploy(const ServiceDeployment& deployment);
    [[nodiscard]] std::future<void> update(const ServiceUpdate& update);
    [[nodiscard]] std::future<ServiceEndpoint> local_market();
    [[nodiscard]] std::future<ServiceEndpoint> local_data();
    [[nodiscard]] std::future<ServiceEndpoint> local_research(
        const std::optional<std::vector<std::string>>& selected_plugins = std::nullopt);
    [[nodiscard]] std::future<Json>
    firewall(const std::string& service, const std::string& action, const std::string& token = {});
    [[nodiscard]] std::future<void>
    action(const std::string& service, const std::string& operation);

    NodeSnapshot owner_view() const;

private:
    AgentClient(ServiceIo&, NodeEndpoint);
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

} // namespace asterion::terminal
