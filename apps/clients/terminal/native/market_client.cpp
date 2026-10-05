#include "market_client.hpp"
#include "market_history.hpp"
#include "market_snapshot.hpp"
#include <asterion/kernel/ipc/rpc_client.hpp>
#include <asterion/kernel/trace.hpp>
#include <asterion/protocol/market.hpp>
#include <asterion/protocol/trading.hpp>
#include <array>
namespace asterion::terminal {
namespace wire = asterion::market::v1;
using namespace std::chrono_literals;
struct MarketClient::Impl : std::enable_shared_from_this<Impl> {
  ServiceIo& io;
  const ServiceEndpoint endpoint;
  // Controls, minute reads, ordered event reads and watch have separate capacity.
  enum Channel : std::size_t { control, minutes, events, watch, channel_count };
  std::array<std::unique_ptr<ipc::RpcClient>, channel_count> transports;
  std::stop_source lifetime;
  MarketSnapshot view;
  MarketHistory history;
  bool online = false, events_due = false, command_busy = false;
  std::uint64_t history_epoch = 0;
  std::chrono::steady_clock::time_point seen{};
  Impl(ServiceIo& owner, ServiceEndpoint value) : io(owner), endpoint(std::move(value)) {
    validate_id(endpoint.session);
    constexpr std::array<std::size_t, channel_count> capacity{1, 8, 1, 1};
    for (std::size_t i = 0; i < transports.size(); ++i) {
      auto budget = io.payload_budget(i != control ? ServiceIo::PayloadLane::data
                                                   : ServiceIo::PayloadLane::control);
      transports[i] =
          endpoint.endpoint.empty()
              ? std::make_unique<ipc::RpcClient>(endpoint.host, endpoint.port, endpoint.tls,
                                                 capacity[i], budget)
              : std::make_unique<ipc::RpcClient>(endpoint.endpoint, capacity[i], budget);
    }
  }
  bool stopped(std::stop_token stop) const {
    return stop.stop_requested() || lifetime.stop_requested();
  }
  void identify(wire::Request& request) {
    request.set_version(1);
    request.set_service_id(endpoint.session);
    request.set_correlation_id(next_correlation_id());
  }
  struct Decoded {
    wire::Response response;
    std::optional<MarketUpdate> snapshot;
  };
  static Decoded decode(const std::string& raw, const wire::Request& request) {
    wire::Response response;
    if (!response.ParseFromString(raw))
      throw Error(ErrorCode::unavailable, "invalid market response");
    protocol::validate_message(response);
    if (response.version() != 1 || response.service_id() != request.service_id() ||
        response.correlation_id() != request.correlation_id())
      throw Error(ErrorCode::unavailable, "market response identity mismatch");
    if (response.has_error())
      throw_remote_error(response.error().code(), response.error().message());
    if (request.has_minutes()) {
      if (!response.has_minutes() ||
          response.minutes().instrument().venue() != request.minutes().instrument().venue() ||
          response.minutes().instrument().symbol() != request.minutes().instrument().symbol())
        throw Error(ErrorCode::unavailable, "missing market minutes");
      return {std::move(response), {}};
    }
    if (request.has_events()) {
      if (!response.has_events())
        throw Error(ErrorCode::unavailable, "missing market events");
      return {std::move(response), {}};
    }
    if (!response.has_snapshot() || response.snapshot().instance_id().empty())
      throw Error(ErrorCode::unavailable, "missing market snapshot");
    if (!request.has_watch() &&
        (response.snapshot().catalog_omitted() || response.snapshot().subscriptions_delta()))
      throw Error(ErrorCode::unavailable, "market catalog revision is unavailable");
    MarketUpdate snapshot(response.snapshot());
    response.clear_snapshot();
    return {std::move(response), std::move(snapshot)};
  }
  void interrupt_history() {
    history.interrupt();
    ++history_epoch;
    events_due = false;
  }
  void publish(MarketUpdate update, std::uint64_t generation, bool from_watch = false) {
    const auto& previous = view.metadata();
    const bool changed_instance =
        !previous.is_null() && previous.at("instance_id") != update.value.at("instance_id");
    const bool was_connected = !previous.is_null() && previous.at("phase") == "connected";
    if (!view.apply(std::move(update), generation))
      return;
    if (changed_instance && !from_watch)
      transports[watch]->cancel();
    const bool connected = view.metadata().at("phase") == "connected";
    if (changed_instance || (was_connected && !connected))
      interrupt_history();
    events_due = connected;
    online = true;
    seen = std::chrono::steady_clock::now();
  }
  PolledTask<wire::Response> exchange(wire::Request request, std::stop_token stop) {
    const bool command = !request.has_events() && !request.has_minutes();
    if (command)
      co_await PollUntil{[&] { return !command_busy || stopped(stop); }};
    if (stopped(stop))
      throw Error(ErrorCode::cancelled, "market connection closed");
    if (command)
      command_busy = true;
    struct Release {
      bool& busy;
      bool command;
      ~Release() {
        if (command)
          busy = false;
      }
    } release{command_busy, command};
    identify(request);
    auto& transport = transports[request.has_events()    ? events
                                 : request.has_minutes() ? minutes
                                                         : control];
    const auto generation = view.generation();
    auto reply = transport->request(request.SerializeAsString(), 3s);
    while (reply.wait_for(0ms) != std::future_status::ready) {
      if (stopped(stop))
        transport->cancel();
      else
        transport->poll();
      co_await std::suspend_always{};
    }
    auto response = co_await io.read<Decoded>(
        [raw = reply.get(), &request] { return decode(*raw, request); },
        request.has_events() || request.has_minutes() ? ServiceIo::ReadLane::data
                                                      : ServiceIo::ReadLane::response);
    if (response.snapshot)
      publish(std::move(*response.snapshot), generation);
    co_return std::move(response.response);
  }
  std::future<void> command(wire::Request request) {
    return io.submit<void>([state = shared_from_this(), request = std::move(request)](
                               std::stop_token stop) mutable -> PolledTask<void> {
      co_await state->exchange(std::move(request), stop);
    });
  }
  PolledTask<void> initialize(std::stop_token stop) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    for (;;) {
      try {
        wire::Request request;
        request.mutable_snapshot();
        co_await exchange(std::move(request), stop);
        co_return;
      } catch (const Error&) {
        if (stopped(stop) || std::chrono::steady_clock::now() >= deadline)
          throw;
      }
      const auto next = std::chrono::steady_clock::now() + 100ms;
      co_await PollUntil{[&] { return stopped(stop) || std::chrono::steady_clock::now() >= next; }};
    }
  }
  PolledTask<void> observe(std::stop_token stop) {
    auto& transport = transports[watch];
    while (!stopped(stop)) {
      try {
        wire::Request request;
        request.mutable_watch();
        identify(request);
        MarketWatchCursor cursor;
        std::optional<Payload> frame;
        auto watching = transport->watch(request.SerializeAsString(), 2s,
                                         [&](Payload bytes) { frame = std::move(bytes); });
        while (watching.wait_for(0ms) != std::future_status::ready) {
          if (stopped(stop))
            transport->cancel();
          else
            transport->poll();
          if (frame) {
            const auto generation = view.generation();
            // RpcClient yields one frame per poll. Do not poll this stream again
            // until its frame is prepared and applied: order and memory stay bounded.
            auto response = co_await io.read<Decoded>(
                [raw = std::move(*frame), &request] { return decode(*raw, request); });
            frame.reset();
            cursor.accept(*response.snapshot);
            publish(std::move(*response.snapshot), generation, true);
          }
          co_await std::suspend_always{};
        }
        watching.get();
      } catch (const std::exception&) {
        transport->cancel();
        online = false;
        interrupt_history();
      }
      const auto next = std::chrono::steady_clock::now() + 2s;
      co_await PollUntil{[&] { return stopped(stop) || std::chrono::steady_clock::now() >= next; }};
    }
  }
  PolledTask<void> observe_events(std::stop_token stop) {
    while (!stopped(stop)) {
      co_await PollUntil{[&] { return stopped(stop) || (online && events_due); }};
      if (stopped(stop))
        break;
      events_due = false;
      const auto epoch = history_epoch;
      try {
        wire::Request request;
        request.mutable_events()->set_stream_id(history.stream);
        request.mutable_events()->set_after_sequence(history.cursor);
        request.mutable_events()->set_limit(1024);
        const auto response = co_await exchange(std::move(request), stop);
        if (epoch == history_epoch && online)
          history.append(response.events());
      } catch (const std::exception&) {
        if (epoch == history_epoch)
          interrupt_history();
      }
    }
  }
  PolledTask<void> monitor(std::stop_token stop) {
    auto quotes = observe(stop);
    auto events = observe_events(stop);
    for (;;) {
      const bool quotes_done = quotes.poll();
      const bool events_done = events.poll();
      if (quotes_done && events_done)
        break;
      co_await std::suspend_always{};
    }
    quotes.take();
    events.take();
  }
  MarketProjection read() const {
    auto out = view.capture();
    out.header["transport_online"] = online && std::chrono::steady_clock::now() - seen < 5s;
    out.header["service"] = endpoint.session;
    out.header["remote"] = endpoint.endpoint.empty();
    out.header["host"] = endpoint.endpoint.empty() ? endpoint.host : "localhost";
    out.header["port"] = endpoint.port;
    out.header["history"] = history.snapshot();
    return out;
  }
};
MarketClient::MarketClient(ServiceIo& io, ServiceEndpoint endpoint)
    : impl_(std::make_shared<Impl>(io, std::move(endpoint))) {}
