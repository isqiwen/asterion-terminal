#pragma once
#include <asterion/kernel/ipc/tcp_channel.hpp>
#include <asio/ssl.hpp>

namespace asterion::ipc::detail {

// TLS 1.3 with this side's certificate and key, accepting only a peer whose
// certificate has one of the identity's fingerprints.
std::shared_ptr<asio::ssl::context> tls_context(const TlsIdentity& identity);
// Throws std::invalid_argument unless the identity is complete and valid.
void validate_identity(const TlsIdentity& identity);

} // namespace asterion::ipc::detail
