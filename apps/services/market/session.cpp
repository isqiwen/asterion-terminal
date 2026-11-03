#include "session.hpp"
#include "catalog_job.hpp"
#include "ctp_feed.hpp"
#include "recording/recorder.hpp"
#include "recording/reader.hpp"
#include "recording/compaction.hpp"
#include "recording/compactor.hpp"
#include "warehouse/reader.hpp"
#include "acquisition/manager.hpp"
#include "acquisition/tushare.hpp"
#include "acquisition/exchanges.hpp"
#include "acquisition/recorded.hpp"
#include <asterion/futures/intraday_bars.hpp>
#include <asterion/base/bounded_queue.hpp>
#include <asterion/protocol/market.hpp>
#include <asterion/protocol/rpc_diagnostics.hpp>
#include <asterion/protocol/trading.hpp>
#include <algorithm>
#include <map>
#include <deque>
#include <asterion/system/process/artifact.hpp>
#include <asterion/system/process/child.hpp>
#include <set>
#include <thread>

namespace asterion::market_data {

using namespace std::chrono_literals;
namespace wire = market::proto;
using Clock = std::chrono::steady_clock;

static std::int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
        .count();
}

namespace {

struct State
{
    const std::string service, instance;
    const std::filesystem::path directory, sdk, catalog_sdk;
    ThreadPool sdk_owner{1, 2};
    std::unique_ptr<ctp::Feed> feed;
    CatalogJob catalog;
    std::uint64_t applied_catalog = 0;
    IntradayBars intraday;
    std::string stream;
    std::uint64_t event_cursor = 0;
    const Settings settings;
    std::unique_ptr<recording::Recorder> recorder;
    const std::int64_t started_ms = now_ms();
    std::int64_t last_quote_ms = 0;
    MarketPhase seen = MarketPhase::disconnected;
    int feed_error = 0;
    std::uint32_t subscribed = 0;
    bool catalog_wanted = false;
    Clock::time_point next_login{}, next_catalog{}, quiet_since = Clock::now();
    recording::Compactor compactor;
    acquisition::Manager& acquisitions;
    // Merged trading days (YYYYMMDD) whose recorded minutes are still to be
    // published; tried again each minute while acquisition is busy.
    std::set<std::string> unpublished;
    Clock::time_point next_publish{};

    State(
        const Session::Configuration& config,
        std::shared_ptr<std::shared_mutex> archive,
        acquisition::Manager& manager)
        : service(config.service),
          instance(config.instance),
          directory(config.settings.directory),
          sdk(config.settings.ctp_library),
          catalog_sdk(config.settings.ctp_catalog_library),
          catalog(sdk_owner),
          settings(config.settings),
          compactor(directory / "recordings", std::move(archive), config.settings.tick_days),
          acquisitions(manager)
    {
        catalog.persist_to(directory / "ctp-catalog.pb");
        {
            recorder = std::make_unique<recording::Recorder>(directory / "recordings", instance);
            feed = std::make_unique<ctp::Feed>(sdk_owner, sdk, directory / "ctp-flow");
            feed->start();
            login();
        }
    }

    ~State()
    {
        if (feed)
            feed->stop();
        if (recorder) {
            try {
                recorder->close("stopped", now_ms(), intraday);
            } catch (...) {
                // The writer reports the failure; provider teardown must finish.
            }
        }
    }

    void login()
    {
        feed->connect(settings.market, {});
        applied_catalog = 0;
        subscribed = 0;
        next_login = Clock::now() + 60s;
    }

    void advance_login()
    {
        const auto now = Clock::now();
        if (feed->phase() == MarketPhase::connected && seen != MarketPhase::connected)
            catalog_wanted = true;
        seen = feed->phase();
        if (seen == MarketPhase::error && now >= next_login)
            login();
        if ((catalog_wanted || (next_catalog != Clock::time_point{} && now >= next_catalog))
            && !catalog.running() && !catalog.initializing()) {
            catalog.start(catalog_sdk, directory / "ctp-catalog-flow", settings.catalog);
            catalog_wanted = false;
            next_catalog = {};
        }
    }

    std::string phase() const
    {
        if (catalog.initializing())
            return "initializing";
        if (feed)
            return std::string(market_phase_name(feed->phase()));
        return sdk.empty() ? "sdk_unavailable" : "disconnected";
    }