std::future<std::shared_ptr<MarketClient>> MarketClient::open(ServiceIo& io,
                                                              ServiceEndpoint endpoint) {
  return io.submit<std::shared_ptr<MarketClient>>(
      [&io, endpoint = std::move(endpoint)](
          std::stop_token stop) mutable -> PolledTask<std::shared_ptr<MarketClient>> {
        auto client = co_await io.admin<std::shared_ptr<MarketClient>>([&] {
          return std::shared_ptr<MarketClient>(new MarketClient(io, std::move(endpoint)));
        });
        co_await client->impl_->initialize(stop);
        (void)client->impl_->io.submit<void>(
            [state = client->impl_](std::stop_token stop) { return state->monitor(stop); },
            ServiceIo::Lane::observation);
        co_return client;
      });
}
MarketClient::~MarketClient() {
  impl_->lifetime.request_stop();
}

std::future<void> MarketClient::connect(const Json& params) {
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
  return impl_->command(std::move(request));
}
std::future<void> MarketClient::catalog(const Json& params) {
  wire::Request request;
  auto* c = request.mutable_catalog();
  c->set_front(params.at("front").get<std::string>());
  c->set_broker(params.at("broker").get<std::string>());
  c->set_user(params.at("user").get<std::string>());
  c->set_password(params.at("password").get<std::string>());
  c->set_app_id(params.at("app_id").get<std::string>());
  c->set_auth_code(params.at("auth_code").get<std::string>());
  return impl_->command(std::move(request));
}
std::future<void> MarketClient::subscribe(const Json& ids) {
  wire::Request request;
  for (const auto& id : ids) {
    auto* i = request.mutable_subscribe()->add_instruments();
    i->set_venue(id.at("venue").get<std::string>());
    i->set_symbol(id.at("symbol").get<std::string>());
  }
  if (ids.empty())
    request.mutable_subscribe();
  return impl_->command(std::move(request));
}
std::future<void> MarketClient::disconnect() {
  wire::Request request;
  request.mutable_disconnect();
  return impl_->command(std::move(request));
}
std::future<Json> MarketClient::minutes(const std::string& venue, const std::string& symbol) {
  const InstrumentId id{venue, symbol};
  id.validate();
  wire::Request request;
  request.mutable_minutes()->mutable_instrument()->set_venue(venue);
  request.mutable_minutes()->mutable_instrument()->set_symbol(symbol);
  return impl_->io.submit<Json>([state = impl_, request = std::move(request)](
                                    std::stop_token stop) mutable -> PolledTask<Json> {
    auto response = co_await state->exchange(std::move(request), stop);
    co_return co_await state->io.read<Json>(
        [response = std::move(response)] { return protocol::decode_minutes(response.minutes()); });
  });
}
std::future<Json> MarketClient::snapshot() const {
  return impl_->io.submit<Json>([state = impl_](std::stop_token) -> PolledTask<Json> {
    co_return co_await state->io.read<Json>(
        [projection = state->read()] { return projection.render(); },
        ServiceIo::ReadLane::response);
  });
}
MarketProjection MarketClient::owner_read() const {
  return impl_->read();
}
} // namespace asterion::terminal
