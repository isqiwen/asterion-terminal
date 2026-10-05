#include <algorithm>
#include <vector>
#include "timed_operation.hpp"
#include "tls_security.hpp"
#include <array>
#include <asio.hpp>
#include <asio/ssl.hpp>
#include <asterion/foundation/error.hpp>
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/ipc/tls_channel.hpp>
#include <filesystem>
#include <fstream>
#include <stdexcept>
namespace asterion::ipc {
namespace {
using Tcp = asio::ip::tcp;
using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;
std::string pem(const std::string& filename) {
  const std::filesystem::path path(std::u8string(filename.begin(), filename.end()));
  if (!path.is_absolute() || !std::filesystem::is_regular_file(path) ||
      std::filesystem::file_size(path) > 1024 * 1024)
    throw std::invalid_argument("TLS requires absolute paths to PEM files (maximum 1 MiB)");
  std::ifstream file(path, std::ios::binary);
  if (!file)
    throw std::invalid_argument("cannot read TLS identity file");
  return {std::istreambuf_iterator<char>(file), {}};
}
using detail::run;
Ms remaining(Clock::time_point end) {
  return std::max(Ms{0}, std::chrono::duration_cast<Ms>(end - Clock::now()));
}
} // namespace
namespace detail {
std::shared_ptr<asio::ssl::context> tls_context(const TlsIdentity& identity, bool server) {
  auto ctx = std::make_shared<asio::ssl::context>(asio::ssl::context::tls);
  if (SSL_CTX_set_min_proto_version(ctx->native_handle(), TLS1_3_VERSION) != 1)
    throw std::runtime_error("TLS 1.3 unavailable");
  ctx->set_verify_mode(asio::ssl::verify_peer |
                       (server ? asio::ssl::verify_fail_if_no_peer_cert : 0));
  ctx->set_password_callback(
      [](std::size_t, asio::ssl::context::password_purpose) { return std::string{}; });
  ctx->add_certificate_authority(asio::buffer(pem(identity.ca_file)));
  ctx->use_certificate_chain(asio::buffer(pem(identity.certificate_file)));
  ctx->use_private_key(asio::buffer(pem(identity.private_key_file)), asio::ssl::context::pem);
  if (SSL_CTX_check_private_key(ctx->native_handle()) != 1)
    throw std::invalid_argument("TLS certificate/key mismatch");
  SSL_CTX_set_session_cache_mode(ctx->native_handle(), SSL_SESS_CACHE_OFF);
  SSL_CTX_set_num_tickets(ctx->native_handle(), 0);
  return ctx;
}
PeerRole tls_peer_role(SSL* stream) {
  // The handshake already verified this certificate against the node CA.
  std::unique_ptr<X509, decltype(&X509_free)> peer(SSL_get1_peer_certificate(stream), X509_free);
  if (!peer)
    return PeerRole::unknown;
  const auto* subject = X509_get_subject_name(peer.get());
  PeerRole found = PeerRole::unknown;
  for (int index = X509_NAME_get_index_by_NID(subject, NID_organizationalUnitName, -1); index >= 0;
       index = X509_NAME_get_index_by_NID(subject, NID_organizationalUnitName, index)) {
    const auto* data = X509_NAME_ENTRY_get_data(X509_NAME_get_entry(subject, index));
    const std::string_view value(reinterpret_cast<const char*>(ASN1_STRING_get0_data(data)),
                                 static_cast<std::size_t>(ASN1_STRING_length(data)));
    PeerRole role = PeerRole::unknown;
    for (const auto candidate : {PeerRole::admin, PeerRole::client, PeerRole::service})
      if (value == role_subject(candidate))
        role = candidate;
    // Exactly one recognised role; several conflicting ones grant nothing.
    if (role != PeerRole::unknown) {
      if (found != PeerRole::unknown && found != role)
        return PeerRole::unknown;
      found = role;
    }
  }
  return found;
}
} // namespace detail
struct TlsChannel::Impl {
  std::shared_ptr<asio::io_context> io;
  std::shared_ptr<asio::ssl::context> ctx;
  asio::ssl::stream<Tcp::socket> stream;
  Impl(std::shared_ptr<asio::io_context> i, std::shared_ptr<asio::ssl::context> c)
      : io(std::move(i)), ctx(std::move(c)), stream(*io, *ctx) {}
  void cancel() {
    asio::error_code ignored;
    stream.lowest_layer().close(ignored);
  }
  template <class Start> void operation(Ms timeout, Start start) {
    run(*io, timeout, start, [&] { cancel(); });
  }
  void read(void* data, std::size_t size, Ms timeout) {
    operation(timeout,
              [&](auto done) { asio::async_read(stream, asio::buffer(data, size), done); });
  }
};
TlsChannel::TlsChannel() = default;
TlsChannel::~TlsChannel() = default;
TlsChannel::TlsChannel(TlsChannel&&) noexcept = default;
TlsChannel& TlsChannel::operator=(TlsChannel&&) noexcept = default;
std::string TlsChannel::peer_address() const {
  if (!impl_)
    throw std::logic_error("closed TLS channel");
  return impl_->stream.lowest_layer().remote_endpoint().address().to_string();
}
std::string_view role_name(PeerRole role) noexcept {
  switch (role) {
  case PeerRole::admin:
    return "admin";
  case PeerRole::client:
    return "client";
  case PeerRole::service:
    return "service";
  case PeerRole::local:
    return "local";
  case PeerRole::unknown:
    break;
  }
  return "unknown";
}
std::string role_subject(PeerRole role) {
  if (role == PeerRole::unknown || role == PeerRole::local)
    throw std::invalid_argument("only admin, client and service roles are issued");
  return "asterion:" + std::string(role_name(role));
}
PeerRole TlsChannel::peer_role() const {
  if (!impl_)
    throw std::logic_error("closed TLS channel");
  return detail::tls_peer_role(impl_->stream.native_handle());
}

void TlsChannel::close() noexcept {
  impl_.reset();
}
TlsChannel TlsChannel::connect(const std::string& host, std::uint16_t port,
                               const TlsIdentity& identity, Ms timeout) {
  if (host.empty() || host.find('\0') != std::string::npos || !port || timeout.count() <= 0)
    throw std::invalid_argument("TCP host, port and positive timeout are required");
  TlsChannel channel;
  channel.impl_ = std::make_unique<Impl>(std::make_shared<asio::io_context>(),
                                         detail::tls_context(identity, false));
  auto& p = *channel.impl_;
  if (SSL_set1_host(p.stream.native_handle(), host.c_str()) != 1 ||
      SSL_set_tlsext_host_name(p.stream.native_handle(), host.c_str()) != 1)
    throw std::invalid_argument("invalid TLS server identity");
  const auto end = Clock::now() + timeout;
  Tcp::resolver resolver(*p.io);
  Tcp::resolver::results_type addresses;
  run(
      *p.io, remaining(end),
      [&](auto done) {
        resolver.async_resolve(host, std::to_string(port),
                               [&, done](asio::error_code ec, auto values) {
                                 addresses = std::move(values);
                                 done(ec);
                               });
      },
      [&] { resolver.cancel(); });
  // IPv4 first: services bind IPv4 addresses by default, and "localhost" often
  // resolves to ::1 first.
  std::vector<Tcp::endpoint> endpoints;
  for (const auto& entry : addresses)
    endpoints.push_back(entry.endpoint());
  std::stable_partition(endpoints.begin(), endpoints.end(),
                        [](const Tcp::endpoint& e) { return e.address().is_v4(); });
  p.operation(remaining(end), [&](auto done) {
    asio::async_connect(p.stream.next_layer(), endpoints,
                        [done](asio::error_code ec, const Tcp::endpoint&) { done(ec); });
  });
  p.operation(remaining(end),
              [&](auto done) { p.stream.async_handshake(asio::ssl::stream_base::client, done); });
  return channel;
}
void TlsChannel::send(const std::string& payload, Ms timeout) {
  if (!impl_)
    throw Error(ErrorCode::unavailable, "TCP connection is closed");
  if (payload.empty() || payload.size() > Channel::max_frame)
    throw std::invalid_argument("invalid frame size");
  const auto size = static_cast<std::uint32_t>(payload.size());
  std::array<unsigned char, 4> header{
      static_cast<unsigned char>(size >> 24), static_cast<unsigned char>(size >> 16),
      static_cast<unsigned char>(size >> 8), static_cast<unsigned char>(size)};
  std::array<asio::const_buffer, 2> buffers{asio::buffer(header), asio::buffer(payload)};
  try {
    impl_->operation(timeout, [&](auto done) { asio::async_write(impl_->stream, buffers, done); });
  } catch (...) {
    close();
    throw;
  }
}
std::string TlsChannel::receive(Ms timeout) {
  if (!impl_)
    throw Error(ErrorCode::unavailable, "TCP connection is closed");
  try {
    std::array<unsigned char, 4> header{};
    auto end = Clock::now() + timeout;
    impl_->read(header.data(), 1, timeout);
    const auto frame_end = Clock::now() + std::chrono::seconds(10);
    end = timeout.count() < 0 ? frame_end : std::min(end, frame_end);
    impl_->read(header.data() + 1, 3, remaining(end));
    const auto size = (std::uint32_t(header[0]) << 24) | (std::uint32_t(header[1]) << 16) |
                      (std::uint32_t(header[2]) << 8) | header[3];
    if (!size || size > Channel::max_frame)
      throw Error(ErrorCode::resource_exhausted, "invalid TCP frame size");
    std::string payload(size, '\0');
    impl_->read(payload.data(), size, remaining(end));
    return payload;
  } catch (...) {
    close();
    throw;
  }
}
struct TlsListener::Impl {
  std::shared_ptr<asio::io_context> io = std::make_shared<asio::io_context>();
  std::shared_ptr<asio::ssl::context> ctx;
  Tcp::acceptor acceptor;
  Impl(const std::string& address, std::uint16_t port, const TlsIdentity& identity)
      : ctx(detail::tls_context(identity, true)), acceptor(*io) {
    const Tcp::endpoint endpoint(asio::ip::make_address(address), port);
    acceptor.open(endpoint.protocol());
    acceptor.set_option(Tcp::acceptor::reuse_address(true));
    acceptor.bind(endpoint);
    acceptor.listen();
  }
};
TlsListener::TlsListener(const std::string& address, std::uint16_t port,
                         const TlsIdentity& identity)
    : impl_(std::make_unique<Impl>(address, port, identity)) {}
TlsListener::~TlsListener() = default;
struct TlsPendingConnection::Handshake {
  Clock::time_point deadline;
  asio::error_code error;
  bool complete = false;
};
TlsPendingConnection::TlsPendingConnection() = default;
TlsPendingConnection::~TlsPendingConnection() = default;
TlsPendingConnection::TlsPendingConnection(TlsPendingConnection&&) noexcept = default;
TlsPendingConnection& TlsPendingConnection::operator=(TlsPendingConnection&&) noexcept = default;
TlsChannel TlsPendingConnection::handshake(Ms timeout) && {
  if (handshake_)
    throw std::logic_error("TLS handshake is already pending");
  if (!impl_)
    throw Error(ErrorCode::unavailable, "pending TLS connection is closed");
  if (timeout.count() <= 0)
    throw std::invalid_argument("positive TLS handshake timeout required");
  TlsChannel channel;
  channel.impl_ = std::move(impl_);
  channel.impl_->operation(timeout, [&](auto done) {
    channel.impl_->stream.async_handshake(asio::ssl::stream_base::server, done);
  });
  return channel;
}
void TlsPendingConnection::start_handshake(Ms timeout) {
  if (!impl_ || handshake_)
    throw std::logic_error("TLS handshake requires an unused pending connection");
  if (timeout.count() <= 0)
    throw std::invalid_argument("positive TLS handshake timeout required");
  handshake_ = std::make_shared<Handshake>();
  handshake_->deadline = Clock::now() + timeout;
  impl_->stream.async_handshake(asio::ssl::stream_base::server,
                                [state = handshake_](asio::error_code error) {
                                  state->error = error;
                                  state->complete = true;
                                });
}
std::optional<TlsChannel> TlsPendingConnection::poll_handshake() {
  if (!impl_ || !handshake_)
    throw std::logic_error("TLS handshake is not pending");
  if (Clock::now() >= handshake_->deadline) {
    impl_->cancel();
    throw Error(ErrorCode::unavailable, "TLS handshake timed out");
  }
  impl_->io->restart();
  impl_->io->poll();
  if (!handshake_->complete)
    return std::nullopt;
  if (handshake_->error) {
    impl_->cancel();
    throw Error(ErrorCode::unavailable, "TLS handshake failed");
  }
  TlsChannel channel;
  channel.impl_ = std::move(impl_);
  handshake_.reset();
  return channel;
}
TlsPendingConnection TlsListener::accept_pending(Ms wait_timeout) {
  TlsPendingConnection pending;
  pending.impl_ =
      std::make_unique<TlsChannel::Impl>(std::make_shared<asio::io_context>(), impl_->ctx);
  run(
      *impl_->io, wait_timeout,
      [&](auto done) { impl_->acceptor.async_accept(pending.impl_->stream.next_layer(), done); },
      [&] {
        asio::error_code ignored;
        impl_->acceptor.cancel(ignored);
      },
      detail::DeadlinePolicy::preserve_accepted);
  return pending;
}
TlsChannel TlsListener::accept(Ms timeout, Ms wait_timeout) {
  if (timeout.count() <= 0)
    throw std::invalid_argument("positive TLS handshake timeout required");
  return accept_pending(wait_timeout).handshake(timeout);
}
} // namespace asterion::ipc