    void advance()
    {
        if (feed)
            feed->poll();
        catalog.poll();
        advance_login();
        const auto [catalog_revision, catalog_state] = catalog.snapshot();
        if (catalog_state.phase() == "error" && next_catalog == Clock::time_point{})
            next_catalog = Clock::now() + 5min;
        if (feed && feed->phase() == MarketPhase::connected
            && (catalog_state.phase() == "ready"
                || (!applied_catalog && catalog_state.contracts_size()))
            && applied_catalog != catalog_revision) {
            std::map<InstrumentId, int> multipliers;
            for (const auto& row : catalog_state.contracts())
                multipliers[{row.instrument().venue(), row.instrument().symbol()}] =
                    row.multiplier();
            feed->set_multipliers(std::move(multipliers));
            auto ids = interests(catalog_state);
            feed->subscribe(ids);
            subscribed = static_cast<std::uint32_t>(ids.size());
            applied_catalog = catalog_revision;
        }
        if (!feed)
            return;
        // Bounded work per turn. Client traffic cannot prevent event consumption,
        // and a continuous provider stream cannot starve admitted commands.
        const auto batch = feed->events_after(stream, event_cursor, 1024);
        if (batch.gap || batch.failed) {
            intraday.interrupt();
            if (recorder)
                recorder->interrupt(batch.failed ? "feed_failed" : "events_lost");
        }
        for (const auto& event : batch.events) {
            event_cursor = event.sequence;
            if (const auto* status = std::get_if<LiveMarketSnapshot>(&event.value)) {
                feed_error = status->error_code;
                if (status->phase != MarketPhase::connected) {
                    intraday.interrupt();
                    if (recorder)
                        recorder->interrupt(market_phase_name(status->phase));
                }
            } else if (const auto* quote = std::get_if<MarketQuoteObservation>(&event.value)) {
                last_quote_ms = quote->quote.received_ms;
                quiet_since = Clock::now();
                if (recorder)
                    recorder->observe(quote->quote, quote->out_of_order, intraday);
                if (!quote->out_of_order)
                    intraday.observe(quote->quote);
            }
        }
        stream = batch.stream_id;
        // A feed that lost provider events never recovers in place: exit, and
        // systemd starts a complete stream again.
        if (batch.failed)
            throw std::runtime_error("market feed lost provider events");
        if (recorder) {
            recorder->poll(now_ms(), intraday);
            const auto now = Clock::now();
            if (const auto day = compactor
                    .poll(now - quiet_since >= 30min, now, std::chrono::system_clock::now()))
                unpublished.insert(*day);
            publish_recorded(now);
        }
    }

    // Every contract's minutes of a merged day go to the recorder source, and
    // so to research. A merge after late quotes publishes the day again.
    void publish_recorded(Clock::time_point now)
    {
        if (unpublished.empty() || now < next_publish)
            return;
        next_publish = now + 1min;
        const auto day = *unpublished.begin();
        asterion::acquisition::proto::Request request;
        request.set_id("recorded-" + day + "-" + unique_process_id().substr(0, 8));
        request.set_provider("recorder");
        request.set_table("minutes");
        request.set_begin_day(day.substr(0, 4) + "-" + day.substr(4, 2) + "-" + day.substr(6));
        request.set_end_day(request.begin_day());
        request.set_refresh(true);
        try {
            acquisitions.submit(request);
            unpublished.erase(day);
        } catch (const Error& error) {
            // Another task of the recorder, or both workers, are busy.
            if (error.code() != ErrorCode::conflict
                && error.code() != ErrorCode::resource_exhausted)
                throw;
        }
    }

    std::vector<InstrumentId> interests(const wire::CatalogState& state)
    {
        std::vector<InstrumentId> ids;
        std::set<std::string> symbols;
        for (const auto& row : state.contracts())
            if (symbols.insert(row.instrument().symbol()).second)
                ids.push_back({row.instrument().venue(), row.instrument().symbol()});
        return ids;
    }

    wire::Snapshot snapshot(WatchCursor* held = nullptr, const wire::Watch* watch = nullptr)
    {
        const auto [catalog_revision, catalog_state] = catalog.snapshot();
        LiveMarketSnapshot state;
        if (feed) {
            const auto forced =
                held ? intraday.changed_after(held->intraday) : std::vector<InstrumentId>{};
            state = feed->snapshot(held ? held->feed : std::nullopt, forced);
        }
        auto out = protocol::encode_market(state, instance);
        if (!feed && sdk.empty())
            out.set_phase("sdk_unavailable");
        out.set_sequence(state.sequence + catalog_revision + intraday.revision());
        if (state.subscriptions_delta)
            out.set_base_sequence(held->sequence);
        out.set_catalog_revision(catalog_revision);
        if (held && held->catalog == catalog_revision)
            out.set_catalog_omitted(true);
        else
            *out.mutable_catalog() = catalog_state;
        if (watch && !watch->instruments().empty()) {
            std::set<InstrumentId> selected;
            for (const auto& id : watch->instruments()) {
                selected.insert({id.venue(), id.symbol()});
                *out.add_watchlist() = id;
            }
            auto& rows = *out.mutable_subscriptions();
            rows.erase(
                std::remove_if(
                    rows.begin(),
                    rows.end(),
                    [&](const auto& row) {
                        return !selected.contains(
                            {row.instrument().venue(), row.instrument().symbol()});
                    }),
                rows.end());
        }
        for (auto& row : *out.mutable_subscriptions())
            if (const auto change = intraday.change_1m_percent(
                    {row.instrument().venue(), row.instrument().symbol()}))
                row.set_change_1m_percent(change->str());
        if (held) {
            held->catalog = catalog_revision;
            held->feed = feed ? std::optional(state.sequence) : std::nullopt;
            held->intraday = intraday.revision();
            held->sequence = out.sequence();
        }
        return out;
    }

