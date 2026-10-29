#include "recording.hpp"
#include "catalog_job.hpp"
#include "compaction.hpp"
#include "ctp_feed.hpp"
#include <asterion/domain/intraday_bars.hpp>
#include <asterion/kernel/log.hpp>
#include <thread>

namespace asterion::recording {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

namespace {

std::int64_t now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// The catalog is read again after every login, so the contracts listed on a
// new trading day are recorded; a failed read is tried again five minutes
// later and the contracts already subscribed stay. A refused market login is
// tried again a minute later. Neither is a trading command.
struct State
{
    const Recording::Configuration config;
    ThreadPool sdk_owner{1, 2};
    ctp::Feed feed;
    market_data::CatalogJob catalog;
    IntradayBars minutes;
    Recorder recorder;
    std::vector<InstrumentId> contracts;
    std::uint64_t applied_catalog = 0, cursor = 0;
    std::string stream;
    MarketPhase seen = MarketPhase::disconnected;
    int error_code = 0;
    bool catalog_wanted = true;
    Clock::time_point next_login{}, next_catalog{};
    std::int64_t last_quote_ms = 0;

    // Merging waits for half an hour without quotes and runs on its own thread
    // with its own database: the recording loop never waits for it.
    struct Merge
    {
        std::optional<Compacted> done;
        std::string error;
    };

    Clock::time_point quiet_since = Clock::now(), next_merge{};
    std::string merging, merge_error;
    std::shared_ptr<Merge> merge;
    std::future<void> merge_finished;
    ThreadPool merger{1, 1};

    explicit State(Recording::Configuration configuration)
        : config(std::move(configuration)),
          feed(sdk_owner, config.settings.ctp_library, config.settings.directory / "ctp-flow"),
          catalog(sdk_owner),
          recorder(config.settings.directory / "recordings", config.instance)
    {
        finish_compactions(config.settings.directory / "recordings");
        catalog.persist_to(config.settings.directory / "ctp-catalog.pb");
        feed.start();
        login();
    }

    ~State()
    {
        feed.stop();
        try {
            recorder.close("stopped", now_ms(), minutes);
        } catch (...) {
            // A failed write was logged by the writer; stopping continues.
        }
    }

    void login()
    {
        feed.connect(config.settings.market, contracts);
        next_login = Clock::now() + 60s;
    }

    void advance()
    {
        feed.poll();
        catalog.poll();
        const auto now = Clock::now();
        const auto phase = feed.phase();
        if (phase == MarketPhase::connected && seen != MarketPhase::connected)
            catalog_wanted = true;
        if (phase == MarketPhase::error && now >= next_login) {
            log::warn("market login refused, retrying: error_code={}", error_code);
            login();
        }
        seen = phase;
        if ((catalog_wanted || (next_catalog != Clock::time_point{} && now >= next_catalog))
            && !catalog.running() && !catalog.initializing()) {
            catalog.start(
                config.settings.ctp_catalog_library,
                config.settings.directory / "ctp-catalog-flow",
                config.settings.catalog);
            catalog_wanted = false;
            next_catalog = {};
        }
        apply_catalog(now);
        // Bounded work per turn, so a stop request is seen during a busy market.
        const auto batch = feed.events_after(stream, cursor, 1024);
        if (batch.gap || batch.failed) {
            minutes.interrupt();
            recorder.interrupt(batch.failed ? "feed_failed" : "events_lost");
        }
        for (const auto& event : batch.events) {
            cursor = event.sequence;
            if (const auto* status = std::get_if<LiveMarketSnapshot>(&event.value)) {
                error_code = status->error_code;
                if (status->phase != MarketPhase::connected) {
                    minutes.interrupt();
                    recorder.interrupt(market_phase_name(status->phase));
                }
            } else if (const auto* quote = std::get_if<MarketQuoteObservation>(&event.value)) {
                last_quote_ms = quote->quote.received_ms;
                quiet_since = now;
                recorder.observe(quote->quote, quote->out_of_order, minutes);
                if (!quote->out_of_order)
                    minutes.observe(quote->quote);
            }
        }
        stream = batch.stream_id;
        if (batch.failed)
            cursor = batch.latest_sequence;
        recorder.poll(now_ms(), minutes);
        advance_merge(now);
    }

