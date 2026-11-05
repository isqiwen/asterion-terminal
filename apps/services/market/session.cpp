#include "session.hpp"
#include "catalog_job.hpp"
#include "ctp_feed.hpp"
#include "recording/recorder.hpp"
#include "recording/compaction.hpp"
#include "recording/compactor.hpp"
#include "warehouse/reader.hpp"
#include "warehouse/research.hpp"
#include "acquisition/manager.hpp"
#include "acquisition/tushare.hpp"
#include "acquisition/exchanges.hpp"
#include "acquisition/recorded.hpp"
#include "acquisition/recorded_publication.hpp"
#include <asterion/futures/intraday_bars.hpp>
#include <asterion/base/bounded_queue.hpp>
#include <asterion/protocol/common.hpp>
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
namespace wire = ::market;
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
    const std::string instance;
    const std::filesystem::path directory, sdk, catalog_sdk;
    ThreadPool sdk_owner{1, 2};
    std::unique_ptr<ctp::Feed> feed;
    CatalogJob catalog;
    std::uint64_t applied_catalog = 0;
    IntradayBars intraday;
    const Settings settings;
    std::unique_ptr<recording::Recorder> recorder;
    const std::int64_t started_ms = now_ms();
    std::int64_t last_quote_ms = 0;
    std::uint64_t out_of_order = 0;
    MarketPhase seen = MarketPhase::disconnected;
    int feed_error = 0;
    std::uint32_t subscribed = 0;
    bool catalog_wanted = false;
    Clock::time_point next_login{}, next_catalog{}, quiet_since = Clock::now();
    recording::Compactor compactor;
    acquisition::Manager& acquisitions;
    // Every contract's minutes of a merged day go to the recorder source, and
    // so to research. A merge after late quotes publishes the day again.
    acquisition::RecordedPublication publication;

    State(
        const Session::Configuration& config,
        std::shared_ptr<std::shared_mutex> archive,
        acquisition::Manager& manager)
        : instance(config.instance),
          directory(config.settings.directory),
          sdk(config.settings.ctp_library),
          catalog_sdk(config.settings.ctp_catalog_library),
          catalog(sdk_owner),
          settings(config.settings),
          compactor(directory / "recordings", std::move(archive), config.settings.tick_days),
          acquisitions(manager),
          publication(directory / "recordings" / "unpublished.json", manager)
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

    ::market::Feed feed_status() const
    {
        auto out = protocol::encode_feed(feed->phase(), feed_error);
        if (sdk.empty())
            out.set_state(wire::FEED_STATE_NO_SDK);
        return out;
    }

    void advance()
    {
        catalog.poll();
        advance_login();
        const auto [catalog_revision, catalog_state] = catalog.snapshot();
        if (catalog_state.status().state() == wire::CATALOG_STATE_FAILED
            && next_catalog == Clock::time_point{})
            next_catalog = Clock::now() + 5min;
        if (feed && feed->phase() == MarketPhase::connected
            && (catalog_state.status().state() == wire::CATALOG_STATE_READY
                || (!applied_catalog && catalog_state.contracts_size()))
            && applied_catalog != catalog_revision) {
            std::map<InstrumentId, int> multipliers;
            for (const auto& row : catalog_state.contracts())
                multipliers[protocol::instrument(row.instrument())] = row.multiplier();
            feed->set_multipliers(std::move(multipliers));
            auto ids = interests(catalog_state);
            feed->subscribe(ids);
            subscribed = static_cast<std::uint32_t>(ids.size());
            applied_catalog = catalog_revision;
        }
        if (!feed)
            return;
        const auto update = feed->poll();
        for (const auto& event : update.events) {
            if (const auto* status = std::get_if<ctp::FeedStatus>(&event)) {
                feed_error = status->error_code;
                if (status->phase != MarketPhase::connected) {
                    intraday.interrupt();
                    if (recorder)
                        recorder->interrupt(market_phase_name(status->phase));
                }
            } else if (const auto* quote = std::get_if<MarketQuoteObservation>(&event)) {
                last_quote_ms = quote->quote.received_ms;
                out_of_order += quote->out_of_order;
                quiet_since = Clock::now();
                if (recorder)
                    recorder->observe(quote->quote, quote->out_of_order, intraday);
                if (!quote->out_of_order)
                    intraday.observe(quote->quote);
            }
        }
        // A feed that lost provider events never recovers in place: exit, and
        // systemd starts a complete stream again.
        if (update.failed) {
            intraday.interrupt();
            recorder->interrupt("feed_failed");
            throw std::runtime_error("market feed lost provider events");
        }
        if (recorder) {
            recorder->poll(now_ms(), intraday);
            const auto now = Clock::now();
            if (const auto day = compactor
                    .poll(now - quiet_since >= 30min, now, std::chrono::system_clock::now()))
                publication.add(*day);
            publication.poll(now);
        }
    }

    std::vector<InstrumentId> interests(const wire::Catalog& state)
    {
        std::vector<InstrumentId> ids;
        std::set<std::string> symbols;
        for (const auto& row : state.contracts())
            if (symbols.insert(row.instrument().symbol()).second)
                ids.push_back(protocol::instrument(row.instrument()));
        return ids;
    }

    wire::QuoteFrame quotes(WatchCursor& held, const wire::StreamQuotes& selection)
    {
        const auto [catalog_revision, catalog_state] = catalog.snapshot();
        const auto forced =
            held.catalog ? intraday.changed_after(held.intraday) : std::vector<InstrumentId>{};
        const auto state = feed->snapshot(held.feed, forced);
        auto board = protocol::encode_board(state);
        *board.mutable_feed() = feed_status();
        if (!selection.instruments().empty()) {
            std::set<InstrumentId> selected;
            for (const auto& id : selection.instruments())
                selected.insert(protocol::instrument(id));
            auto& rows = *board.mutable_rows();
            rows.erase(
                std::remove_if(
                    rows.begin(),
                    rows.end(),
                    [&](const auto& row) {
                        return !selected.contains(protocol::instrument(row.instrument()));
                    }),
                rows.end());
        }
        for (auto& row : *board.mutable_rows())
            if (const auto change =
                    intraday.change_1m_percent(protocol::instrument(row.instrument())))
                *row.mutable_change_1m_percent() = protocol::decimal(*change);
        if (!state.subscriptions_delta || held.catalog != catalog_revision)
            *board.mutable_catalog() = catalog_state;
        wire::QuoteFrame frame;
        *(state.subscriptions_delta ? frame.mutable_changed() : frame.mutable_full()) =
            std::move(board);
        held.catalog = catalog_revision;
        held.feed = state.sequence;
        held.intraday = intraday.revision();
        return frame;
    }

    wire::Status status() const
    {
        wire::Status out;
        out.set_version("0.1.0");
        out.set_started_ms(started_ms);
        *out.mutable_feed() = feed_status();
        *out.mutable_catalog() = catalog.snapshot().second.status();
        out.set_subscribed(subscribed);
        if (last_quote_ms)
            out.set_last_quote_ms(last_quote_ms);
        out.set_out_of_order_quotes(out_of_order);
        const auto& written = recorder->status();
        auto* recording = out.mutable_recording();
        recording->set_trading_day(protocol::iso_day(written.trading_day));
        recording->set_rows(written.rows);
        recording->set_files(written.files);
        recording->set_bytes(written.bytes);
        recording->set_lost_rows(written.lost);
        if (written.written_ms)
            recording->set_written_ms(written.written_ms);
        recording->set_disk_free_bytes(std::filesystem::space(directory).available);
        recording->set_write_error(written.error);
        const auto maintenance = compactor.status();
        recording->set_merging_day(protocol::iso_day(maintenance.day));
        recording->set_merge_error(maintenance.error);
        return out;
    }

    service::RpcHost::Message process(wire::Request request, WatchCursor* held)
    {
        wire::Response response;
        response.set_id(request.id());
        try {
            if (request.has_get_status()) {
                *response.mutable_get_status() = status();
            } else if (request.has_get_minutes()) {
                const auto id = protocol::instrument(request.get_minutes());
                id.validate();
                if (auto series = intraday.series(id))
                    *response.mutable_get_minutes() = protocol::encode_minutes(*series);
                else
                    *response.mutable_get_minutes()->mutable_instrument() = request.get_minutes();
            } else if (request.has_get_board()) {
                WatchCursor fresh;
                *response.mutable_get_board() = std::move(*quotes(fresh, {}).mutable_full());
            } else if (request.has_stream_quotes()) {
                if (!held->catalog) {
                    std::vector<InstrumentId> ids;
                    for (const auto& id : request.stream_quotes().instruments())
                        ids.push_back(protocol::instrument(id));
                    if (ids.size() > 50)
                        throw std::invalid_argument("at most 50 watch contracts");
                    ctp::validate_instruments(ids);
                }
                *response.mutable_stream_quotes() = quotes(*held, request.stream_quotes());
            } else {
                throw std::invalid_argument("missing market operation");
            }
        } catch (const Error& error) {
            *response.mutable_error() = protocol::error(error);
        } catch (const std::exception& error) {
            *response.mutable_error() = protocol::error(error);
            if (classify(error) != ErrorCode::invalid_request)
                response.mutable_error()->set_message("Market request failed; check service logs");
        }
        protocol::log_rpc_result("market", request, response, true);
        return {
            response.SerializeAsString(),
            request.has_stream_quotes() && response.has_stream_quotes()
        };
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
    if (!request.has_get_status() && !request.has_stream_quotes() && !request.has_get_minutes()
        && !request.has_get_board()) {
        auto promise = std::make_shared<std::promise<service::RpcHost::Message>>();
        auto future = promise->get_future();
        loop_->reads.submit([loop = loop_.get(), request = std::move(request), promise, can_manage](
                                std::stop_token) {
            wire::Response response;
            response.set_id(request.id());
            try {
                const auto& directory = loop->config.settings.directory;
                const warehouse::Reader datasets(directory);
                auto& tasks = loop->acquisitions;
                switch (request.call_case()) {
                    case wire::Request::kListDatasets:
                        *response.mutable_list_datasets() = datasets.datasets();
                        break;
                    case wire::Request::kGetDatasetVersion:
                        *response.mutable_get_dataset_version() =
                            datasets.version(request.get_dataset_version());
                        break;
                    case wire::Request::kReadDatasetFile:
                        *response.mutable_read_dataset_file() =
                            datasets.file(request.read_dataset_file());
                        break;
                    case wire::Request::kGetDatasetSummary:
                        *response.mutable_get_dataset_summary() =
                            datasets.summary(request.get_dataset_summary());
                        break;
                    case wire::Request::kGetDatasetCoverage:
                        *response.mutable_get_dataset_coverage() =
                            datasets.coverage(request.get_dataset_coverage());
                        break;
                    case wire::Request::kCompareMinutes:
                        *response.mutable_compare_minutes() = warehouse::compare_minutes(directory);
                        break;
                    case wire::Request::kGetMinuteDifferences:
                        *response.mutable_get_minute_differences() = warehouse::minute_differences(
                            directory,
                            request.get_minute_differences());
                        break;
                    case wire::Request::kListSources:
                        *response.mutable_list_sources() = tasks.sources();
                        response.mutable_list_sources()->set_can_manage(can_manage);
                        break;
                    case wire::Request::kStartAcquisition:
                        *response.mutable_start_acquisition() =
                            tasks.submit(request.start_acquisition());
                        break;
                    case wire::Request::kGetAcquisitionTask:
                        *response.mutable_get_acquisition_task() =
                            tasks.get(request.get_acquisition_task().id());
                        break;
                    case wire::Request::kListAcquisitionTasks:
                        *response.mutable_list_acquisition_tasks() =
                            tasks.list(request.list_acquisition_tasks());
                        break;
                    case wire::Request::kCancelAcquisition:
                        tasks.cancel(request.cancel_acquisition().id());
                        *response.mutable_cancel_acquisition() =
                            tasks.get(request.cancel_acquisition().id());
                        break;
                    default:
                        throw std::invalid_argument("missing market operation");
                }
            } catch (const std::exception& error) {
                *response.mutable_error() = protocol::error(error);
            }
            protocol::log_rpc_result(
                "market",
                request,
                response,
                !request.has_start_acquisition() && !request.has_cancel_acquisition());
            promise->set_value({response.SerializeAsString()});
            service::wake_io_owner();
        });
        return future;
    }
    const bool control = request.has_get_status();
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
