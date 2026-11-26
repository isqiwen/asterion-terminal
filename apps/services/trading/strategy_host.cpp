#include "strategy_host.hpp"
#include "alert_webhook.hpp"
#include "dominant_series.hpp"
#include "live_session.hpp"
#include "strategy_bars.hpp"
#include "strategy_local_bars.hpp"
#include "strategy_validation.hpp"
#include <asterion/base/error.hpp>
#include <asterion/base/id.hpp>
#include <asterion/base/serialization.hpp>
#include <asterion/database/sqlite/sqlite_database.hpp>
#include <asterion/futures/futures.hpp>
#include <asterion/protocol/common.hpp>
#include <asterion/system/durable_file.hpp>
#include <asterion/system/ipc/local_channel.hpp>
#include <asterion/system/log.hpp>
#include <asterion/system/process/artifact.hpp>
#include <asterion/system/process/child.hpp>
#include <asterion/system/process/file_lock.hpp>
#include <google/protobuf/util/json_util.h>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <unistd.h>

namespace asterion::trading {
namespace fs = std::filesystem;
namespace wire = ::strategy;
namespace pb = ::trading;
using namespace std::chrono_literals;

struct StrategyHost::State
{
    State(
        fs::path installed,
        fs::path account,
        ctp::Configuration configuration,
        std::shared_ptr<AlertWebhook> webhook,
        std::uint64_t memory_limit_mb,
        std::shared_ptr<const Clock> injected_clock)
        : installation(std::move(installed)),
          root(std::move(account) / "strategies"),
          market(std::move(configuration)),
          alerts(std::move(webhook)),
          clock(injected_clock ? std::move(injected_clock) : default_clock()),
          strategy_memory_limit_mb(memory_limit_mb)
    {
        create_directories_durably(root / "packages");
        lock = std::make_unique<FileLock>(root, "owner");
        local_bars = std::make_unique<sqlite::Database>(root / "bars.sqlite");
        local_bars->execute(
            "CREATE TABLE IF NOT EXISTS bars (" "period INTEGER NOT NULL, " "end_ms INTEGER NOT NULL, " "exchange TEXT NOT NULL, " "symbol TEXT NOT NULL, " "bar BLOB NOT NULL, " "PRIMARY KEY(period, end_ms, exchange, symbol))");
        local_bars->execute(
            "CREATE INDEX IF NOT EXISTS bars_contract_end ON bars(period, exchange, symbol, end_ms DESC)");
        if (fs::exists(root / "instance.pb")) {
            if (!instance.ParseFromString(read_strategy_file(root / "instance.pb", 131072)))
                throw std::invalid_argument(
                    "invalid strategy instance: " + (root / "instance.pb").string());
            if (instance.state() == pb::INSTANCE_STATE_WARMING
                || instance.state() == pb::INSTANCE_STATE_READY
                || instance.state() == pb::INSTANCE_STATE_STARTING) {
                instance.set_state(pb::INSTANCE_STATE_INTERRUPTED);
                instance.set_error("Trading restarted; explicitly start a new strategy instance");
                instance.set_process_id(0);
                save();
                if (alerts && instance.has_instance())
                    alerts->notify(
                        "strategy_failed",
                        "strategy_interrupted:" + instance.instance().id(),
                        instance.instance().id() + ": " + instance.error(),
                        {{"instance_id", instance.instance().id()},
                            {"state", "interrupted"},
                            {"error", instance.error()}});
            }
        }
    }

    ~State()
    {
        try {
            close();
        } catch (const std::exception& error) {
            log::error("strategy shutdown failed: {}", error.what());
        }
    }

    void save()
    {
        replace_file_durably(root / "instance.pb", instance.SerializeAsString());
        if (alerts && instance.state() == pb::INSTANCE_STATE_FAILED && instance.has_instance())
            alerts->notify(
                "strategy_failed",
                "strategy_failed:" + instance.instance().id(),
                instance.instance().id() + ": " + instance.error(),
                {{"instance_id", instance.instance().id()},
                    {"state", "failed"},
                    {"error", instance.error()}});
    }

    void close()
    {
        std::exception_ptr failure;
        if (authority) {
            try {
                account->release_strategy(authority).get();
                authority.reset();
            } catch (...) {
                failure = std::current_exception();
            }
        }
        if (!authority)
            account = nullptr;
        execution = {};
        quotes.clear();
        live_series.clear();
        live_sequence = 0;
        subscribed_contracts.clear();
        next_subscription_retry = {};
        next_feed_reconnect = {};
        next_target_heartbeat = {};
        execution_options = {};
        last_awaited_day.clear();
        running.store(false, std::memory_order_release);
        channel.close();
        if (child) {
            child->request_stop();
            child.reset();
        }
        feed.reset();
        bars.reset();
        records.reset();
        if (!endpoint.empty()) {
            std::error_code ignored;
            fs::remove_all(endpoint.parent_path(), ignored);
            endpoint.clear();
        }
        if (failure)
            std::rethrow_exception(failure);
    }

