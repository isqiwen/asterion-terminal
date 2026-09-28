#include "market_client.hpp"
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/protocol/market.hpp>
#include <asterion/protocol/trading.hpp>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
namespace asterion::terminal {
namespace wire = asterion::market::v1;
using namespace std::chrono_literals;
struct MarketClient::Impl {
  ServiceEndpoint endpoint;
  mutable std::mutex mutex;
  std::mutex commands;
  std::condition_variable wake;
  std::jthread worker;
  Json cached = nullptr;
  bool online = false;
  std::atomic<std::uint64_t> sequence{0};
  std::chrono::steady_clock::time_point seen{};
  explicit Impl(ServiceEndpoint value) : endpoint(std::move(value)) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    for (;;) {
      try {
        wire::Request request;
        request.mutable_snapshot();
        publish(call(request).snapshot());
        break;
      } catch (const Error&) {
        if (std::chrono::steady_clock::now() >= deadline)
          throw;
        std::this_thread::sleep_for(100ms);
      }
    }
    worker = std::jthread([this](std::stop_token stop) {
      while (!stop.stop_requested()) {
        try {
          wire::Request request;
          request.mutable_watch();
          identify(request);
          auto stream = [&](auto channel) {
            channel.send(request.SerializeAsString(), 2s);
            while (!stop.stop_requested()) {
              auto response = decode(channel.receive(2s), request);
              publish(response.snapshot());
            }
          };
          if (endpoint.endpoint.empty())
            stream(ipc::TlsChannel::connect(endpoint.host, endpoint.port, endpoint.tls, 2s));
          else
            stream(ipc::Channel::connect(endpoint.endpoint, 2s));
        } catch (const std::exception&) {
          std::unique_lock lock(mutex);
          online = false;
          wake.wait_for(lock, 2s, [&] { return stop.stop_requested(); });
        }
      }
    });
  }
  ~Impl() {
    worker.request_stop();
    wake.notify_all();
    if (worker.joinable())
      worker.join();
  }
  void identify(wire::Request& request) {
    request.set_version(1);
    request.set_service_id(endpoint.session);
    request.set_correlation_id("market." + std::to_string(++sequence));
  }
  wire::Response decode(const std::string& raw, const wire::Request& request) {
    wire::Response response;
    if (!response.ParseFromString(raw))
      throw Error(ErrorCode::unavailable, "invalid market response");
    protocol::validate_message(response);
    if (response.version() != 1 || response.service_id() != endpoint.session ||
        response.correlation_id() != request.correlation_id())
      throw Error(ErrorCode::unavailable, "market response identity mismatch");
    if (response.has_error())
      throw_remote_error(response.error().code(), response.error().message());
    if (!response.has_snapshot() || response.snapshot().instance_id().empty())
      throw Error(ErrorCode::unavailable, "missing market snapshot");
    return response;
  }
  wire::Response call(wire::Request request) {
    std::lock_guard lock(commands);
    identify(request);
    auto exchange = [&](auto channel) {
      channel.send(request.SerializeAsString(), 3s);
      return decode(channel.receive(3s), request);
    };
    return endpoint.endpoint.empty()
               ? exchange(ipc::TlsChannel::connect(endpoint.host, endpoint.port, endpoint.tls, 3s))
               : exchange(ipc::Channel::connect(endpoint.endpoint, 3s));
  }
  void publish(const wire::Snapshot& state) {
    std::lock_guard lock(mutex);
    if (cached.is_null() || cached.at("instance_id") != state.instance_id() ||
        cached.at("sequence").get<std::uint64_t>() <= state.sequence())
      cached = protocol::decode_market(state);
    online = true;
    seen = std::chrono::steady_clock::now();
  }
};
MarketClient::MarketClient(ServiceEndpoint endpoint)
    : impl_(std::make_unique<Impl>(std::move(endpoint))) {}
MarketClient::~MarketClient() = default;
void MarketClient::connect(const Json& params) {
  wire::Request request;
  auto* c = request.mutable_connect();
  c->set_front(params.at("front").get<std::string>());
  c->set_broker(params.at("broker").get<std::string>());
  c->set_user(params.at("user").get<std::string>());
  c->set_password(params.at("password").get<std::string>());
  for (const auto& id : params.at("instruments")) {
    auto* i = c->add_instruments();
    i->set_venue(id.at("venue").get<std::string>());
    i->set_symbol(id.at("symbol").get<std::string>());
  }
  impl_->publish(impl_->call(std::move(request)).snapshot());
}
void MarketClient::subscribe(const Json& ids) {
  wire::Request request;
  for (const auto& id : ids) {
    auto* i = request.mutable_subscribe()->add_instruments();
    i->set_venue(id.at("venue").get<std::string>());
    i->set_symbol(id.at("symbol").get<std::string>());
  }
  if (ids.empty())
    request.mutable_subscribe();
  impl_->publish(impl_->call(request).snapshot());
}
void MarketClient::disconnect() {
  wire::Request request;
  request.mutable_disconnect();
  impl_->publish(impl_->call(request).snapshot());
}
Json MarketClient::snapshot() const {
  std::lock_guard lock(impl_->mutex);
  auto out = impl_->cached;
  out["transport_online"] = impl_->online && std::chrono::steady_clock::now() - impl_->seen < 5s;
  out["service"] = impl_->endpoint.session;
  return out;
}
} // namespace asterion::terminal
