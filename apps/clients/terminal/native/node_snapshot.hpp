#pragma once
#include <asterion/foundation/serialization.hpp>
#include <asterion/node.pb.h>
#include <memory>

namespace asterion::terminal {

struct NodeSnapshot
{
    std::string id, host;
    std::uint16_t port = 0;
    bool online = false;
    std::int64_t last_heartbeat_ms = 0, latency_ms = 0;
    std::string error;
    std::shared_ptr<const node::proto::Status> health;
    bool operator==(const NodeSnapshot&) const = default;
};

const char* service_kind_name(node::proto::ServiceKind kind);
node::proto::ServiceKind parse_service_kind(const std::string& name);
Json node_snapshot_json(const NodeSnapshot& snapshot);

} // namespace asterion::terminal