    void record_event(pb::InstanceEventKind kind, const std::string& event)
    {
        const auto now = utc_now_ms(*clock);
        sqlite::Database::Statement insert(*records, "INSERT INTO events VALUES(?,?,?)");
        insert.bind(1, now).bind(2, static_cast<std::int64_t>(kind)).bind(3, event).step();
        log::info("strategy {}: {}", instance.instance().id(), event);
        auto* added = instance.add_events();
        added->set_time_ms(now);
        added->set_text(event);
        added->set_kind(kind);
        if (instance.events_size() > 20)
            instance.mutable_events()->DeleteSubrange(0, 1);
        save();
        if (alerts && kind == pb::INSTANCE_EVENT_KIND_BATCH_ABANDONED)
            alerts->notify(
                "batch_abandoned",
                "batch_abandoned:" + instance.instance().id() + ":"
                    + std::to_string(++abandoned_batches),
                instance.instance().id() + ": " + event,
                {{"instance_id", instance.instance().id()}, {"reason", event}});
    }

    void require_running(const std::string& id) const
    {
        if (!authority || instance.instance().id() != id)
            throw Error(ErrorCode::conflict, "strategy instance changed; inspect before acting");
    }

    void pause(const pb::PauseStrategy& request)
    {
        require_running(request.id());
        account->pause_strategy(authority, request.paused()).get();
        instance.set_paused(request.paused());
        if (request.paused())
            record_event(pb::INSTANCE_EVENT_KIND_PAUSED, "paused");
        else
            record_event(pb::INSTANCE_EVENT_KIND_RESUMED, "resumed");
    }

    void flatten(const pb::InstanceId& request)
    {
        require_running(request.id());
        account->flatten_strategy(authority, latest_quotes()).get();
        instance.set_paused(true);
        record_event(pb::INSTANCE_EVENT_KIND_FLATTEN_REQUESTED, "flatten requested");
    }

    std::vector<MarketQuote> latest_quotes() const
    {
        std::vector<MarketQuote> result;
        for (const auto& [_, quote] : quotes)
            result.push_back(quote);
        return result;
    }

    // `execute` false replays a recorded input: state only, no targets reach the account.
    void decide(
        const wire::Bars& input,
        bool execute = true,
        sqlite::Database::Statement* warmup_insert = nullptr)
    {
        record_series_step_bars(live_series, input);
        const auto expires_at = steady_now(*clock) + strategy_execution_window(execution_options);
        const auto input_bytes = input.SerializeAsString();
        // Commit live inputs before delivering them. A missing reply remains
        // unknown in the record and is never replayed into a live child.
        if (!warmup_insert) {
            sqlite::Database::Transaction transaction(*records);
            sqlite::Database::Statement insert(
                *records,
                "INSERT INTO steps(sequence,input) VALUES(?,?)");
            insert.bind(1, static_cast<std::int64_t>(input.sequence()))
                .bind_blob(2, input_bytes)
                .step();
            transaction.commit();
        }
        wire::Request request;
        *request.mutable_bars() = input;
        channel.send(request.SerializeAsString(), 1s);
        wire::Response reply;
        if (!reply.ParseFromString(channel.receive(2s)) || !reply.has_bars())
            throw std::runtime_error(
                reply.has_error() ? reply.error().message() : "strategy returned no decision");
        const auto& decision = reply.bars();
        if (decision.sequence() != input.sequence()
            || (!decision.ready() && decision.targets_size()))
            throw std::runtime_error("strategy decision sequence or readiness is invalid");
        std::set<InstrumentId> selected_ids, targets;
        for (const auto& id : instance.instance().instruments())
            selected_ids.insert({id.exchange(), id.symbol()});
        for (const auto& series : live_series)
            selected_ids.insert(series.candidates[series.active_index]);
        int mode = 0;
        for (const auto& target : decision.targets()) {
            const InstrumentId id{target.instrument().exchange(), target.instrument().symbol()};
            if (!selected_ids.contains(id) || !targets.insert(id).second || !target.size_case()
                || (mode && mode != target.size_case()))
                throw std::runtime_error("strategy decision has invalid targets");
            mode = target.size_case();
        }
        if (warmup_insert) {
            warmup_insert->reset();
            warmup_insert->bind(1, static_cast<std::int64_t>(input.sequence()))
                .bind_blob(2, input_bytes)
                .bind_blob(3, decision.SerializeAsString())
                .step();
        } else {
            sqlite::Database::Transaction transaction(*records);
            sqlite::Database::Statement update(
                *records,
                "UPDATE steps SET decision=? WHERE sequence=?");
            update.bind_blob(1, decision.SerializeAsString())
                .bind(2, static_cast<std::int64_t>(input.sequence()))
                .step();
            transaction.commit();
        }
        instance.set_completed_steps(input.sequence());
        instance.set_last_bar_ms(input.bars(0).end_ms());
        *instance.mutable_last_decision() = decision;
        instance.set_state(
            decision.ready() ? pb::INSTANCE_STATE_READY : pb::INSTANCE_STATE_WARMING);
        if (!execute)
            return;
        auto exec_decision = decision;
        roll_decision_for_live_day(live_series, account, authority, exec_decision);
        account->accept_strategy_target(authority, exec_decision, latest_quotes(), expires_at)
            .get();
        save();
    }

