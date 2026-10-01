#include "node_snapshot.hpp"
#include <stdexcept>
namespace asterion::terminal {
namespace wire = node::v1;
const char* service_kind_name(wire::ServiceKind kind) {
  switch (kind) {
  case wire::PAPER_TRADING:
    return "paper";
  case wire::LIVE_TRADING:
    return "live";
  case wire::MARKET_DATA:
    return "market";
  case wire::TASK_SERVICE:
    return "research";
  case wire::STRATEGY:
    return "strategy";
  default:
    return "unsupported";
  }
}
wire::ServiceKind parse_service_kind(const std::string& name) {
  if (name == "paper")
    return wire::PAPER_TRADING;
  if (name == "live")
    return wire::LIVE_TRADING;
  if (name == "market")
    return wire::MARKET_DATA;
  if (name == "research")
    return wire::TASK_SERVICE;
  if (name == "strategy")
    return wire::STRATEGY;
  throw std::invalid_argument("invalid service kind");
}
Json node_snapshot_json(const NodeSnapshot& snapshot) {
  Json health = nullptr;
  if (snapshot.health) {
    const auto& status = *snapshot.health;
    Json services = Json::array();
    for (const auto& s : status.services())
      services.push_back(
          {{"id", s.id()},
           {"kind", service_kind_name(s.kind())},
           {"active_workers", s.active_workers()},
           {"artifact", s.artifact()},
           {"revision", s.revision()},
           {"plugin_artifacts",
            std::vector<std::string>(s.plugin_artifacts().begin(), s.plugin_artifacts().end())},
           {"port", s.port()},
           {"state", s.state()},
           {"desired_running", s.desired_running()},
           {"pid", s.pid()},
           {"restarts", s.restarts()},
           {"error", s.error()},
           {"health", s.health()},
           {"last_heartbeat_ms", s.last_heartbeat_ms()},
           {"endpoint", s.endpoint()},
           {"directory", s.directory()}});
    health = {{"instance_id", status.instance_id()},
              {"maintenance", status.maintenance()},
              {"pid", status.pid()},
              {"os", status.os()},
              {"arch", status.arch()},
              {"version", status.version()},
              {"uptime_ms", status.uptime_ms()},
              {"services", services}};
  }
  return {{"id", snapshot.id},
          {"host", snapshot.host},
          {"port", snapshot.port},
          {"state", snapshot.online ? "online" : "unreachable"},
          {"last_heartbeat_ms", snapshot.last_heartbeat_ms},
          {"latency_ms", snapshot.latency_ms},
          {"error", snapshot.error},
          {"health", health}};
}
} // namespace asterion::terminal