    service::RpcHost::Message process(wire::Request request, WatchCursor* held)
    {
        wire::Response response;
        response.set_version(1);
        response.set_service_id(service);
        response.set_correlation_id(request.correlation_id());
        try {
            if (request.has_recording_status()) {
                if (!recorder)
                    throw Error(ErrorCode::unavailable, "this market is not recording");
                const auto& written = recorder->status();
                const auto& state = catalog.snapshot().second;
                auto* out = response.mutable_recording_status();
                out->set_instance(instance);
                out->set_version("0.1.0");
                out->set_started_ms(started_ms);
                out->set_phase(phase());
                out->set_error_code(feed_error);
                out->set_catalog_phase(state.phase());
                out->set_catalog_trading_day(state.trading_day());
                out->set_catalog_error(state.error_code());
                out->set_subscribed(subscribed);
                out->set_last_quote_ms(last_quote_ms);
                out->set_trading_day(written.trading_day);
                out->set_rows(written.rows);
                out->set_files(written.files);
                out->set_bytes(written.bytes);
                out->set_lost(written.lost);
                out->set_write_error(written.error);
                out->set_written_ms(written.written_ms);
                out->set_disk_free_bytes(std::filesystem::space(directory).available);
                const auto maintenance = compactor.status();
                out->set_merging(maintenance.day);
                out->set_merge_error(maintenance.error);
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
                const InstrumentId id{
                    request.minutes().instrument().venue(),
                    request.minutes().instrument().symbol()
                };
                id.validate();
                if (auto series = intraday.series(id))
                    *response.mutable_minutes() = protocol::encode_minutes(*series);
                else
                    *response.mutable_minutes()->mutable_instrument() =
                        request.minutes().instrument();
            } else if (request.has_watch()) {
                if (!held || !held->catalog) {
                    std::vector<InstrumentId> ids;
                    for (const auto& id : request.watch().instruments())
                        ids.push_back({id.venue(), id.symbol()});
                    if (ids.size() > 50)
                        throw std::invalid_argument("at most 50 watch contracts");
                    ctp::validate_instruments(ids);
                }
                *response.mutable_snapshot() = snapshot(held, &request.watch());
            } else {
                if (!request.has_snapshot())
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
        protocol::log_rpc_result(
            "market-data",
            request,
            response,
            request.has_snapshot() || request.has_heartbeat() || request.has_watch());
        return {response.SerializeAsString(), request.has_watch() && response.has_snapshot()};
    }
};

} // namespace

static std::map<std::string, acquisition::Factory>
sources(const Settings& settings, std::shared_ptr<std::shared_mutex> archive)
{
    std::map<std::string, acquisition::Factory> result;
    result["recorder"] = [root = settings.directory / "recordings", archive](std::stop_token stop) {
        return std::make_unique<acquisition::Recorded>(root, archive, stop);
    };
    for (const auto& exchange : settings.exchanges) {
        auto id = exchange;
        std::transform(id.begin(), id.end(), id.begin(), [](unsigned char c) {
            return std::tolower(c);
        });
        result[id] = [exchange](std::stop_token stop) {
            return std::make_unique<acquisition::Exchange>(exchange, stop);
        };
    }
    if (!settings.tushare_token.empty()) {
        auto budget = std::make_shared<acquisition::Budget>(settings.tushare_requests_per_minute);
        result["tushare"] = [token = settings.tushare_token, budget](std::stop_token stop) {
            return std::make_unique<acquisition::Tushare>(token, budget, stop);
        };
    }
    return result;
}

struct Session::Loop
{
    const Configuration config;
    std::shared_ptr<std::shared_mutex> archive = std::make_shared<std::shared_mutex>();
    std::unique_ptr<recording::Reader> reader;
    acquisition::Manager acquisitions;
    ThreadPool reads{1, 8};

    using Work = std::packaged_task<service::RpcHost::Message(State&)>;
    BoundedQueue<Work> requests{64}, controls{4};
    std::promise<void> completion;
    std::shared_future<void> stopped = completion.get_future().share();
    std::jthread thread;