    void advance()
    {
        observe();
        if (!feed)
            return;
        try {
            // The account detects execution faults independently of the child
            // and has already fenced sends and scheduled its cancellations.
            if (authority) {
                if (account->strategy_has_failure()) {
                    const auto failure = account->strategy_failure(authority).get();
                    if (!failure.empty())
                        throw std::runtime_error(failure);
                }
                if (account->strategy_events_pending()) {
                    for (const auto& event : account->take_strategy_events(authority).get())
                        record_event(event.kind(), event.text());
                }
            }
            const auto now = steady_now(*clock);
            if (next_feed_reconnect != std::chrono::steady_clock::time_point{}
                && now >= next_feed_reconnect) {
                next_feed_reconnect = {};
                feed->connect(market, subscribed_contracts);
            }
            const auto update = feed->poll();
            if (update.failed)
                throw std::runtime_error("strategy market stream lost observations");
            bool quotes_changed = false;
            for (const auto& event : update.events) {
                if (const auto* status = std::get_if<ctp::FeedStatus>(&event)) {
                    // CTP reconnects by itself; the window continues across the gap.
                    if (status->phase == MarketPhase::error) {
                        if (!connected && status->error_code != 7 && status->error_code != 8)
                            throw std::runtime_error("strategy market connection failed");
                        log::warn(
                            "strategy {} market feed error code={}; reconnecting in 5s",
                            instance.instance().id(),
                            status->error_code);
                        if (online) {
                            online = false;
                            bars->interrupt();
                            record_event(
                                pb::INSTANCE_EVENT_KIND_MARKET_DISCONNECTED,
                                "market disconnected");
                        }
                        feed->disconnect();
                        next_feed_reconnect = now + 5s;
                    } else if (status->phase == MarketPhase::connected && !online) {
                        if (connected)
                            record_event(
                                pb::INSTANCE_EVENT_KIND_MARKET_RECONNECTED,
                                "market reconnected");
                        connected = online = true;
                        next_feed_reconnect = {};
                        if (feed->failed_subscriptions() > 0)
                            next_subscription_retry = now + 5s;
                    } else if (status->phase != MarketPhase::connected && online) {
                        online = false;
                        bars->interrupt();
                        record_event(
                            pb::INSTANCE_EVENT_KIND_MARKET_DISCONNECTED,
                            "market disconnected");
                    }
                } else if (const auto* subscription = std::get_if<MarketSubscription>(&event)) {
                    if (subscription->state == SubscriptionState::error) {
                        log::warn(
                            "strategy {} subscription rejected: {}/{} code={}; will retry",
                            instance.instance().id(),
                            subscription->instrument.venue,
                            subscription->instrument.symbol,
                            subscription->error_code);
                        if (next_subscription_retry == std::chrono::steady_clock::time_point{})
                            next_subscription_retry = now + 5s;
                    }
                } else if (const auto* quote = std::get_if<MarketQuoteObservation>(&event)) {
                    if (quote->out_of_order)
                        continue;
                    const auto& observed = quote->quote;
                    const auto held = quotes.find(observed.instrument);
                    if (observed.received_ms - observed.source_ms <= 10000
                        && observed.source_ms - observed.received_ms <= 2000
                        && (held == quotes.end() || observed.source_ms >= held->second.source_ms)) {
                        quotes.insert_or_assign(observed.instrument, observed);
                        quotes_changed = true;
                        observe_series_quote(
                            instance.instance(),
                            live_series,
                            bars.get(),
                            last_awaited_day,
                            observed);
                    }
                    for (const auto& input : bars->observe(observed)) {
                        store_local_bars(
                            *local_bars,
                            bar_write_counts,
                            instance.instance().period(),
                            input);
                        if (live_series.empty()) {
                            decide(input);
                            quotes_changed = true;
                        } else {
                            auto filtered = filter_and_roll_bars(
                                instance.instance(),
                                live_series,
                                account,
                                authority,
                                bars.get(),
                                input,
                                [this](const std::string& message) {
                                    record_event(pb::INSTANCE_EVENT_KIND_UNSPECIFIED, message);
                                });
                            if (filtered.bars_size() > 0) {
                                filtered.set_sequence(++live_sequence);
                                decide(filtered);
                                quotes_changed = true;
                            }
                        }
                    }
                }
            }
            if (online && feed->failed_subscriptions() > 0
                && next_subscription_retry != std::chrono::steady_clock::time_point{}
                && now >= next_subscription_retry) {
                feed->retry_failed_subscriptions();
                next_subscription_retry = now + 5s;
            }
            if (!connected && now >= connect_deadline)
                throw std::runtime_error("strategy market login timed out");
            // Process all queued feed faults before admitting another leg. SDK
            // and journal waits run on the account loop, never on this worker.
            if (execution.valid()) {
                if (execution.wait_for(0s) != std::future_status::ready)
                    return;
                execution.get();
            }
            if (quotes_changed || account->strategy_batch_active()
                || now >= next_target_heartbeat) {
                execution = account->advance_strategy_target(authority, latest_quotes());
                next_target_heartbeat = now + 500ms;
            }
        } catch (const std::exception& error) {
            instance.set_state(pb::INSTANCE_STATE_FAILED);
            instance.set_error(error.what());
            instance.set_process_id(0);
            close();
            save();
        }
    }

