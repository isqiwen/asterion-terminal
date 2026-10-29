#include "startup.hpp"
#include <asterion/foundation/error.hpp>
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/log.hpp>
#include <asterion/manager.pb.h>
#include <algorithm>
#include <thread>

namespace asterion::installation {

using namespace std::chrono_literals;

void wait_for_local_readiness(
    const std::string& manager_endpoint,
    const std::filesystem::path& root,
    std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    auto remaining = [&] {
        return std::max(
            0ms,
            std::chrono::ceil<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()));
    };
    auto io_timeout = [&] {
        return std::min(1000ms, remaining());
    };
    std::string reason = "Manager has not responded";
    std::uint64_t manager_pid = 0, agent_pid = 0;
    auto evidence = [&] {
        auto detail = reason + "; logs in " + root.string();
        if (manager_pid)
            detail += "; Manager: "
                + log::file(root / "manager", "asterion_node_manager", manager_pid).string();
        if (agent_pid)
            detail +=
                "; Agent: " + log::file(root / "agent", "asterion_node_agent", agent_pid).string();
        return detail;
    };
    while (remaining() > 0ms) {
        manager::proto::Request request;
        request.set_version(1);
        request.set_correlation_id("installer-readiness");
        request.mutable_snapshot();
        std::string payload;
        bool received = false;
        try {
            auto channel = ipc::Channel::connect(manager_endpoint, io_timeout());
            channel.send(request.SerializeAsString(), io_timeout());
            payload = channel.receive(io_timeout());
            received = true;
        } catch (const Error& error) {
            if (error.code() != ErrorCode::unavailable)
                throw;
            // A newly started process may not have bound its socket yet. Only
            // this read-only observation is retried, within the startup deadline.
            reason = std::string("Manager: ") + error.what();
        }
        if (received) {
            manager::proto::Response response;
            if (!response.ParseFromString(payload) || response.version() != 1
                || response.correlation_id() != request.correlation_id()
                || (!response.has_snapshot() && !response.has_error()))
                throw Error(ErrorCode::invalid_request, "invalid Manager readiness response");
            if (response.has_error())
                throw_remote_error(response.error().code(), response.error().message());
            manager_pid = response.snapshot().pid();
            reason = "Manager is initializing the local node";
            for (const auto& node : response.snapshot().nodes()) {
                if (node.id() != "local")
                    continue;
                agent_pid = node.health().pid();
                if (!node.online()) {
                    reason = "local Agent is offline: " + node.error();
                    break;
                }
                if (node.health().phase() == node::proto::Status::READY)
                    return;
                if (node.health().phase() == node::proto::Status::RECOVERY_REQUIRED) {
                    reason = node.health().failure().message();
                    throw Error(
                        ErrorCode::recovery_required,
                        "local Agent startup failed: " + evidence());
                }
                reason = "local Agent is initializing";
                break;
            }
        }
        std::this_thread::sleep_for(std::min(100ms, remaining()));
    }
    throw Error(ErrorCode::unavailable, "management startup timed out: " + evidence());
}

} // namespace asterion::installation
