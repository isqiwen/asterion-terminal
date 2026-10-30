#pragma once
#include "managed_service.hpp"
#include "node_paths.hpp"
#include "node_snapshot.hpp"
#include "service_io.hpp"
#include <asterion/manager.pb.h>
#include <memory>
#include <vector>

namespace asterion::terminal {

class NodeClient;

// One connection to the local Manager. Node handles have no Agent transport or registry.
class ManagerClient
{
public:
    explicit ManagerClient(ServiceIo&);
    ~ManagerClient();
    ManagerClient(const ManagerClient&) = delete;
    ManagerClient& operator=(const ManagerClient&) = delete;
    std::future<manager::proto::Response> request(manager::proto::Request);
    std::shared_ptr<NodeClient> node(std::string id);
    const std::vector<NodeSnapshot>& view() const;
    std::future<Json> inventory();
    std::future<void> shutdown(bool recover);

private:
    friend class NodeClient;
    ServiceIo& io_;
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

class NodeClient
{
public:
    NodeClient(ManagerClient& manager, std::string id)
        : manager_(manager),
          id_(std::move(id))
    {}

    std::future<std::vector<HistoryService>> history_inventory();
    std::future<void> action(const std::string&, const std::string&);
    std::future<ServiceEndpoint> service_endpoint(const std::string&, node::proto::ServiceKind);
    std::future<ServiceEndpoint> local_market();
    std::future<ServiceEndpoint> local_data();
    std::future<ServiceEndpoint> local_research(
        const std::optional<std::vector<std::string>>& = std::nullopt);

private:
    std::future<ServiceEndpoint> endpoint(manager::proto::NodeCommand);
    ManagerClient& manager_;
    std::string id_;
};

} // namespace asterion::terminal
