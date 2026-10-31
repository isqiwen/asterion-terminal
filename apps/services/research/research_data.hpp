#pragma once
#include <asterion/data_service.pb.h>
#include <asterion/task.pb.h>
#include <filesystem>
#include <asterion/node.pb.h>
#include <asterion/system/ipc/rpc_client.hpp>
#include <asterion/system/polled_task.hpp>
#include <chrono>
#include <vector>
#include <optional>
#include <string>

namespace asterion::tasks {

struct ResearchDataRoute
{
    data::proto::DataOwner owner;
    std::string endpoint;
    std::string research_node_id;
};

struct ResearchProgram
{
    resource::proto::Owner owner;
    std::string artifact;
};

ResearchProgram
resolve_research_program(const node::proto::Status&, const std::string& service, std::uint64_t pid);

// Resolve a task's logical source against one authoritative Agent observation.
// The locator is transient; only owner belongs in persisted input provenance.
ResearchDataRoute resolve_research_data(const node::proto::Status&, const data::proto::DataOwner&);

data::proto::DataRetention research_retention(
    const task::proto::TaskExecution&,
    const ResearchDataRoute&,
    const std::string& service,
    const std::string& id);
void save_research_retention(
    const std::filesystem::path& task_directory,
    const data::proto::DataRetention&);
void verify_research_retention(
    const std::filesystem::path& task_directory,
    const data::proto::DataRetention&);

// One Research I/O owner pumps discovery and a bounded set of direct Data reads.
// Manager/Agent carry metadata only; business payloads use the resolved endpoint.
class ResearchData
{
public:
    explicit ResearchData(std::string discovery_endpoint);
    PolledTask<ResearchDataRoute>
    resolve(data::proto::DataOwner, std::chrono::milliseconds timeout, std::string parent);
    PolledTask<Payload>
    read(const ResearchDataRoute&, std::string request, std::chrono::milliseconds timeout);
    PolledTask<ResearchProgram> program(
        std::string service,
        std::uint64_t pid,
        std::chrono::milliseconds timeout,
        std::string parent);
    void poll();

private:
    PolledTask<node::proto::Status> observe(std::chrono::milliseconds timeout, std::string parent);
    PolledTask<Payload>
    read_endpoint(std::string endpoint, std::string request, std::chrono::milliseconds timeout);
    ipc::RpcClient discovery_;
    PayloadBudget payloads_{128 * 1024 * 1024};
    std::vector<std::weak_ptr<ipc::RpcClient>> readers_;
};

} // namespace asterion::tasks
