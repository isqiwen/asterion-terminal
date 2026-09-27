#pragma once
#include <asterion/kernel/ipc/tls_channel.hpp>
namespace asterion::terminal {
struct ServiceEndpoint {
  std::string host, session;
  std::uint16_t port;
  ipc::TlsIdentity tls;
  std::string endpoint{};
};
} // namespace asterion::terminal