    void observe()
    {
        if (child && child->exited()) {
            instance.set_state(pb::INSTANCE_STATE_FAILED);
            instance.set_error(
                "strategy process exited with code " + std::to_string(child->exit_code()));
            instance.set_process_id(0);
            close();
            save();
        }
    }

    pb::Strategies snapshot()
    {
        observe();
        pb::Strategies result;
        for (const auto& name : fs::directory_iterator(root / "packages")) {
            if (!name.is_directory() || name.path().filename().string().starts_with('.'))
                continue;
            for (const auto& version : fs::directory_iterator(name.path())) {
                if (version.path().filename().string().starts_with('.'))
                    continue;
                auto* manifest = result.add_packages();
                if (!google::protobuf::util::JsonStringToMessage(
                        read_strategy_file(version.path() / "strategy.json", 16384),
                        manifest)
                        .ok())
                    throw std::invalid_argument(
                        "invalid installed strategy manifest: " + version.path().string());
            }
        }
        if (instance.has_instance())
            *result.mutable_instance() = instance;
        return result;
    }

    pb::RecordedBars recorded_bars(const pb::GetRecordedBars& query)
    {
        return query_recorded_bars(*local_bars, query);
    }

    pb::StrategyRun strategy_run(const pb::GetStrategyRun& query)
    {
        const auto id = !query.id().empty() ? query.id()
            : instance.has_instance()       ? instance.instance().id()
                                            : std::string{};
        if (id.empty())
            throw std::invalid_argument("no strategy instance run is recorded");
        validate_package_identity(id, "instance");
        const auto limit = query.limit() ? query.limit() : 200U;
        if (limit > 500U)
            throw std::invalid_argument("strategy run pages contain 1 to 500 steps");
        std::optional<sqlite::Database> opened;
        sqlite::Database* db = nullptr;
        if (records && instance.has_instance() && id == instance.instance().id()) {
            db = records.get();
        } else {
            const auto run_file = root / "runs" / (id + ".sqlite");
            if (!fs::is_regular_file(run_file))
                throw std::invalid_argument("no recorded strategy instance: " + id);
            opened.emplace(run_file, sqlite::Database::Access::read_only);
            db = &*opened;
        }
        pb::StrategyRun out;
        {
            sqlite::Database::Statement cfg(*db, "SELECT value FROM configuration");
            if (!cfg.step() || !out.mutable_configuration()->ParseFromString(cfg.blob(0)))
                throw std::runtime_error("invalid strategy configuration record: " + id);
        }
        {
            sqlite::Database::Statement steps(
                *db,
                "SELECT input, decision IS NOT NULL, decision FROM steps " "WHERE sequence > ? ORDER BY sequence ASC LIMIT ?");
            steps.bind(1, static_cast<std::int64_t>(query.after_sequence()))
                .bind(2, static_cast<std::int64_t>(limit));
            while (steps.step()) {
                auto* step = out.add_steps();
                if (!step->mutable_input()->ParseFromString(steps.blob(0)))
                    throw std::runtime_error("invalid strategy step input record: " + id);
                if (steps.integer(1) && !step->mutable_decision()->ParseFromString(steps.blob(2)))
                    throw std::runtime_error("invalid strategy step decision record: " + id);
            }
        }
        {
            sqlite::Database::Statement events(
                *db,
                "SELECT time_ms, kind, event FROM events ORDER BY rowid ASC");
            while (events.step()) {
                auto* item = out.add_events();
                item->set_time_ms(events.integer(0));
                item->set_kind(static_cast<pb::InstanceEventKind>(events.integer(1)));
                item->set_text(events.text(2));
            }
        }
        return out;
    }

