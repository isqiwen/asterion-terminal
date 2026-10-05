#pragma once
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
namespace asterion::ipc {
struct TlsIdentity {
  std::string ca_file, certificate_file, private_key_file;
};
// Authorization role of a certificate, carried in its subject as
// OU=asterion:<role>. The node CA issues all roles at enrollment and discards
// its key, so roles cannot be forged without that key.
//   admin   - deploy, upload and run programs, maintenance, firewall
//   client  - business calls only (trading, market data, task, strategy)
//   service - calls between services
// local marks same-user IPC peers; unknown means no recognised role.
enum class PeerRole { unknown, admin, client, service, local };
std::string_view role_name(PeerRole role) noexcept;
// The subject value for a role, e.g. "asterion:admin".
std::string role_subject(PeerRole role);
// Single-owner framed TCP client stream. Mutual TLS is mandatory; no plaintext
// mode. Services accept TLS peers through RpcHost.
// Distinct channels have independent event loops and may be used concurrently.
// Move ownership between threads; never operate on one channel concurrently.
class TlsChannel {
public:
  TlsChannel();
  ~TlsChannel();
  TlsChannel(TlsChannel&&) noexcept;
  TlsChannel& operator=(TlsChannel&&) noexcept;
  TlsChannel(const TlsChannel&) = delete;
  TlsChannel& operator=(const TlsChannel&) = delete;
  static TlsChannel connect(const std::string& host, std::uint16_t port,
                            const TlsIdentity& identity, std::chrono::milliseconds timeout);
  std::string receive(std::chrono::milliseconds timeout);
  void send(const std::string& payload, std::chrono::milliseconds timeout);
  void close() noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace asterion::ipc
