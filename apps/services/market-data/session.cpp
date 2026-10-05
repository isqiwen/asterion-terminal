#include "session.hpp"
#include "catalog_job.hpp"
#include "ctp_feed.hpp"
#include <asterion/domain/intraday_bars.hpp>
#include <asterion/foundation/bounded_queue.hpp>
#include <asterion/protocol/market.hpp>
#include <asterion/protocol/rpc_diagnostics.hpp>
#include <asterion/protocol/trading.hpp>
#include <map>
#include <set>
#include <thread>
namespace asterion::market_data {
using namespace std::chrono_literals;
namespace wire = market::v1;
namespace {
struct State {
  const std::string service, instance;
  const std::filesystem::path directory, sdk, catalog_sdk;
  ThreadPool sdk_owner{1, 2};
  std::unique_ptr<ctp::Feed> feed;
  CatalogJob catalog;
  bool quiescing = false;
  std::vector<InstrumentId> watchlist;
  std::uint64_t applied_catalog = 0, extra_sequence = 0;
  IntradayBars intraday;
  std::string stream;
  std::uint64_t event_cursor = 0;
  State(const Session::Configuration& config)
      : service(config.service), instance(config.instance), directory(config.directory),
        sdk(config.sdk), catalog_sdk(config.catalog_sdk), catalog(sdk_owner) {
    catalog.persist_to(directory / "ctp-catalog.pb");
  }
  ~State() {
    if (feed)
      feed->stop();
  }
  std::string phase() const {
    if (catalog.initializing())
      return "initializing";
    return feed ? feed->phase() : sdk.empty() ? "sdk_unavailable" : "disconnected";
  }
  void advance() {
    if (feed)
      feed->poll();
    catalog.poll();
    const auto [catalog_revision, catalog_state] = catalog.snapshot();
    if (feed && feed->phase() == "connected" && catalog_state.phase() == "ready" &&
        applied_catalog != catalog_revision) {
      std::map<InstrumentId, int> multipliers;
      for (const auto& row : catalog_state.contracts())
        multipliers[{row.instrument().venue(), row.instrument().symbol()}] = row.multiplier();
      feed->set_multipliers(std::move(multipliers));
      feed->subscribe(interests(catalog_state, watchlist));
      applied_catalog = catalog_revision;
    }
    if (!feed)
      return;
    // Bounded work per turn. Client traffic cannot prevent event consumption,
    // and a continuous provider stream cannot starve admitted commands.
    const auto batch = feed->events_after(stream, event_cursor, 1024);
    if (batch.gap || batch.failed)
      intraday.interrupt();
    for (const auto& event : batch.events) {
      event_cursor = event.sequence;
      if (const auto* status = std::get_if<LiveMarketSnapshot>(&event.value)) {
        if (status->phase != "connected")
          intraday.interrupt();
      } else if (const auto* quote = std::get_if<MarketQuoteObservation>(&event.value);
                 quote && !quote->out_of_order)
        intraday.observe(quote->quote);
    }
    stream = batch.stream_id;
    if (batch.failed)
      event_cursor = batch.latest_sequence;
  }
  std::vector<InstrumentId> interests(const wire::CatalogState& state,
                                      const std::vector<InstrumentId>& selected) {
    std::vector<InstrumentId> ids = selected;
    std::set<std::string> symbols;
    for (const auto& id : ids)
      symbols.insert(id.symbol);
    if (state.phase() == "ready")
      for (const auto& row : state.contracts())
        if (symbols.insert(row.instrument().symbol()).second)
          ids.push_back({row.instrument().venue(), row.instrument().symbol()});
    return ids;
  }
  wire::Snapshot snapshot(WatchCursor* held = nullptr) {
    const auto [catalog_revision, catalog_state] = catalog.snapshot();
    LiveMarketSnapshot state;
    if (feed) {
      const auto forced =
          held ? intraday.changed_after(held->intraday) : std::vector<InstrumentId>{};
      state = feed->snapshot(held ? held->feed : std::nullopt, forced);
    } else
      state.phase = sdk.empty() ? "sdk_unavailable" : "disconnected";
    auto out = protocol::encode_market(state, instance);
    out.set_sequence(state.sequence + catalog_revision + extra_sequence + intraday.revision());
    if (state.subscriptions_delta)
      out.set_base_sequence(held->sequence);
    out.set_catalog_revision(catalog_revision);
    if (held && held->catalog == catalog_revision)
      out.set_catalog_omitted(true);
    else
      *out.mutable_catalog() = catalog_state;
    for (const auto& id : watchlist) {
      auto* row = out.add_watchlist();
      row->set_venue(id.venue);
      row->set_symbol(id.symbol);
    }
    for (auto& row : *out.mutable_subscriptions())
      if (const auto change =
              intraday.change_1m_percent({row.instrument().venue(), row.instrument().symbol()}))
        row.set_change_1m_percent(change->str());
    if (held) {
      held->catalog = catalog_revision;
      held->feed = feed ? std::optional(state.sequence) : std::nullopt;
      held->intraday = intraday.revision();
      held->sequence = out.sequence();
    }
    return out;
  }
  service::RpcHost::Message process(wire::Request request, WatchCursor* held, bool control) {
    wire::Response response;
    response.set_version(1);
    response.set_service_id(service);
    response.set_correlation_id(request.correlation_id());
    try {
      if (control) {
        if (request.has_quiesce()) {
          const auto state = feed ? feed->phase() : "disconnected";
          if (catalog.running() || (state != "disconnected" && state != "sdk_unavailable")) {
            response.mutable_error()->set_code("unavailable");
            response.mutable_error()->set_message(
                "market connection must be disconnected before upgrade");
            return {response.SerializeAsString()};
          }
          quiescing = true;
          if (request.quiesce().stop())
            service::request_stop();
        }
        auto* h = response.mutable_health();
        h->set_instance_id(instance);
        h->set_initialized(!catalog.initializing());
        h->set_phase(phase());
      } else if (request.has_heartbeat()) {
        auto* h = response.mutable_health();
        h->set_instance_id(instance);
        h->set_initialized(!catalog.initializing());
        h->set_phase(phase());
      } else if (request.has_events()) {
        if (!feed)
          throw Error(ErrorCode::unavailable, "market event stream not started");
        const auto& read = request.events();
        *response.mutable_events() = protocol::encode_market_events(
            feed->events_after(read.stream_id(), read.after_sequence(), read.limit()));
      } else if (request.has_minutes()) {
        const InstrumentId id{request.minutes().instrument().venue(),
                              request.minutes().instrument().symbol()};
        id.validate();
        if (auto series = intraday.series(id))
          *response.mutable_minutes() = protocol::encode_minutes(*series);
        else
          *response.mutable_minutes()->mutable_instrument() = request.minutes().instrument();
      } else if (request.has_watch()) {
        *response.mutable_snapshot() = snapshot(held);
      } else {
        if (quiescing && !request.has_snapshot())
          throw Error(ErrorCode::unavailable, "market service is preparing for upgrade");
        if (request.has_connect()) {
          if (!feed) {
            auto plugin = std::make_unique<ctp::Feed>(sdk_owner, sdk, directory / "ctp-flow");
            plugin->start();
            feed = std::move(plugin);
          }
          auto* c = request.mutable_connect();
          std::vector<InstrumentId> ids;
          for (const auto& i : c->instruments())
            ids.push_back({i.venue(), i.symbol()});
          if (ids.size() > 50)
            throw std::invalid_argument("at most 50 watchlist contracts");
          ctp::Configuration config{c->front(), c->broker(), c->user(), c->password()};
          c->clear_password();
          feed->connect(std::move(config), ids);
          watchlist = std::move(ids);
          catalog.cancel();
          applied_catalog = 0;
          ++extra_sequence;
        } else if (request.has_catalog()) {
          if (catalog_sdk.empty())
            throw Error(ErrorCode::unavailable, "CTP catalog SDK unavailable");
          auto* c = request.mutable_catalog();
          ctp::CatalogConfiguration config{c->front(),    c->broker(), c->user(),
                                           c->password(), c->app_id(), c->auth_code()};
          c->clear_password();
          c->clear_auth_code();
          catalog.start(catalog_sdk, std::filesystem::path(directory) / "ctp-catalog-flow",
                        std::move(config));
        } else if (request.has_subscribe()) {
          if (!feed)
            throw Error(ErrorCode::conflict, "connect market data first");
          std::vector<InstrumentId> ids;
          for (const auto& i : request.subscribe().instruments())
            ids.push_back({i.venue(), i.symbol()});
          if (ids.size() > 50)
            throw std::invalid_argument("at most 50 watchlist contracts");
          ctp::validate_instruments(ids);
          feed->subscribe(interests(catalog.snapshot().second, ids));
          watchlist = std::move(ids);
          ++extra_sequence;
        } else if (request.has_disconnect()) {
          catalog.cancel();
          applied_catalog = 0;
          if (feed)
            feed->disconnect();
        } else if (!request.has_snapshot())
          throw std::invalid_argument("missing market operation");
        *response.mutable_snapshot() = snapshot();
      }
    } catch (const Error& error) {
      response.mutable_error()->set_code(std::string(error_name(error.code())));
      response.mutable_error()->set_message(error.what());
    } catch (const std::exception& error) {
      response.mutable_error()->set_code(std::string(error_name(classify(error))));
      response.mutable_error()->set_message(
          "Market request rejected; check configuration and SDK availability");
    }
    protocol::log_rpc_result("market-data", request, response,
                             request.has_snapshot() || request.has_heartbeat() ||
                                 request.has_watch());
    return {response.SerializeAsString(), request.has_watch() && response.has_snapshot()};
  }
};
} // namespace
struct Session::Loop {
  using Work = std::packaged_task<service::RpcHost::Message(State&)>;
  BoundedQueue<Work> requests{64}, controls{4};
  std::promise<void> completion;
  std::shared_future<void> stopped = completion.get_future().share();
  std::jthread thread;
  explicit Loop(Configuration config)
      : thread([this, config = std::move(config)](std::stop_token stop) {
          try {
            {
              State state(config);
              while (!stop.stop_requested()) {
                state.advance();
                for (int i = 0; i < 4; ++i) {
                  auto work = controls.try_pop();
                  if (!work)
                    break;
                  (*work)(state);
                }
                for (int i = 0; !state.catalog.initializing() && i < 16; ++i) {
                  auto work = requests.try_pop();
                  if (!work)
                    break;
                  (*work)(state);
                }
                std::this_thread::sleep_for(10ms);
              }
            } // Join provider resources on their owner before reporting completion.
            completion.set_value();
          } catch (...) {
            completion.set_exception(std::current_exception());
          }
          requests.close();
          controls.close();
        }) {}
};
Session::Session(Configuration config) : loop_(std::make_unique<Loop>(std::move(config))) {}
Session::~Session() = default;
std::future<service::RpcHost::Message>
Session::request(wire::Request request, std::shared_ptr<WatchCursor> cursor, bool control) {
  Loop::Work work(
      [request = std::move(request), cursor = std::move(cursor), control](State& state) mutable {
        return state.process(std::move(request), cursor.get(), control);
      });
  auto future = work.get_future();
  if (!(control ? loop_->controls : loop_->requests).try_push(std::move(work)))
    throw Error(ErrorCode::resource_exhausted, "market request queue is full or stopped");
  return future;
}
void Session::stop() {
  loop_->requests.close();
  loop_->controls.close();
  loop_->thread.request_stop();
}
std::shared_future<void> Session::stopped() const {
  return loop_->stopped;
}
} // namespace asterion::market_data
