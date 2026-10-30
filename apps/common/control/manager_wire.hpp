#pragma once
#include "managed_service.hpp"
#include "node_snapshot.hpp"
#include <asterion/manager.pb.h>

namespace asterion::terminal {

inline void encode(manager::proto::Endpoint& out, const ServiceEndpoint& in)
{
    out.set_host(in.host);
    out.set_port(in.port);
    out.set_session(in.session);
    out.set_socket(in.endpoint);
    out.set_owner_node_id(in.owner_node_id);
}

inline ServiceEndpoint decode(const manager::proto::Endpoint& in)
{
    return {
        in.host(),
        in.session(),
        static_cast<std::uint16_t>(in.port()),
        in.socket(),
        "local",
        in.owner_node_id()
    };
}

inline void encode(manager::proto::Node& out, const NodeSnapshot& in)
{
    out.set_id(in.id);
    out.set_host(in.host);
    out.set_port(in.port);
    out.set_online(in.online);
    out.set_last_heartbeat_ms(in.last_heartbeat_ms);
    out.set_latency_ms(in.latency_ms);
    out.set_error(in.error);
    if (in.health)
        *out.mutable_health() = *in.health;
}

inline NodeSnapshot decode(const manager::proto::Node& in)
{
    return {
        in.id(),
        in.host(),
        static_cast<std::uint16_t>(in.port()),
        in.online(),
        in.last_heartbeat_ms(),
        in.latency_ms(),
        in.error(),
        in.has_health() ? std::make_shared<const node::proto::Status>(in.health()) : nullptr
    };
}

} // namespace asterion::terminal
