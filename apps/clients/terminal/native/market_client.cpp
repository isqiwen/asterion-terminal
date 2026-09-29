#include "market_client.hpp"
#include "market_history.hpp"
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/protocol/market.hpp>
#include <asterion/protocol/trading.hpp>
#include <atomic>
#include <condition_variable>
#include <algorithm>
#include <map>
#include <mutex>
#include <thread>
namespace asterion::terminal {
namespace wire = asterion::market::v1;
using namespace std::chrono_literals;
namespace {
// Process-wide so a replaced market client never reuses a revision a
// Terminal window already holds; polls then receive complete state.
std::atomic<std::uint64_t> sync_revision{0};
} // namespace
struct MarketClient::Impl {
  ServiceEndpoint endpoint;
  mutable std::mutex mutex;
  std::mutex commands;
  std::condition_variable wake;
  std::jthread worker;
  Json cached = nullptr;
  MarketHistory history;
  bool online = false;
  std::atomic<std::uint64_t> sequence{0};
  std::chrono::steady_clock::time_point seen{};
  // Change tracking for incremental Terminal polls (see annotate()).
  std::map<std::string, std::pair<Json, std::uint64_t>> rows;
  std::vector<std::string> keys;
  Json catalog = nullptr;
  std::uint64_t set_revision = 0, catalog_revision = 0;
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
              if (response.snapshot().phase() != "connected") {
                std::lock_guard lock(mutex);
                if (!history.stream.empty())
                  history.interrupt();
                continue;
              }
              try {
                wire::Request events;
                {
                  std::lock_guard lock(mutex);
                  events.mutable_events()->set_stream_id(history.stream);
                  events.mutable_events()->set_after_sequence(history.cursor);
                  events.mutable_events()->set_limit(1024);
                }
                const auto batch = call(events);
                std::lock_guard lock(mutex);
                history.append(batch.events());
              } catch (const std::exception&) {
                // A history error must not stop quote/status delivery.
                std::lock_guard lock(mutex);
                history.interrupt();
              }
            }
          };
          if (endpoint.endpoint.empty())
            stream(ipc::TlsChannel::connect(endpoint.host, endpoint.port, endpoint.tls, 2s));
          else
            stream(ipc::Channel::connect(endpoint.endpoint, 2s));
        } catch (const std::exception&) {
          std::unique_lock lock(mutex);
          online = false;
          history.interrupt();
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
    if (request.has_minutes()) {
      if (!response.has_minutes() ||
          response.minutes().instrument().venue() != request.minutes().instrument().venue() ||
          response.minutes().instrument().symbol() != request.minutes().instrument().symbol())
        throw Error(ErrorCode::unavailable, "missing market minutes");
      return response;
    }
    if (request.has_events()) {
      if (!response.has_events())
        throw Error(ErrorCode::unavailable, "missing market events");
      return response;
    }
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
  // Stamps each quote row with the revision of its last change, and the
  // subscription set and catalog with their own revisions.
  void annotate() {
    auto& subscriptions = cached.at("subscriptions");
    std::vector<std::string> next;
    next.reserve(subscriptions.size());
    for (auto& row : subscriptions) {
      auto key = row.at("venue").get<std::string>() + "." + row.at("symbol").get<std::string>();
      auto found = rows.find(key);
      if (found == rows.end() || found->second.first != row)
        found = rows.insert_or_assign(key, std::pair{row, ++sync_revision}).first;
      row["revision"] = found->second.second;
      next.push_back(std::move(key));
    }
    if (next != keys || !set_revision) {
      keys = std::move(next);
      set_revision = ++sync_revision;
      std::erase_if(rows, [&](const auto& item) {
        return std::ranges::find(keys, item.first) == keys.end();
      });
    }
    if (cached.at("catalog") != catalog || !catalog_revision) {
      catalog = cached.at("catalog");
      catalog_revision = ++sync_revision;
    }
    cached["subscription_set"] = set_revision;
    cached["catalog"]["revision"] = catalog_revision;
  }
  void publish(const wire::Snapshot& state) {
    std::lock_guard lock(mutex);
    if (cached.is_null() || cached.at("instance_id") != state.instance_id() ||
        cached.at("sequence").get<std::uint64_t>() <= state.sequence()) {
      cached = protocol::decode_market(state);
      annotate();
    }
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
void MarketClient::catalog(const Json& params) {
  wire::Request request;
  auto* c = request.mutable_catalog();
  c->set_front(params.at("front").get<std::string>());
  c->set_broker(params.at("broker").get<std::string>());
  c->set_user(params.at("user").get<std::string>());
  c->set_password(params.at("password").get<std::string>());
  c->set_app_id(params.at("app_id").get<std::string>());
  c->set_auth_code(params.at("auth_code").get<std::string>());
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
Json MarketClient::minutes(const std::string& venue, const std::string& symbol) {
  const InstrumentId id{venue, symbol};
  id.validate();
  wire::Request request;
  request.mutable_minutes()->mutable_instrument()->set_venue(venue);
  request.mutable_minutes()->mutable_instrument()->set_symbol(symbol);
  return protocol::decode_minutes(impl_->call(std::move(request)).minutes());
}
Json MarketClient::snapshot() const {
  std::lock_guard lock(impl_->mutex);
  auto out = impl_->cached;
  out["transport_online"] = impl_->online && std::chrono::steady_clock::now() - impl_->seen < 5s;
  out["service"] = impl_->endpoint.session;
  out["history"] = impl_->history.snapshot();
  return out;
}
} // namespace asterion::terminal