    void advance_merge(Clock::time_point now)
    {
        if (merge_finished.valid()) {
            if (merge_finished.wait_for(0s) != std::future_status::ready)
                return;
            merge_finished.get();
            if (merge->done) {
                merge_error.clear();
                log::info(
                    "merged trading day {}: ticks={} minutes={} files {} -> {}",
                    merging,
                    merge->done->ticks,
                    merge->done->minutes,
                    merge->done->files_before,
                    merge->done->files_after);
            } else {
                merge_error = merge->error;
                next_merge = now + 1h;
                log::error("merging trading day {} failed: {}", merging, merge->error);
            }
            merging.clear();
            return;
        }
        if (now < next_merge || now - quiet_since < 30min)
            return;
        next_merge = now + 1min;
        const auto days = compactable_days(
            config.settings.directory / "recordings",
            std::chrono::system_clock::now());
        if (days.empty())
            return;
        merging = days.front();
        merge = std::make_shared<Merge>();
        merge_finished = merger.submit(
            [root = config.settings.directory / "recordings", day = merging, merge = merge](
                std::stop_token stop) {
                try {
                    merge->done = compact_day(root, day, stop);
                } catch (const std::exception& error) {
                    merge->error = error.what();
                }
            });
    }

    void apply_catalog(Clock::time_point now)
    {
        const auto [revision, state] = catalog.snapshot();
        if (revision == applied_catalog)
            return;
        applied_catalog = revision;
        if (state.phase() == "error") {
            log::warn(
                "contract catalog failed: error_code={} subscribed={}",
                state.error_code(),
                contracts.size());
            next_catalog = now + 5min;
            return;
        }
        if (state.phase() != "ready" && state.phase() != "cached")
            return;
        std::map<InstrumentId, int> multipliers;
        std::vector<InstrumentId> ids;
        for (const auto& row : state.contracts()) {
            const InstrumentId id{row.instrument().venue(), row.instrument().symbol()};
            multipliers[id] = row.multiplier();
            ids.push_back(id);
        }
        feed.set_multipliers(std::move(multipliers));
        feed.subscribe(ids);
        contracts = std::move(ids);
        log::info(
            "contract catalog applied: phase={} trading_day={} contracts={}",
            state.phase(),
            state.trading_day(),
            contracts.size());
    }

    Recording::Status status() const
    {
        const auto& state = catalog.snapshot().second;
        Recording::Status out;
        out.phase =
            catalog.initializing() ? "initializing" : std::string(market_phase_name(feed.phase()));
        out.error_code = error_code;
        out.catalog_phase = state.phase();
        out.catalog_trading_day = state.trading_day();
        out.catalog_error = state.error_code();
        out.subscribed = contracts.size();
        out.last_quote_ms = last_quote_ms;
        out.written = recorder.status();
        out.merging = merging;
        out.merge_error = merge_error;
        return out;
    }
};

} // namespace

struct Recording::Loop
{
    mutable std::mutex mutex;
    Status published;
    std::promise<void> completion;
    std::shared_future<void> stopped = completion.get_future().share();
    std::jthread thread;

    explicit Loop(Configuration config)
        : thread([this, config = std::move(config)](std::stop_token stop) mutable {
              try {
                  {
                      State state(std::move(config));
                      auto next = Clock::now();
                      while (!stop.stop_requested()) {
                          state.advance();
                          if (Clock::now() >= next) {
                              std::lock_guard lock(mutex);
                              published = state.status();
                              next = Clock::now() + 250ms;
                          }
                          std::this_thread::sleep_for(10ms);
                      }
                  } // The last files are written before completion is reported.
                  completion.set_value();
              } catch (...) {
                  completion.set_exception(std::current_exception());
              }
          })
    {}
};

Recording::Recording(Configuration config)
    : loop_(std::make_unique<Loop>(std::move(config)))
{}

Recording::~Recording() = default;

Recording::Status Recording::status() const
{
    std::lock_guard lock(loop_->mutex);
    return loop_->published;
}

void Recording::stop()
{
    loop_->thread.request_stop();
}

std::shared_future<void> Recording::stopped() const
{
    return loop_->stopped;
}

} // namespace asterion::recording
