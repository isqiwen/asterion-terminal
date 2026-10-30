#pragma once
#include <cstdint>
#include <string>

namespace asterion::terminal {

struct ServiceEndpoint
{
    std::string host, session;
    std::uint16_t port;
    std::string endpoint{};
    std::string node_id{"local"}, owner_node_id{};
};

inline bool same_service_endpoint(const ServiceEndpoint& a, const ServiceEndpoint& b)
{
    if (!a.endpoint.empty() || !b.endpoint.empty())
        return !a.endpoint.empty() && a.endpoint == b.endpoint && a.session == b.session;
    return a.host == b.host && a.port == b.port && a.session == b.session
        && a.endpoint == b.endpoint;
}

} // namespace asterion::terminal