    void publish(const wire::Package& package)
    {
        validate_strategy_package(package);
        const auto& manifest = package.manifest();
        const auto parent = root / "packages" / manifest.name();
        create_directories_durably(parent);
        const auto destination = parent / manifest.version();
        if (fs::exists(destination))
            throw Error(
                ErrorCode::conflict,
                "strategy version is already installed; publish a new version");
        const auto temporary = parent / (".publish-" + unique_process_id());
        try {
            create_directories_durably(temporary);
            for (const auto& file : package.files()) {
                const auto path = temporary / file.path();
                create_directories_durably(path.parent_path());
                write_file_durably(path, file.content());
            }
            write_file_durably(temporary / "strategy.json", proto_to_json(manifest));
            fs::rename(temporary, destination);
            sync_directory(parent);
        } catch (...) {
            fs::remove_all(temporary);
            throw;
        }
    }

    void start(const wire::Instance& configuration, LiveSession* execution)
    {
        observe();
        if (child)
            throw Error(ErrorCode::conflict, "stop the loaded strategy before starting another");
        validate_strategy_instance(configuration);
        if (!execution)
            throw Error(
                ErrorCode::unavailable,
                "initialize and synchronize the account before starting a strategy");
        const auto package =
            root / "packages" / configuration.package_name() / configuration.package_version();
        (void)read_strategy_file(package / "strategy.json", 16384);
        const auto run = root / "runs" / (configuration.id() + ".sqlite");
        if (fs::exists(run))
            throw Error(ErrorCode::conflict, "strategy instance id already has a recorded run");
        const auto expiries = cached_expiries(root);
        std::map<std::int64_t, wire::Bars> merged;
        if (configuration.has_warmup())
            for (const auto& batch : configuration.warmup().bars())
                merged.emplace(batch.bars(0).end_ms(), batch);
        if (configuration.local_bars() || !configuration.resume_from().empty()) {
            const auto resumed = configuration.local_bars()
                ? load_local_bars(*local_bars, configuration, configuration.has_warmup(), expiries)
                : recorded_inputs(
                      root / "runs" / (configuration.resume_from() + ".sqlite"),
                      configuration);
            for (const auto& batch : resumed)
                merged.try_emplace(batch.bars(0).end_ms(), batch);
        }
        std::vector<wire::Bars> replay;
        replay.reserve(merged.size());
        for (auto& [_, batch] : merged)
            replay.push_back(std::move(batch));
        const auto python = installation / "runtime/python/bin/python3";
        const auto code = installation / "lib/strategy";
        if (!fs::is_regular_file(python) || !fs::is_regular_file(code / "strategy_runner.py"))
            throw Error(
                ErrorCode::unavailable,
                "strategy runtime must be installed at " + python.string() + " with code in "
                    + code.string());
        std::vector<InstrumentId> contracts, subscribed;
        for (const auto& id : configuration.instruments()) {
            contracts.push_back({id.exchange(), id.symbol()});
            subscribed.push_back(contracts.back());
        }
        std::vector<DominantSeriesSpec> series_specs;
        live_series.clear();
        for (const auto& series : configuration.series()) {
            DominantSeriesSpec spec;
            spec.symbol = series_symbol(series);
            LiveSeries state;
            for (const auto& entry : series.contracts()) {
                const InstrumentId id{entry.instrument().exchange(), entry.instrument().symbol()};
                spec.candidates.push_back(id);
                state.delivery_months.push_back(entry.delivery_month());
                const auto exp = expiries.find(id);
                state.expiry_days.push_back(exp == expiries.end() ? std::string{} : exp->second);
            }
            for (auto it = replay.rbegin(); it != replay.rend(); ++it) {
                bool matched = false;
                for (const auto& bar : it->bars()) {
                    const InstrumentId id{bar.instrument().exchange(), bar.instrument().symbol()};
                    const auto found = std::ranges::find(spec.candidates, id);
                    if (found != spec.candidates.end()) {
                        spec.initial_index =
                            static_cast<std::size_t>(found - spec.candidates.begin());
                        matched = true;
                        break;
                    }
                }
                if (matched)
                    break;
            }
            for (std::size_t c = spec.initial_index; c < spec.candidates.size(); ++c)
                subscribed.push_back(spec.candidates[c]);
            state.symbol = spec.symbol;
            state.candidates = spec.candidates;
            state.active_index = spec.initial_index;
            state.history.resize(spec.candidates.size());
            state.prev_bar_close.resize(spec.candidates.size());
            live_series.push_back(std::move(state));
            series_specs.push_back(std::move(spec));
        }
        std::optional<StrategyWeightBudget> budget;
        if (configuration.has_weight_limits()) {
            const auto& wl = configuration.weight_limits();
            budget = StrategyWeightBudget{
                Decimal::parse(wl.gross().text()),
                Decimal::parse(wl.net().text()),
                wl.has_max_margin_ratio() && !wl.max_margin_ratio().text().empty()
                    ? Decimal::parse(wl.max_margin_ratio().text())
                    : Decimal{},
                wl.has_min_rebalance_weight() && !wl.min_rebalance_weight().text().empty()
                    ? Decimal::parse(wl.min_rebalance_weight().text())
                    : Decimal{}
            };
        }
        execution_options = configuration.has_execution()
            ? normalize_execution_options(
                  {configuration.execution().aggressive_ticks(),
                      configuration.execution().max_reprices(),
                      configuration.execution().reprice_interval_ms(),
                      configuration.execution().twap_slices(),
                      configuration.execution().twap_interval_ms()})
            : StrategyExecutionOptions{};
        authority = execution
                        ->register_strategy(
                            configuration.id(),
                            contracts,
                            budget,
                            std::move(series_specs),
                            execution_options)
                        .get();
        account = execution;
        auto persisted = configuration;
        if (persisted.has_warmup())
            persisted.mutable_warmup()->clear_bars();
        instance.Clear();
        *instance.mutable_instance() = persisted;
        instance.set_state(pb::INSTANCE_STATE_STARTING);
        try {
            save();
            create_directories_durably(root / "runs");
            records = std::make_unique<sqlite::Database>(run);
            records->execute("CREATE TABLE configuration (value BLOB NOT NULL)");
            records->execute(
                "CREATE TABLE steps (sequence INTEGER PRIMARY KEY, input BLOB NOT NULL, decision BLOB)");
            records->execute(
                "CREATE TABLE events (time_ms INTEGER NOT NULL, kind INTEGER NOT NULL, event TEXT NOT NULL)");
            {
                sqlite::Database::Transaction transaction(*records);
                sqlite::Database::Statement insert(*records, "INSERT INTO configuration VALUES(?)");
                insert.bind_blob(1, persisted.SerializeAsString()).step();
                transaction.commit();
            }
            if (configuration.has_warmup()) {
                record_event(
                    pb::INSTANCE_EVENT_KIND_UNSPECIFIED,
                    "warmup dataset_version=" + configuration.warmup().dataset_version()
                        + " sha256=" + configuration.warmup().sha256());
            }
            sync_directory(run.parent_path());
            const auto directory =
                fs::temp_directory_path() / ("ast-strategy-" + unique_process_id().substr(0, 16));
            fs::create_directory(directory);
            fs::permissions(directory, fs::perms::owner_all);
            endpoint = directory / "child.sock";
            const auto bootstrap = "import sys; sys.path.insert(0, " + json_string(code.string())
                + "); import strategy_runner; strategy_runner.main()";
            const auto diagnostics =
                installation / "logs" / ("strategy-" + unique_process_id() + ".log");
            create_directories_durably(diagnostics.parent_path());
            log::info("strategy {} diagnostics: {}", configuration.id(), diagnostics.string());
            child = std::make_unique<ChildProcess>(
                python,
                std::vector<std::string>{
                    "-I",
                    "-B",
                    "-X",
                    "faulthandler",
                    "-c",
                    bootstrap,
                    endpoint.string(),
                    package.string(),
                    std::to_string(::getpid()),
                    std::to_string(strategy_memory_limit_mb)
                },
                true,
                diagnostics,
                true);
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            while (!fs::exists(endpoint)) {
                if (child->exited())
                    throw std::runtime_error("strategy runtime exited before opening its socket");
                if (std::chrono::steady_clock::now() >= deadline)
                    throw std::runtime_error(
                        "strategy runtime did not open its socket within 5 seconds");
                std::this_thread::sleep_for(10ms);
            }
            channel = ipc::Channel::connect(endpoint.string(), 1s);
            wire::Request request;
            *request.mutable_load()->mutable_instance() = persisted;
            for (const auto& symbol : authority->symbols())
                request.mutable_load()->add_symbols(symbol);
            channel.send(request.SerializeAsString(), 1s);
            wire::Response reply;
            if (!reply.ParseFromString(channel.receive(10s)) || !reply.has_load())
                throw std::invalid_argument(
                    reply.has_error() ? reply.error().message() : "strategy did not load");
            std::uint64_t sequence = 0;
            if (!replay.empty()) {
                sqlite::Database::Transaction warmup_tx(*records);
                sqlite::Database::Statement warmup_insert(
                    *records,
                    "INSERT INTO steps(sequence,input,decision) VALUES(?,?,?)");
                for (auto input : replay) {
                    input.set_sequence(++sequence);
                    decide(input, false, &warmup_insert);
                }
                warmup_tx.commit();
            }
            live_sequence = sequence;
            bars = std::make_unique<StrategyBars>(
                persisted,
                clock,
                sequence,
                replay.empty() ? 0 : replay.back().bars(0).end_ms());
            bars->set_awaited(awaited_series_contracts(instance.instance(), live_series));
            feed = std::make_unique<ctp::Feed>(
                sdk_owner,
                installation / "lib/ctp-md.so",
                root / "ctp-flow");
            connected = online = false;
            subscribed_contracts = subscribed;
            next_subscription_retry = {};
            next_feed_reconnect = {};
            next_target_heartbeat = {};
            last_awaited_day.clear();
            connect_deadline = steady_now(*clock) + 30s;
            feed->connect(market, subscribed_contracts);
            running.store(true, std::memory_order_release);
            if (replay.empty())
                instance.set_state(pb::INSTANCE_STATE_WARMING);
            instance.set_process_id(child->id());
            save();
        } catch (const std::exception& error) {
            close();
            instance.set_state(pb::INSTANCE_STATE_FAILED);
            instance.set_process_id(0);
            instance.set_error(error.what());
            save();
            throw;
        }
    }

