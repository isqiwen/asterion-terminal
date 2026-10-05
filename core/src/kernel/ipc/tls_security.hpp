#pragma once
#include <asterion/kernel/ipc/tls_channel.hpp>
#include <asio/ssl.hpp>
namespace asterion::ipc::detail {
std::shared_ptr<asio::ssl::context> tls_context(const TlsIdentity& identity, bool server);
PeerRole tls_peer_role(SSL* stream);
} // namespace asterion::ipc::detail
