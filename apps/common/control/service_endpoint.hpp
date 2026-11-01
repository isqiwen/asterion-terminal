#pragma once
#include <cstdint>
#include <asterion/system/ipc/tcp_channel.hpp>
#include <string>

namespace asterion::terminal {

struct ServiceEndpoint
{
    std::string host, session;
    std::uint16_t port;
    std::string endpoint{};
    std::string node_id{"local"}, owner_node_id{};
    ipc::TlsIdentity tls{};
};

inline bool same_service_endpoint(const ServiceEndpoint& a, const ServiceEndpoint& b)
{
    if (!a.endpoint.empty() || !b.endpoint.empty())
        return !a.endpoint.empty() && a.endpoint == b.endpoint && a.session == b.session;
    return a.host == b.host && a.port == b.port && a.session == b.session
        && a.endpoint == b.endpoint && a.tls.certificate_file == b.tls.certificate_file
        && a.tls.private_key_file == b.tls.private_key_file && a.tls.peers == b.tls.peers;
}

} // namespace asterion::terminal
