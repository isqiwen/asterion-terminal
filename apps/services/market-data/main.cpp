#include "ctp_feed.hpp"
#include "catalog_job.hpp"
#include <CLI/CLI.hpp>
#include <asterion/domain/intraday_bars.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/service_host.hpp>
#include <asterion/protocol/market.hpp>
#include <asterion/protocol/trading.hpp>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <map>
#include <set>
#include <thread>
#include <stdexcept>
using namespace asterion;
using namespace std::chrono_literals;
namespace wire = asterion::market::v1;
int main(int argc, char** argv) {
  CLI::App app{"Asterion read-only live market-data host"};
  app.set_version_flag("--version", "Asterion Market Data 0.1.0");
  std::string service, health_endpoint, directory, sdk, catalog_sdk;
  std::uint64_t owner_pid = 0;
  service::Transport transport;
  app.add_option("--session", service)->required();
  app.add_option("--directory", directory)->required()->check(CLI::ExistingDirectory);
  app.add_option("--endpoint", transport.endpoint);
  app.add_option("--health-endpoint", health_endpoint);
  app.add_option("--bind", transport.bind);
  app.add_option("--port", transport.port);
  app.add_option("--tls-ca", transport.tls.ca_file);
  app.add_option("--tls-cert", transport.tls.certificate_file);
  app.add_option("--tls-key", transport.tls.private_key_file);
  app.add_option("--ctp-library", sdk);
  app.add_option("--ctp-catalog-library", catalog_sdk);
  app.add_option("--owner-pid", owner_pid);
  argv = app.ensure_utf8(argv);
  CLI11_PARSE(app, argc, argv);
  try {
    validate_id(service);
    transport.validate();
    const auto instance = unique_process_id();
    service::install_stop_signals();
    service::OwnerWatch owner(owner_pid);
    PluginManager plugins;
    ctp::Feed* feed = nullptr;
    std::mutex mutex;
    bool quiescing = false;
    market_data::CatalogJob catalog;
    std::vector<InstrumentId> watchlist;
    std::uint64_t applied_catalog = 0, extra_sequence = 0;
    // Minute bars and one-minute change from the ordered event stream, not
    // from coalesced snapshots. Lock order: `mutex` before `intraday_mutex`.
    std::mutex intraday_mutex;
    IntradayBars intraday;
    auto interests = [&](const wire::CatalogState& state) {
      std::vector<InstrumentId> ids = watchlist;
      std::set<std::string> symbols;
      for (const auto& id : ids)
        symbols.insert(id.symbol);
      if (state.phase() == "ready")
        for (const auto& row : state.contracts())
          if (symbols.insert(row.instrument().symbol()).second)
            ids.push_back({row.instrument().venue(), row.instrument().symbol()});
      return ids;
    };
    auto snapshot = [&] {
      std::lock_guard lock(mutex);
      const auto [catalog_revision, catalog_state] = catalog.snapshot();
      LiveMarketSnapshot state;
      if (feed)
        state = feed->snapshot();
      else
        state.phase = sdk.empty() ? "sdk_unavailable" : "disconnected";
      if (feed && state.phase == "connected" && catalog_state.phase() == "ready" &&
          applied_catalog != catalog_revision) {
        std::map<InstrumentId, int> multipliers;
        for (const auto& row : catalog_state.contracts())
          multipliers[{row.instrument().venue(), row.instrument().symbol()}] = row.multiplier();
        feed->set_multipliers(std::move(multipliers));
        feed->subscribe(interests(catalog_state));
        applied_catalog = catalog_revision;
        state = feed->snapshot();
      }
      auto out = protocol::encode_market(state, instance);
      out.set_sequence(state.sequence + catalog_revision + extra_sequence);
      *out.mutable_catalog() = catalog_state;
      for (const auto& id : watchlist) {
        auto* row = out.add_watchlist();
        row->set_venue(id.venue);
        row->set_symbol(id.symbol);
      }
      std::lock_guard bars(intraday_mutex);
      for (auto& row : *out.mutable_subscriptions())
        if (const auto change =
                intraday.change_1m_percent({row.instrument().venue(), row.instrument().symbol()}))
          row.set_change_1m_percent(change->str());
      return out;
    };
    std::jthread aggregator([&](std::stop_token stop) {
      std::string stream;
      std::uint64_t cursor = 0;
      while (!stop.stop_requested()) {
        ctp::Feed* source = nullptr;
        {
          std::lock_guard lock(mutex);
          source = feed;
        }
        try {
          for (bool more = source != nullptr; more && !stop.stop_requested();) {
            const auto batch = source->events_after(stream, cursor, 1024);
            std::lock_guard bars(intraday_mutex);
            if (batch.gap || batch.failed)
              intraday.interrupt();
            for (const auto& event : batch.events) {
              cursor = event.sequence;
              if (const auto* status = std::get_if<LiveMarketSnapshot>(&event.value)) {
                if (status->phase != "connected")
                  intraday.interrupt();
              } else if (const auto& quote = std::get<MarketQuoteObservation>(event.value);
                         !quote.out_of_order)
                intraday.observe(quote.quote);
            }
            stream = batch.stream_id;
            if (batch.failed)
              cursor = batch.latest_sequence;
            more = batch.events.size() == 1024;
          }
        } catch (const std::exception&) {
          // An invalid cursor restarts from the retained stream and marks loss.
          std::lock_guard bars(intraday_mutex);
          intraday.interrupt();
          stream.clear();
          cursor = 0;
        }
        std::this_thread::sleep_for(100ms);
      }
    });
    auto serve = [&](service::Connection& channel, std::stop_token token) {
      try {
        wire::Request request;
        if (!request.ParseFromString(channel.receive(2s)))
          return;
        protocol::validate_message(request);
        wire::Response response;
        response.set_version(1);
        response.set_service_id(service);
        response.set_correlation_id(request.correlation_id());
        try {
          validate_id(request.correlation_id());
          if (request.version() != 1 || request.service_id() != service)
            throw std::invalid_argument("market service identity mismatch");
          if (request.has_heartbeat()) {
            auto* h = response.mutable_health();
            h->set_instance_id(instance);
            h->set_phase(snapshot().phase());
          } else if (request.has_events()) {
            std::lock_guard lock(mutex);
            if (!feed)
              throw Error(ErrorCode::unavailable, "market event stream not started");
            const auto& read = request.events();
            *response.mutable_events() = protocol::encode_market_events(
                feed->events_after(read.stream_id(), read.after_sequence(), read.limit()));
          } else if (request.has_minutes()) {
            const InstrumentId id{request.minutes().instrument().venue(),
                                  request.minutes().instrument().symbol()};
            id.validate();
            std::lock_guard bars(intraday_mutex);
            if (auto series = intraday.series(id))
              *response.mutable_minutes() = protocol::encode_minutes(*series);
            else
              *response.mutable_minutes()->mutable_instrument() = request.minutes().instrument();
          } else if (request.has_watch()) {
            // Coalesced quote snapshots; intermediate ticks are not a
            // historical tick archive.
            while (!token.stop_requested()) {
              *response.mutable_snapshot() = snapshot();
              channel.send(response.SerializeAsString(), 2s);
              std::this_thread::sleep_for(250ms);
            }
            return;
          } else {
            {
              std::lock_guard lock(mutex);
              if (quiescing && !request.has_snapshot())
                throw Error(ErrorCode::unavailable, "market service is preparing for upgrade");
              if (request.has_connect()) {
                if (feed && feed->snapshot().phase != "disconnected" &&
                    feed->snapshot().phase != "error")
                  throw Error(ErrorCode::conflict, "disconnect current market session first");
                if (!feed) {
                  auto plugin = std::make_unique<ctp::Feed>(
                      std::filesystem::path(std::u8string(sdk.begin(), sdk.end())),
                      std::filesystem::path(std::u8string(directory.begin(), directory.end())) /
                          "ctp-flow");
                  feed = plugin.get();
                  plugins.add(std::move(plugin));
                  plugins.start();
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
                const auto previous = watchlist;
                watchlist = ids;
                try {
                  feed->subscribe(interests(catalog.snapshot().second));
                } catch (...) {
                  watchlist = previous;
                  throw;
                }
                ++extra_sequence;
              } else if (request.has_disconnect()) {
                catalog.cancel();
                applied_catalog = 0;
                if (feed)
                  feed->disconnect();
              } else if (!request.has_snapshot())
                throw std::invalid_argument("missing market operation");
            }
            *response.mutable_snapshot() = snapshot();
          }
        } catch (const Error& e) {
          response.mutable_error()->set_code(std::string(error_name(e.code())));
          response.mutable_error()->set_message(e.what());
        } catch (const std::exception& e) {
          // Provider diagnostics may echo configuration; only the code crosses.
          response.mutable_error()->set_code(std::string(error_name(classify(e))));
          response.mutable_error()->set_message(
              "Market request rejected; check configuration and SDK "
              "availability");
        }
        channel.send(response.SerializeAsString(), 2s);
      } catch (const std::exception&) { /* Bounded peer failure never stops the managed feed. */
      }
    };
    service::HealthChannel health(health_endpoint, [&](const std::string& frame) {
      wire::Request request;
      if (!request.ParseFromString(frame))
        return std::string();
      protocol::validate_message(request);
      validate_id(request.correlation_id());
      if (request.version() != 1 || request.service_id() != service ||
          (!request.has_heartbeat() && !request.has_quiesce()))
        return std::string();
      wire::Response response;
      response.set_version(1);
      response.set_service_id(service);
      response.set_correlation_id(request.correlation_id());
      if (request.has_quiesce()) {
        std::lock_guard lock(mutex);
        const auto phase = feed ? feed->snapshot().phase : "disconnected";
        if (catalog.running() || (phase != "disconnected" && phase != "sdk_unavailable")) {
          response.mutable_error()->set_code("unavailable");
          response.mutable_error()->set_message(
              "market connection must be disconnected before upgrade");
          return response.SerializeAsString();
        }
        quiescing = true;
        if (request.quiesce().stop())
          service::request_stop();
      }
      auto* h = response.mutable_health();
      h->set_instance_id(instance);
      h->set_phase(snapshot().phase());
      return response.SerializeAsString();
    });
    // Watch requests stream until the client leaves or the service stops, so
    // concurrency is bounded by workers and a full host rejects immediately.
    service::HostOptions options;
    options.drain = 10s;
    options.workers = 16;
    options.queue = 0;
    options.handshake = 2s;
    service::ServiceHost host(transport, serve, options);
    if (!host.run())
      std::_Exit(0);
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
  return 0;
}