    explicit Loop(Configuration config)
        : config(config),
          acquisitions(config.settings.directory, sources(config.settings, archive))
    {
        // Recovery finishes before any client or publication can read the archive.
        recording::finish_compactions(config.settings.directory / "recordings");
        thread = std::jthread([this, config = std::move(config)](std::stop_token stop) {
            try {
                {
                    State state(config, archive, acquisitions);
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
                    acquisitions.stop();
                } // Join provider resources on their owner before reporting completion.
                completion.set_value();
            } catch (...) {
                completion.set_exception(std::current_exception());
            }
            requests.close();
            controls.close();
        });
    }
};

Session::Session(Configuration config)
    : loop_(std::make_unique<Loop>(std::move(config)))
{}

Session::~Session() = default;

std::future<service::RpcHost::Message>
Session::request(wire::Request request, std::shared_ptr<WatchCursor> cursor, bool can_manage)
{
    if (request.has_recorded_days() || request.has_recorded_minutes() || request.has_datasets()
        || request.has_dataset() || request.has_dataset_file() || request.has_dataset_summary()
        || request.has_acquisition_task() || request.has_acquisition_tasks()
        || request.has_acquisition_start() || request.has_acquisition_cancel()
        || request.has_acquisition_sources() || request.has_dataset_coverage()) {
        auto promise = std::make_shared<std::promise<service::RpcHost::Message>>();
        auto future = promise->get_future();
        loop_->reads.submit([loop = loop_.get(), request = std::move(request), promise, can_manage](
                                std::stop_token) {
            wire::Response response;
            response.set_version(1);
            response.set_service_id(loop->config.service);
            response.set_correlation_id(request.correlation_id());
            try {
                const warehouse::Reader datasets(loop->config.settings.directory);
                if (request.has_acquisition_sources()) {
                    *response.mutable_acquisition_sources() = loop->acquisitions.sources();
                    response.mutable_acquisition_sources()->set_can_manage(can_manage);
                } else if (request.has_dataset_coverage())
                    *response.mutable_dataset_coverage() =
                        datasets.coverage(request.dataset_coverage());
                else if (request.has_acquisition_start())
                    *response.mutable_acquisition_task() =
                        loop->acquisitions.submit(request.acquisition_start());
                else if (request.has_acquisition_cancel()) {
                    loop->acquisitions.cancel(request.acquisition_cancel().id());
                    *response.mutable_acquisition_task() =
                        loop->acquisitions.get(request.acquisition_cancel().id());
                } else if (request.has_acquisition_task() || request.has_acquisition_tasks()) {
                    if (request.has_acquisition_task())
                        *response.mutable_acquisition_task() =
                            loop->acquisitions.get(request.acquisition_task().id());
                    else
                        *response.mutable_acquisition_tasks() =
                            loop->acquisitions.list(request.acquisition_tasks());
                } else if (request.has_datasets())
                    *response.mutable_datasets() = datasets.sources(request.datasets());
                else if (request.has_dataset())
                    *response.mutable_dataset() = datasets.version(request.dataset());
                else if (request.has_dataset_file())
                    *response.mutable_dataset_file() = datasets.file(request.dataset_file());
                else if (request.has_dataset_summary())
                    *response.mutable_dataset_summary() =
                        datasets.summary(request.dataset_summary());
                else {
                    std::shared_lock lock(*loop->archive);
                    if (!loop->reader)
                        loop->reader = std::make_unique<recording::Reader>(
                            loop->config.settings.directory / "recordings");
                    if (request.has_recorded_days())
                        *response.mutable_recorded_days() = loop->reader->days();
                    else
                        *response.mutable_recorded_minutes() =
                            loop->reader->minutes(request.recorded_minutes());
                }
            } catch (const std::exception& error) {
                response.mutable_error()->set_code(std::string(error_name(classify(error))));
                response.mutable_error()->set_message(error.what());
            }
            promise->set_value({response.SerializeAsString()});
            service::wake_io_owner();
        });
        return future;
    }
    const bool control = request.has_heartbeat();
    Loop::Work work(
        [request = std::move(request), cursor = std::move(cursor)](State& state) mutable {
            return state.process(std::move(request), cursor.get());
        });
    auto future = work.get_future();
    if (!(control ? loop_->controls : loop_->requests).try_push(std::move(work)))
        throw Error(ErrorCode::resource_exhausted, "market request queue is full or stopped");
    return future;
}

void Session::stop()
{
    loop_->requests.close();
    loop_->controls.close();
    loop_->thread.request_stop();
}

std::shared_future<void> Session::stopped() const
{
    return loop_->stopped;
}

} // namespace asterion::market_data
