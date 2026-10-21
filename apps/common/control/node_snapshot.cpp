#include "node_snapshot.hpp"
#include <asterion/protocol/health.hpp>
#include <stdexcept>

namespace asterion::terminal {
namespace wire = node::proto;

const char* service_kind_name(wire::ServiceKind kind)
{
    switch (kind) {
        case wire::LIVE_TRADING:
            return "live";
        case wire::MARKET_DATA:
            return "market";
        case wire::DATA_SERVICE:
            return "data";
        case wire::TASK_SERVICE:
            return "task";
        default:
            return "unsupported";
    }
}

wire::ServiceKind parse_service_kind(const std::string& name)
{
    if (name == "live")
        return wire::LIVE_TRADING;
    if (name == "market")
        return wire::MARKET_DATA;
    if (name == "data")
        return wire::DATA_SERVICE;
    if (name == "task")
        return wire::TASK_SERVICE;
    throw std::invalid_argument("invalid service kind");
}

Json node_snapshot_json(const NodeSnapshot& snapshot)
{
    Json health = nullptr;
    if (snapshot.health) {
        const auto& status = *snapshot.health;
        const char* phase;
        switch (status.phase()) {
            case wire::Status::INITIALIZING:
                phase = "initializing";
                break;
            case wire::Status::READY:
                phase = "ready";
                break;
            case wire::Status::RECOVERY_REQUIRED:
                phase = "recovery_required";
                break;
            default:
                throw Error(ErrorCode::unavailable, "invalid node health");
        }
        Json services = Json::array();
        for (const auto& s : status.services())
            services.push_back(
                {{"id", s.id()},
                    {"kind", service_kind_name(s.kind())},
                    {"task_service", s.task_service()},
                    {"data_service", s.data_service()},
                    {"artifact", s.artifact()},
                    {"revision", s.revision()},
                    {"plugin_artifacts",
                        std::vector<std::string>(
                            s.plugin_artifacts().begin(),
                            s.plugin_artifacts().end())},
                    {"port", s.port()},
                    {"state", s.state()},
                    {"desired_running", s.desired_running()},
                    {"pid", s.pid()},
                    {"restarts", s.restarts()},
                    {"error", s.error()},
                    {"health", s.health()},
                    {"execution",
                        s.has_execution() ? protocol::execution_health_json(s.execution())
                                          : Json(nullptr)},
                    {"last_heartbeat_ms", s.last_heartbeat_ms()},
                    {"endpoint", s.endpoint()},
                    {"directory", s.directory()}});
        Json host_resources = nullptr;
        if (status.has_host_resources()) {
            const auto& sample = status.host_resources();
            host_resources = {
                {"sampled_at_ms", sample.sampled_at_ms()},
                {"cpu_usage_percent",
                    sample.has_cpu_usage_percent() ? Json(sample.cpu_usage_percent())
                                                   : Json(nullptr)},
                {"memory_available_bytes", sample.memory_available_bytes()},
                {"disk_available_bytes", sample.disk_available_bytes()},
                {"disk_path", sample.disk_path()},
                {"failure",
                    sample.has_failure()
                        ? Json{{"code", sample.failure().code()}, {"message", sample.failure().message()}}
                        : Json(nullptr)}
            };
        }
        health = {
            {"instance_id", status.instance_id()},
            {"host_resources", std::move(host_resources)},
            {"phase", phase},
            {"failure",
                status.has_failure()
                    ? Json{{"code", status.failure().code()}, {"message", status.failure().message()}}
                    : Json(nullptr)},
            {"execution", protocol::execution_health_json(status.execution())},
            {"maintenance", status.maintenance()},
            {"pid", status.pid()},
            {"os", status.os()},
            {"arch", status.arch()},
            {"version", status.version()},
            {"uptime_ms", status.uptime_ms()},
            {"services", services}
        };
    }
    return {
        {"id", snapshot.id},
        {"host", snapshot.host},
        {"port", snapshot.port},
        {"state", snapshot.online ? "online" : "unreachable"},
        {"last_heartbeat_ms", snapshot.last_heartbeat_ms},
        {"latency_ms", snapshot.latency_ms},
        {"error", snapshot.error},
        {"health", health}
    };
}

} // namespace asterion::terminal
