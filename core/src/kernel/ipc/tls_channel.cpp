#include "timed_operation.hpp"
#include <array>
#include <asio.hpp>
#include <asio/ssl.hpp>
#include <asterion/foundation/error.hpp>
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/ipc/tls_channel.hpp>
#include <filesystem>
#include <fstream>
namespace asterion::ipc {
namespace {
using Tcp = asio::ip::tcp;
using Clock = std::chrono::steady_clock;
using Ms = std::chrono::milliseconds;
std::string pem(const std::string &filename) {
  const std::filesystem::path path(
      std::u8string(filename.begin(), filename.end()));
  if (!path.is_absolute() || !std::filesystem::is_regular_file(path) ||
      std::filesystem::file_size(path) > 1024 * 1024)
    throw std::invalid_argument(
        "TLS requires absolute paths to PEM files (maximum 1 MiB)");
  std::ifstream file(path, std::ios::binary);
  if (!file)
    throw std::invalid_argument("cannot read TLS identity file");
  return {std::istreambuf_iterator<char>(file), {}};
}
std::shared_ptr<asio::ssl::context> context(const TlsIdentity &identity,
                                            bool server) {
  auto ctx = std::make_shared<asio::ssl::context>(asio::ssl::context::tls);
  if (SSL_CTX_set_min_proto_version(ctx->native_handle(), TLS1_3_VERSION) != 1)
    throw std::runtime_error("TLS 1.3 unavailable");
  ctx->set_verify_mode(asio::ssl::verify_peer |
                       (server ? asio::ssl::verify_fail_if_no_peer_cert : 0));
  ctx->set_password_callback(
      [](std::size_t, asio::ssl::context::password_purpose) {
        return std::string{};
      });
  ctx->add_certificate_authority(asio::buffer(pem(identity.ca_file)));
  ctx->use_certificate_chain(asio::buffer(pem(identity.certificate_file)));
  ctx->use_private_key(asio::buffer(pem(identity.private_key_file)),
                       asio::ssl::context::pem);
  if (SSL_CTX_check_private_key(ctx->native_handle()) != 1)
    throw std::invalid_argument("TLS certificate/key mismatch");
  SSL_CTX_set_session_cache_mode(ctx->native_handle(), SSL_SESS_CACHE_OFF);
  SSL_CTX_set_num_tickets(ctx->native_handle(), 0);
  return ctx;
}
using detail::run;
Ms remaining(Clock::time_point end) {
  return std::max(Ms{0}, std::chrono::duration_cast<Ms>(end - Clock::now()));
}
} // namespace
struct TlsChannel::Impl {
  std::shared_ptr<asio::io_context> io;
  std::shared_ptr<asio::ssl::context> ctx;
  asio::ssl::stream<Tcp::socket> stream;
  Impl(std::shared_ptr<asio::io_context> i,
       std::shared_ptr<asio::ssl::context> c)
      : io(std::move(i)), ctx(std::move(c)), stream(*io, *ctx) {}
  void cancel() {
    asio::error_code ignored;
    stream.lowest_layer().close(ignored);
  }
  template <class Start> void operation(Ms timeout, Start start) {
    run(*io, timeout, start, [&] { cancel(); });
  }
  void read(void *data, std::size_t size, Ms timeout) {
    operation(timeout, [&](auto done) {
      asio::async_read(stream, asio::buffer(data, size), done);
    });
  }
};
TlsChannel::TlsChannel() = default;
TlsChannel::~TlsChannel() = default;
TlsChannel::TlsChannel(TlsChannel &&) noexcept = default;
TlsChannel &TlsChannel::operator=(TlsChannel &&) noexcept = default;
std::string TlsChannel::peer_address() const {
  if (!impl_)
    throw std::logic_error("closed TLS channel");
  return impl_->stream.lowest_layer().remote_endpoint().address().to_string();
}
void TlsChannel::close() noexcept { impl_.reset(); }
TlsChannel TlsChannel::connect(const std::string &host, std::uint16_t port,
                               const TlsIdentity &identity, Ms timeout) {
  if (host.empty() || host.find('\0') != std::string::npos || !port ||
      timeout.count() <= 0)
    throw std::invalid_argument(
        "TCP host, port and positive timeout are required");
  TlsChannel channel;
  channel.impl_ = std::make_unique<Impl>(std::make_shared<asio::io_context>(),
                                         context(identity, false));
  auto &p = *channel.impl_;
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
  p.operation(remaining(end), [&](auto done) {
    asio::async_connect(p.stream.next_layer(), addresses, done);
  });
  p.operation(remaining(end), [&](auto done) {
    p.stream.async_handshake(asio::ssl::stream_base::client, done);
  });
  return channel;
}
void TlsChannel::send(const std::string &payload, Ms timeout) {
  if (!impl_)
    throw Error(ErrorCode::unavailable, "TCP connection is closed");
  if (payload.empty() || payload.size() > Channel::max_frame)
    throw std::invalid_argument("invalid frame size");
  const auto size = static_cast<std::uint32_t>(payload.size());
  std::array<unsigned char, 4> header{static_cast<unsigned char>(size >> 24),
                                      static_cast<unsigned char>(size >> 16),
                                      static_cast<unsigned char>(size >> 8),
                                      static_cast<unsigned char>(size)};
  std::array<asio::const_buffer, 2> buffers{asio::buffer(header),
                                            asio::buffer(payload)};
  try {
    impl_->operation(timeout, [&](auto done) {
      asio::async_write(impl_->stream, buffers, done);
    });
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
    const auto size = (std::uint32_t(header[0]) << 24) |
                      (std::uint32_t(header[1]) << 16) |
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
  Impl(const std::string &address, std::uint16_t port,
       const TlsIdentity &identity)
      : ctx(context(identity, true)), acceptor(*io) {
    const Tcp::endpoint endpoint(asio::ip::make_address(address), port);
    acceptor.open(endpoint.protocol());
#ifdef _WIN32
    const int enabled = 1;
    if (::setsockopt(acceptor.native_handle(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                     reinterpret_cast<const char *>(&enabled),
                     static_cast<int>(sizeof(enabled))) != 0)
      throw Error(ErrorCode::unavailable,
                  "cannot acquire exclusive TCP address");
#else
    acceptor.set_option(Tcp::acceptor::reuse_address(true));
#endif
    acceptor.bind(endpoint);
    acceptor.listen();
  }
};
TlsListener::TlsListener(const std::string &address, std::uint16_t port,
                         const TlsIdentity &identity)
    : impl_(std::make_unique<Impl>(address, port, identity)) {}
TlsListener::~TlsListener() = default;
TlsPendingConnection::TlsPendingConnection() = default;
TlsPendingConnection::~TlsPendingConnection() = default;
TlsPendingConnection::TlsPendingConnection(TlsPendingConnection &&) noexcept =
    default;
TlsPendingConnection &
TlsPendingConnection::operator=(TlsPendingConnection &&) noexcept = default;
TlsChannel TlsPendingConnection::handshake(Ms timeout) && {
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
TlsPendingConnection TlsListener::accept_pending(Ms wait_timeout) {
  TlsPendingConnection pending;
  pending.impl_ = std::make_unique<TlsChannel::Impl>(
      std::make_shared<asio::io_context>(), impl_->ctx);
  run(
      *impl_->io, wait_timeout,
      [&](auto done) {
        impl_->acceptor.async_accept(pending.impl_->stream.next_layer(), done);
      },
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
