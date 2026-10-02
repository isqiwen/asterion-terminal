#pragma once
#include <asterion/kernel/ipc/tls_channel.hpp>
namespace asterion::terminal {
struct ServiceEndpoint {
  std::string host, session;
  std::uint16_t port;
  ipc::TlsIdentity tls;
  std::string endpoint{};
};
inline bool same_service_endpoint(const ServiceEndpoint& a, const ServiceEndpoint& b) {
  if (!a.endpoint.empty() || !b.endpoint.empty())
    return !a.endpoint.empty() && a.endpoint == b.endpoint && a.session == b.session;
  return a.host == b.host && a.port == b.port && a.session == b.session &&
         a.endpoint == b.endpoint && a.tls.ca_file == b.tls.ca_file &&
         a.tls.certificate_file == b.tls.certificate_file &&
         a.tls.private_key_file == b.tls.private_key_file;
}
} // namespace asterion::terminal
