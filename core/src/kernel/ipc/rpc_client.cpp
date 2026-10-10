#include <asterion/kernel/ipc/rpc_client.hpp>
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/foundation/error.hpp>
#include "local_security.hpp"
#include "tls_security.hpp"
#include <asio.hpp>
#include <asio/ssl.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <vector>
#include <type_traits>
#include <utility>
namespace asterion::ipc {
using namespace std::chrono_literals;
using Local = asio::local::stream_protocol;
using Tcp = asio::ip::tcp;
using Tls = asio::ssl::stream<Tcp::socket>;
struct Reactor::Impl {
  asio::io_context io;
  // Set by wake() and taken by the next wait(): a wake-up is never lost to a
  // client that pumps the loop in between.
  std::atomic<bool> woken{false};
};
Reactor::Reactor() : impl_(std::make_unique<Impl>()) {}
Reactor::~Reactor() = default;
void Reactor::wait(std::optional<std::chrono::milliseconds> limit) {
  if (impl_->woken.exchange(false))
    return;
  auto& io = impl_->io;
  io.restart();
  // Without outstanding I/O the loop would return at once instead of waiting.
  const auto work = asio::make_work_guard(io);
  if (limit)
    io.run_one_for(*limit);
  else
    io.run_one();
}
void Reactor::wake() {
  // The posted handler only interrupts a wait that is already blocking.
  if (!impl_->woken.exchange(true))
    asio::post(impl_->io, [] {});
}
struct RpcClient::Impl {
  struct Pending {
    bool done = false;
    virtual ~Pending() = default;
    virtual void cancel() = 0;
    virtual void advance() = 0;
  };
  template <class Stream, class Result>
  struct Exchange final : Pending, std::enable_shared_from_this<Exchange<Stream, Result>> {
    Impl& client;
    Stream stream;
    asio::steady_timer timer;
    Tcp::resolver resolver;
    std::promise<Result> result;
    std::function<void(Payload)> frame;
    bool next_frame = false;
    Payload payload;
    std::array<unsigned char, 4> header{};
    const std::chrono::milliseconds reply_timeout;
    const std::chrono::milliseconds send_timeout;
    const std::chrono::steady_clock::time_point expires;
    Exchange(Impl& client, Stream stream, std::string payload,
             std::chrono::milliseconds reply_timeout, std::chrono::steady_clock::time_point expires,
             std::chrono::milliseconds send_timeout, std::function<void(Payload)> frame)
        : client(client), stream(std::move(stream)), timer(client.io), resolver(client.io),
          frame(std::move(frame)), payload(client.payloads.retain(std::move(payload))),
          reply_timeout(reply_timeout), send_timeout(send_timeout), expires(expires) {}
    void close() {
      asio::error_code ignored;
      resolver.cancel();
      timer.cancel();
      stream.lowest_layer().close(ignored);
    }
    void fail(std::exception_ptr error) {
      if (this->done)
        return;
      this->done = true;
      close();
      result.set_exception(std::move(error));
    }
    void cancel() override {
      fail(std::make_exception_ptr(
          Error(ErrorCode::unavailable, "RPC request cancelled; command outcome may be unknown")));
    }
    void advance() override {
      if constexpr (std::is_void_v<Result>) {
        if (!this->done && std::exchange(next_frame, false))
          receive();
      }
    }
    bool accepted(asio::error_code error) {
      if (this->done)
        return false;
      if (std::chrono::steady_clock::now() >= expires) {
        fail(std::make_exception_ptr(Error(
            ErrorCode::unavailable, "RPC request timed out; command outcome may be unknown")));
        return false;
      }
      if (error) {
        fail(std::make_exception_ptr(Error(
            ErrorCode::unavailable, "RPC connection failed; command outcome may be unknown")));
        return false;
      }
      return true;
    }
    void deadline(std::chrono::milliseconds timeout) {
      timer.expires_at(std::min(expires, std::chrono::steady_clock::now() + timeout));
      timer.async_wait([self = this->shared_from_this()](asio::error_code error) {
        if (!error)
          self->fail(std::make_exception_ptr(Error(
              ErrorCode::unavailable, "RPC request timed out; command outcome may be unknown")));
      });
    }
    void start() {
      if (!accepted({}))
        return;
      deadline(5s);
      const auto self = this->shared_from_this();
      if constexpr (std::is_same_v<Stream, Tls>) {
        resolver.async_resolve(
            client.address, std::to_string(client.port),
            [self](asio::error_code error, Tcp::resolver::results_type addresses) {
              if (!self->accepted(error))
                return;
              asio::async_connect(self->stream.next_layer(), addresses,
                                  [self](asio::error_code error, const Tcp::endpoint&) {
                                    if (self->accepted(error))
                                      self->stream.async_handshake(asio::ssl::stream_base::client,
                                                                   [self](asio::error_code error) {
                                                                     if (self->accepted(error))
                                                                       self->send();
                                                                   });
                                  });
            });
      } else {
        stream.async_connect(Local::endpoint(client.address), [self](asio::error_code error) {
          if (!self->accepted(error))
            return;
          try {
            detail::verify_local_peer(self->stream.native_handle());
            self->send();
          } catch (...) {
            self->fail(std::current_exception());
          }
        });
      }
    }
    void send() {
      deadline(send_timeout);
      const auto size = static_cast<std::uint32_t>(payload->size());
      header = {static_cast<unsigned char>(size >> 24), static_cast<unsigned char>(size >> 16),
                static_cast<unsigned char>(size >> 8), static_cast<unsigned char>(size)};
      std::array<asio::const_buffer, 2> buffers{asio::buffer(header), asio::buffer(*payload)};
      asio::async_write(stream, buffers,
                        [self = this->shared_from_this()](asio::error_code error, std::size_t) {
                          if (self->accepted(error))
                            self->receive();
                        });
    }
    void receive() {
      payload.reset();
      deadline(reply_timeout);
      asio::async_read(
          stream, asio::buffer(header),
          [self = this->shared_from_this()](asio::error_code error, std::size_t) {
            if (!self->accepted(error))
              return;
            const auto& h = self->header;
            const auto size = (std::uint32_t{h[0]} << 24) | (std::uint32_t{h[1]} << 16) |
                              (std::uint32_t{h[2]} << 8) | h[3];
            if (!size || size > self->client.frame_bytes) {
              self->fail(std::make_exception_ptr(
                  Error(ErrorCode::resource_exhausted, "IPC frame too large or empty")));
              return;
            }
            std::shared_ptr<std::string> incoming;
            try {
              incoming = self->client.payloads.allocate(size);
            } catch (const Error& error) {
              self->fail(std::make_exception_ptr(
                  Error(error.code(),
                        "RPC response payload capacity reached; command outcome may be unknown")));
              return;
            } catch (...) {
              self->fail(std::current_exception());
              return;
            }
            self->payload = incoming;
            asio::async_read(self->stream, asio::buffer(*incoming),
                             [self](asio::error_code error, std::size_t) {
                               if (!self->accepted(error))
                                 return;
                               if constexpr (std::is_void_v<Result>) {
                                 self->timer.cancel();
                                 try {
                                   self->frame(std::move(self->payload));
                                   self->next_frame = true;
                                 } catch (...) {
                                   self->fail(std::current_exception());
                                 }
                               } else {
                                 self->done = true;
                                 self->close();
                                 self->result.set_value(std::move(self->payload));
                               }
                             });
          });
    }
  };
  // A client pumped on its own has its own loop.
  const std::unique_ptr<asio::io_context> own;
  asio::io_context& io;
  const std::string address;
  const std::uint16_t port;
  const std::size_t capacity, frame_bytes;
  const PayloadBudget payloads;
  std::shared_ptr<asio::ssl::context> tls;
  std::vector<std::shared_ptr<Pending>> active;
  Impl(Reactor* reactor, std::string address, std::uint16_t port, std::size_t capacity,
       PayloadBudget payloads, std::size_t frame_bytes)
      : own(reactor ? nullptr : std::make_unique<asio::io_context>()),
        io(reactor ? reactor->impl_->io : *own), address(std::move(address)), port(port),
        capacity(capacity), frame_bytes(frame_bytes), payloads(std::move(payloads)) {
    if (this->address.empty() || this->address.find('\0') != std::string::npos || !capacity)
      throw std::invalid_argument("invalid RPC client endpoint or capacity");
    if (!frame_bytes || frame_bytes > Channel::max_frame || frame_bytes > this->payloads.limit())
      throw std::invalid_argument("invalid RPC client payload limits");
  }
  ~Impl() {
    for (const auto& request : active)
      request->cancel();
  }
  template <class Result, class Stream>
  std::future<Result> begin(Stream stream, std::string payload, std::chrono::milliseconds timeout,
                            std::chrono::steady_clock::time_point expires,
                            std::chrono::milliseconds send_timeout,
                            std::function<void(Payload)> frame) {
    auto call = std::make_shared<Exchange<Stream, Result>>(*this, std::move(stream),
                                                           std::move(payload), timeout, expires,
                                                           send_timeout, std::move(frame));
    auto result = call->result.get_future();
    active.push_back(call);
    call->start();
    return result;
  }
  template <class Result>
  std::future<Result> request(std::string payload, std::chrono::milliseconds reply_timeout,
                              std::chrono::steady_clock::time_point deadline,
                              std::chrono::milliseconds send_timeout,
                              std::function<void(Payload)> frame = {}) {
    if (payload.empty() || payload.size() > frame_bytes)
      throw Error(ErrorCode::resource_exhausted, "IPC frame too large or empty");
    if (reply_timeout <= 0ms)
      throw std::invalid_argument("RPC reply timeout must be positive");
    std::erase_if(active, [](const auto& call) { return call->done; });
    if (active.size() >= capacity)
      throw Error(ErrorCode::resource_exhausted, "RPC client request capacity reached");
    if (tls) {
      Tls stream(io, *tls);
      if (SSL_set1_host(stream.native_handle(), address.c_str()) != 1 ||
          SSL_set_tlsext_host_name(stream.native_handle(), address.c_str()) != 1)
        throw std::invalid_argument("invalid TLS server identity");
      return begin<Result>(std::move(stream), std::move(payload), reply_timeout, deadline,
                           send_timeout, std::move(frame));
    }
    return begin<Result>(Local::socket(io), std::move(payload), reply_timeout, deadline,
                         send_timeout, std::move(frame));
  }
};
RpcClient::RpcClient(Reactor* reactor, std::string endpoint, std::size_t capacity,
                     PayloadBudget payloads, std::size_t frame_bytes)
    : impl_(std::make_unique<Impl>(reactor, std::move(endpoint), 0, capacity, std::move(payloads),
                                   frame_bytes)) {
  if (impl_->address.front() != '/')
    throw std::invalid_argument("invalid local socket path");
  static_cast<void>(Local::endpoint(impl_->address));
}
RpcClient::RpcClient(Reactor* reactor, std::string host, std::uint16_t port,
                     const TlsIdentity& identity, std::size_t capacity, PayloadBudget payloads,
                     std::size_t frame_bytes)
    : impl_(std::make_unique<Impl>(reactor, std::move(host), port, capacity, std::move(payloads),
                                   frame_bytes)) {
  if (!port)
    throw std::invalid_argument("TCP host, port and positive timeout are required");
  impl_->tls = detail::tls_context(identity, false);
}
RpcClient::RpcClient(std::string endpoint, std::size_t capacity, PayloadBudget payloads,
                     std::size_t frame_bytes)
    : RpcClient(nullptr, std::move(endpoint), capacity, std::move(payloads), frame_bytes) {}
RpcClient::RpcClient(std::string host, std::uint16_t port, const TlsIdentity& identity,
                     std::size_t capacity, PayloadBudget payloads, std::size_t frame_bytes)
    : RpcClient(nullptr, std::move(host), port, identity, capacity, std::move(payloads),
                frame_bytes) {}
RpcClient::RpcClient(Reactor& reactor, std::string endpoint, std::size_t capacity,
                     PayloadBudget payloads, std::size_t frame_bytes)
    : RpcClient(&reactor, std::move(endpoint), capacity, std::move(payloads), frame_bytes) {}
RpcClient::RpcClient(Reactor& reactor, std::string host, std::uint16_t port,
                     const TlsIdentity& identity, std::size_t capacity, PayloadBudget payloads,
                     std::size_t frame_bytes)
    : RpcClient(&reactor, std::move(host), port, identity, capacity, std::move(payloads),
                frame_bytes) {}
RpcClient::~RpcClient() = default;
std::future<Payload> RpcClient::request(std::string payload,
                                        std::chrono::milliseconds reply_timeout,
                                        std::chrono::steady_clock::time_point deadline,
                                        std::chrono::milliseconds send_timeout) {
  return impl_->request<Payload>(std::move(payload), reply_timeout, deadline, send_timeout);
}
std::future<void> RpcClient::watch(std::string payload, std::chrono::milliseconds frame_timeout,
                                   std::function<void(Payload)> frame) {
  return impl_->request<void>(std::move(payload), frame_timeout,
                              std::chrono::steady_clock::time_point::max(), 5s, std::move(frame));
}
void RpcClient::cancel() {
  for (const auto& call : impl_->active)
    call->cancel();
}

void RpcClient::poll(std::chrono::milliseconds wait) {
  for (const auto& call : impl_->active)
    call->advance();
  impl_->io.restart();
  if (wait > 0ms) {
    // Keep the wait bounded even when computation has no outstanding RPC.
    const auto work = asio::make_work_guard(impl_->io);
    impl_->io.run_for(wait);
  } else
    impl_->io.poll();
  std::erase_if(impl_->active, [](const auto& call) { return call->done; });
}
} // namespace asterion::ipc