    static std::string json_string(const std::string& value)
    {
        // JSON string syntax is also a Python string literal; paths are data,
        // never interpolated as executable Python or passed through a shell.
        return nlohmann::json(value).dump();
    }

    void stop()
    {
        if (!child && !authority)
            return;
        close();
        instance.set_state(pb::INSTANCE_STATE_STOPPED);
        instance.set_process_id(0);
        save();
    }

    fs::path installation, root, endpoint;
    std::unique_ptr<FileLock> lock;
    pb::InstanceStatus instance;
    std::unique_ptr<ChildProcess> child;
    ipc::Channel channel;
    ctp::Configuration market;
    ThreadPool sdk_owner{1, 8};
    std::unique_ptr<ctp::Feed> feed;
    std::unique_ptr<StrategyBars> bars;
    std::unique_ptr<sqlite::Database> records;
    std::unique_ptr<sqlite::Database> local_bars;
    LiveSession* account = nullptr;
    StrategyAuthorityPtr authority;
    std::map<InstrumentId, MarketQuote> quotes;
    std::future<void> execution;
    bool connected = false, online = false;
    std::chrono::steady_clock::time_point connect_deadline;
    std::shared_ptr<AlertWebhook> alerts;
    std::uint64_t abandoned_batches = 0;
    std::vector<LiveSeries> live_series;
    std::uint64_t live_sequence = 0;
    std::vector<InstrumentId> subscribed_contracts;
    std::chrono::steady_clock::time_point next_subscription_retry{}, next_feed_reconnect{};
    std::shared_ptr<const Clock> clock;
    std::uint64_t strategy_memory_limit_mb = 2048;
    std::map<std::tuple<int, std::string, std::string>, std::uint64_t> bar_write_counts;
    std::atomic<bool> running{false};
    std::string last_awaited_day;
    std::chrono::steady_clock::time_point next_target_heartbeat{};
    StrategyExecutionOptions execution_options{};
};

StrategyHost::StrategyHost(
    fs::path installation,
    fs::path account,
    ctp::Configuration market,
    std::shared_ptr<AlertWebhook> alerts,
    std::uint64_t strategy_memory_limit_mb,
    std::shared_ptr<const Clock> clock)
    : state_(
          std::make_unique<State>(
              std::move(installation),
              std::move(account),
              std::move(market),
              std::move(alerts),
              strategy_memory_limit_mb,
              std::move(clock)))
{}

StrategyHost::~StrategyHost()
{
    catalog_stop_.request_stop();
    catalog_worker_.shutdown();
    worker_.shutdown();
}

std::future<pb::RecordedBars> StrategyHost::recorded_bars(pb::GetRecordedBars query)
{
    if (closing_)
        throw Error(ErrorCode::unavailable, "strategy host is stopping");
    auto result = std::make_shared<std::promise<pb::RecordedBars>>();
    auto future = result->get_future();
    (void)worker_.submit([this, query = std::move(query), result](std::stop_token) {
        try {
            result->set_value(state_->recorded_bars(query));
        } catch (...) {
            result->set_exception(std::current_exception());
        }
    });
    return future;
}

std::future<pb::StrategyRun> StrategyHost::strategy_run(pb::GetStrategyRun query)
{
    if (closing_)
        throw Error(ErrorCode::unavailable, "strategy host is stopping");
    auto result = std::make_shared<std::promise<pb::StrategyRun>>();
    auto future = result->get_future();
    (void)worker_.submit([this, query = std::move(query), result](std::stop_token) {
        try {
            result->set_value(state_->strategy_run(query));
        } catch (...) {
            result->set_exception(std::current_exception());
        }
    });
    return future;
}

std::future<pb::ContractCatalog> StrategyHost::catalog(
    pb::GetCatalog query,
    fs::path ctp_library,
    ctp::CatalogConfiguration configuration)
{
    if (closing_)
        throw Error(ErrorCode::unavailable, "strategy host is stopping");
    auto result = std::make_shared<std::promise<pb::ContractCatalog>>();
    auto future = result->get_future();
    const auto root = state_->root;
    const auto stop = catalog_stop_.get_token();
    (void)catalog_worker_.submit([query = std::move(query),
                                     ctp_library = std::move(ctp_library),
                                     configuration = std::move(configuration),
                                     root,
                                     stop,
                                     result](std::stop_token) mutable {
        try {
            const auto cache = root / "catalog.pb";
            if (!query.refresh() && fs::is_regular_file(cache) && !fs::is_symlink(cache)
                && fs::file_size(cache) <= 64 * 1024 * 1024) {
                std::ifstream input(cache, std::ios::binary);
                pb::ContractCatalog cached;
                if (cached.ParseFromIstream(&input) && cached.contracts_size() > 0
                    && cached.contracts_size() <= 20000) {
                    result->set_value(std::move(cached));
                    return;
                }
            }
            ctp::CatalogQuery session(ctp_library, root / "catalog-flow", std::move(configuration));
            for (;;) {
                if (auto fetched = session.poll(stop)) {
                    pb::ContractCatalog out;
                    out.set_trading_day(protocol::iso_day(fetched->trading_day));
                    for (const auto& entry : fetched->contracts) {
                        auto* item = out.add_contracts();
                        item->mutable_instrument()->set_exchange(entry.instrument.venue);
                        item->mutable_instrument()->set_symbol(entry.instrument.symbol);
                        item->set_contract(entry.contract_id);
                        item->set_product(entry.product);
                        item->set_name(entry.name);
                        item->set_expiry_day(protocol::iso_day(entry.expiry));
                        item->set_multiplier(entry.multiplier);
                        *item->mutable_price_tick() = protocol::decimal(entry.price_tick);
                    }
                    try {
                        replace_file_durably(cache, out.SerializeAsString());
                    } catch (const std::exception& error) {
                        log::warn(
                            "trading contract catalog cache could not be saved: {}",
                            error.what());
                    }
                    result->set_value(std::move(out));
                    return;
                }
                std::this_thread::sleep_for(20ms);
            }
        } catch (...) {
            result->set_exception(std::current_exception());
        }
    });
    return future;
}

std::future<pb::Strategies> StrategyHost::request(pb::Request request, LiveSession* account)
{
    if (closing_)
        throw Error(ErrorCode::unavailable, "strategy host is stopping");
    auto result = std::make_shared<std::promise<pb::Strategies>>();
    auto future = result->get_future();
    (void)worker_.submit([this, request = std::move(request), result, account](std::stop_token) {
        try {
            if (request.has_publish_strategy())
                state_->publish(request.publish_strategy());
            else if (request.has_start_strategy())
                state_->start(request.start_strategy(), account);
            else if (request.has_pause_strategy())
                state_->pause(request.pause_strategy());
            else if (request.has_flatten_strategy())
                state_->flatten(request.flatten_strategy());
            else if (request.has_stop_strategy()) {
                if (request.stop_strategy().id().empty())
                    throw std::invalid_argument("stopping a strategy requires its instance id");
                if (!state_->instance.has_instance()
                    || state_->instance.instance().id() != request.stop_strategy().id())
                    throw Error(
                        ErrorCode::conflict,
                        "strategy instance changed; inspect before stopping");
                state_->stop();
            } else if (!request.has_get_strategies())
                throw std::invalid_argument("missing strategy operation");
            result->set_value(state_->snapshot());
        } catch (...) {
            result->set_exception(std::current_exception());
        }
    });
    return future;
}

void StrategyHost::poll()
{
    if (closing_ || !state_->running.load(std::memory_order_acquire))
        return;
    if (observation_.valid()) {
        if (observation_.wait_for(0s) != std::future_status::ready)
            return;
        try {
            observation_.get();
        } catch (const std::exception& error) {
            log::error("strategy status persistence failed: {}", error.what());
        }
    }
    try {
        observation_ = worker_.submit([this](std::stop_token) {
            state_->advance();
        });
    } catch (const Error& error) {
        if (error.code() != ErrorCode::resource_exhausted)
            throw;
        // Accepted administration filled the bounded queue. Observe exit on
        // the next owner turn; client traffic must not terminate the account.
    }
}

void StrategyHost::stop()
{
    if (shutdown_.valid())
        return;
    closing_ = true;
    catalog_stop_.request_stop();
    try {
        shutdown_ = worker_.submit([this](std::stop_token) {
            state_->stop();
        });
    } catch (const Error& error) {
        if (error.code() != ErrorCode::resource_exhausted)
            throw;
        // Admission is closed; enqueue shutdown once accepted work advances.
    }
}

bool StrategyHost::stopped() const
{
    return shutdown_.valid() && shutdown_.wait_for(0s) == std::future_status::ready;
}

} // namespace asterion::trading
