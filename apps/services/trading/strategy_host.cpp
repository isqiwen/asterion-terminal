#include "strategy_host.hpp"
#include "strategy_bars.hpp"
#include "live_session.hpp"
#include <asterion/database/sqlite/sqlite_database.hpp>
#include <asterion/base/error.hpp>
#include <asterion/base/id.hpp>
#include <asterion/base/serialization.hpp>
#include <algorithm>
#include <asterion/system/durable_file.hpp>
#include <asterion/system/process/child.hpp>
#include <asterion/system/process/file_lock.hpp>
#include <asterion/system/ipc/local_channel.hpp>
#include <asterion/system/log.hpp>
#include <google/protobuf/util/json_util.h>
#include <google/protobuf/util/message_differencer.h>
#include <fstream>
#include <regex>
#include <map>
#include <set>
#include <unistd.h>

namespace asterion::trading {
namespace fs = std::filesystem;
namespace wire = ::strategy;
namespace pb = ::trading;
using namespace std::chrono_literals;

namespace {

std::string read(const fs::path& path, std::size_t limit)
{
    if (!fs::is_regular_file(path) || fs::is_symlink(path) || fs::file_size(path) > limit)
        throw std::invalid_argument("missing or invalid strategy file: " + path.string());
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

std::string json(const google::protobuf::Message& message)
{
    std::string result;
    if (!google::protobuf::util::MessageToJsonString(message, &result).ok())
        throw std::runtime_error("cannot encode strategy description");
    return result;
}

void package_identity(const std::string& name, const std::string& version)
{
    const std::regex component("[A-Za-z0-9][A-Za-z0-9._-]{0,63}");
    if (!std::regex_match(name, component) || !std::regex_match(version, component))
        throw std::invalid_argument(
            "package name and version must be 1-64 letters, digits, dots, underscores or hyphens, beginning with a letter or digit");
}

void validate(const wire::Package& package)
{
    const auto& m = package.manifest();
    package_identity(m.name(), m.version());
    if (m.runtime() != "python3.12-numpy2.3.5")
        throw std::invalid_argument("unsupported strategy runtime: " + m.runtime());
    const std::regex entry(
        "[A-Za-z_][A-Za-z0-9_]*(\\.[A-Za-z_][A-Za-z0-9_]*)*:[A-Za-z_][A-Za-z0-9_]*");
    if (!std::regex_match(m.entry(), entry))
        throw std::invalid_argument("strategy entry must be module:Class");
    if (m.files_size() < 1 || m.files_size() > 64 || m.files_size() != package.files_size()
        || package.ByteSizeLong() > 600 * 1024)
        throw std::invalid_argument(
            "a strategy package has 1-64 Python files, at most 512 KiB of code");
    const std::regex path("([A-Za-z_][A-Za-z0-9_]*/)*[A-Za-z_][A-Za-z0-9_]*\\.py");
    std::set<std::string> names;
    std::size_t bytes = 0;
    for (int i = 0; i < m.files_size(); ++i) {
        const auto& name = m.files(i);
        if (!std::regex_match(name, path) || name.starts_with("asterion")
            || !names.insert(name).second || package.files(i).path() != name)
            throw std::invalid_argument("invalid or duplicate strategy source path: " + name);
        bytes += package.files(i).content().size();
    }
    if (bytes > 512 * 1024)
        throw std::invalid_argument("strategy source exceeds 512 KiB");
    auto module = m.entry().substr(0, m.entry().find(':'));
    std::replace(module.begin(), module.end(), '.', '/');
    if (!names.contains(module + ".py"))
        throw std::invalid_argument("strategy entry is not included in package files");
}

void validate(const wire::Instance& instance)
{
    package_identity(instance.id(), "instance");
    package_identity(instance.package_name(), instance.package_version());
    if (instance.instruments_size() < 1 || instance.instruments_size() > 20)
        throw std::invalid_argument("a strategy instance selects 1-20 native contracts");
    std::set<std::string> contracts;
    const std::regex exchange("SHFE|INE|DCE|CZCE|CFFEX|GFEX"), symbol("[A-Za-z]+[0-9]{3,4}");
    for (const auto& value : instance.instruments()) {
        const auto name = value.exchange() + "/" + value.symbol();
        if (!std::regex_match(value.exchange(), exchange)
            || !std::regex_match(value.symbol(), symbol) || !contracts.insert(name).second)
            throw std::invalid_argument("invalid or duplicate native contract: " + name);
    }
    if (instance.period() == ::common::PERIOD_UNSPECIFIED
        || instance.period() > ::common::PERIOD_DAY)
        throw std::invalid_argument("strategy bars are daily or of 1 to 60 minutes");
    if (instance.has_weight_limits()
        && (!instance.weight_limits().has_gross() || !instance.weight_limits().has_net()))
        throw std::invalid_argument(
            "strategy weight budget requires explicit gross and net limits");
    if (instance.ByteSizeLong() > 16384)
        throw std::invalid_argument("strategy instance configuration exceeds 16 KiB");
    if (!instance.resume_from().empty()) {
        package_identity(instance.resume_from(), "instance");
        if (instance.resume_from() == instance.id())
            throw std::invalid_argument("a strategy instance cannot resume from itself");
    }
}

// The bars an ended instance received, in order. Its configuration must match
// apart from the instance identities.
std::vector<wire::Bars> recorded_inputs(const fs::path& run, const wire::Instance& configuration)
{
    if (!fs::is_regular_file(run))
        throw std::invalid_argument("no recorded strategy instance: " + run.stem().string());
    sqlite::Database previous(run, sqlite::Database::Access::read_only);
    sqlite::Database::Statement stored(previous, "SELECT value FROM configuration");
    wire::Instance recorded;
    if (!stored.step() || !recorded.ParseFromString(stored.blob(0)))
        throw std::invalid_argument("invalid strategy record: " + run.string());
    auto wanted = configuration;
    for (auto* item : {&recorded, &wanted}) {
        item->clear_id();
        item->clear_resume_from();
    }
    if (!google::protobuf::util::MessageDifferencer::Equals(recorded, wanted))
        throw std::invalid_argument(
            "a resumed instance needs the same package, contracts, period, parameters and budget");
    std::vector<wire::Bars> inputs;
    sqlite::Database::Statement steps(previous, "SELECT input FROM steps ORDER BY sequence");
    while (steps.step()) {
        auto& input = inputs.emplace_back();
        if (!input.ParseFromString(steps.blob(0)))
            throw std::invalid_argument("invalid strategy record: " + run.string());
    }
    return inputs;
}

} // namespace

struct StrategyHost::State
{
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
    LiveSession* account = nullptr;
    StrategyAuthorityPtr authority;
    std::map<InstrumentId, MarketQuote> quotes;
    std::future<void> execution;
    bool connected = false, online = false;
    std::chrono::steady_clock::time_point connect_deadline;

    State(fs::path installed, fs::path account, ctp::Configuration configuration)
        : installation(std::move(installed)),
          root(std::move(account) / "strategies"),
          market(std::move(configuration))
    {
        create_directories_durably(root / "packages");
        lock = std::make_unique<FileLock>(root, "owner");
        if (fs::exists(root / "instance.pb")) {
            if (!instance.ParseFromString(read(root / "instance.pb", 32768)))
                throw std::invalid_argument(
                    "invalid strategy instance: " + (root / "instance.pb").string());
            if (instance.state() == pb::INSTANCE_STATE_WARMING
                || instance.state() == pb::INSTANCE_STATE_READY
                || instance.state() == pb::INSTANCE_STATE_STARTING) {
                instance.set_state(pb::INSTANCE_STATE_INTERRUPTED);
                instance.set_error("Trading restarted; explicitly start a new strategy instance");
                instance.set_process_id(0);
                save();
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
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
                             .count();
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
    void decide(const wire::Bars& input, bool execute = true)
    {
        const auto expires_at = std::chrono::steady_clock::now() + 10s;
        // Commit the exact input before delivering it. A missing reply remains
        // unknown in the record and is never replayed into a live child.
        {
            sqlite::Database::Transaction transaction(*records);
            sqlite::Database::Statement insert(
                *records,
                "INSERT INTO steps(sequence,input) VALUES(?,?)");
            insert.bind(1, static_cast<std::int64_t>(input.sequence()))
                .bind_blob(2, input.SerializeAsString())
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
        int mode = 0;
        for (const auto& target : decision.targets()) {
            const InstrumentId id{target.instrument().exchange(), target.instrument().symbol()};
            if (!selected_ids.contains(id) || !targets.insert(id).second || !target.size_case()
                || (mode && mode != target.size_case()))
                throw std::runtime_error("strategy decision has invalid targets");
            mode = target.size_case();
        }
        {
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
        *instance.mutable_last_decision() = decision;
        instance.set_state(
            decision.ready() ? pb::INSTANCE_STATE_READY : pb::INSTANCE_STATE_WARMING);
        if (!execute)
            return;
        save();
        account->accept_strategy_target(authority, decision, latest_quotes(), expires_at).get();
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
                const auto failure = account->strategy_failure(authority).get();
                if (!failure.empty())
                    throw std::runtime_error(failure);
                for (const auto& event : account->take_strategy_events(authority).get())
                    record_event(event.kind(), event.text());
            }
            const auto update = feed->poll();
            if (update.failed)
                throw std::runtime_error("strategy market stream lost observations");
            for (const auto& event : update.events) {
                if (const auto* status = std::get_if<ctp::FeedStatus>(&event)) {
                    // CTP reconnects by itself; the window continues across the gap.
                    if (status->phase == MarketPhase::error)
                        throw std::runtime_error("strategy market connection failed");
                    if (status->phase == MarketPhase::connected && !online) {
                        if (connected)
                            record_event(
                                pb::INSTANCE_EVENT_KIND_MARKET_RECONNECTED,
                                "market reconnected");
                        connected = online = true;
                    } else if (status->phase != MarketPhase::connected && online) {
                        online = false;
                        record_event(
                            pb::INSTANCE_EVENT_KIND_MARKET_DISCONNECTED,
                            "market disconnected");
                    }
                } else if (const auto* subscription = std::get_if<MarketSubscription>(&event)) {
                    if (subscription->state == SubscriptionState::error)
                        throw std::runtime_error("strategy contract subscription rejected");
                } else if (const auto* quote = std::get_if<MarketQuoteObservation>(&event)) {
                    if (quote->out_of_order)
                        continue;
                    const auto& observed = quote->quote;
                    const auto held = quotes.find(observed.instrument);
                    if (observed.received_ms - observed.source_ms <= 10000
                        && (held == quotes.end() || observed.source_ms >= held->second.source_ms))
                        quotes.insert_or_assign(observed.instrument, observed);
                    for (const auto& input : bars->observe(observed))
                        decide(input);
                }
            }
            if (!connected && std::chrono::steady_clock::now() >= connect_deadline)
                throw std::runtime_error("strategy market login timed out");
            // Process all queued feed faults before admitting another leg. SDK
            // and journal waits run on the account loop, never on this worker.
            if (execution.valid()) {
                if (execution.wait_for(0s) != std::future_status::ready)
                    return;
                execution.get();
            }
            execution = account->advance_strategy_target(authority, latest_quotes());
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
                        read(version.path() / "strategy.json", 16384),
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

    void publish(const wire::Package& package)
    {
        validate(package);
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
            write_file_durably(temporary / "strategy.json", json(manifest));
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
        validate(configuration);
        if (!execution)
            throw Error(
                ErrorCode::unavailable,
                "initialize and synchronize the account before starting a strategy");
        const auto package =
            root / "packages" / configuration.package_name() / configuration.package_version();
        (void)read(package / "strategy.json", 16384);
        const auto run = root / "runs" / (configuration.id() + ".sqlite");
        if (fs::exists(run))
            throw Error(ErrorCode::conflict, "strategy instance id already has a recorded run");
        const auto replay = configuration.resume_from().empty()
            ? std::vector<wire::Bars>{}
            : recorded_inputs(
                  root / "runs" / (configuration.resume_from() + ".sqlite"),
                  configuration);
        const auto python = installation / "runtime/python/bin/python3";
        const auto code = installation / "lib/strategy";
        if (!fs::is_regular_file(python) || !fs::is_regular_file(code / "strategy_runner.py"))
            throw Error(
                ErrorCode::unavailable,
                "strategy runtime must be installed at " + python.string() + " with code in "
                    + code.string());
        std::vector<InstrumentId> contracts;
        for (const auto& id : configuration.instruments())
            contracts.push_back({id.exchange(), id.symbol()});
        std::optional<StrategyWeightBudget> budget;
        if (configuration.has_weight_limits())
            budget = StrategyWeightBudget{
                Decimal::parse(configuration.weight_limits().gross().text()),
                Decimal::parse(configuration.weight_limits().net().text())
            };
        authority = execution->register_strategy(configuration.id(), contracts, budget).get();
        account = execution;
        instance.Clear();
        *instance.mutable_instance() = configuration;
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
                insert.bind_blob(1, configuration.SerializeAsString()).step();
                transaction.commit();
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
                    std::to_string(::getpid())
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
            *request.mutable_load()->mutable_instance() = configuration;
            for (const auto& symbol : authority->symbols())
                request.mutable_load()->add_symbols(symbol);
            channel.send(request.SerializeAsString(), 1s);
            wire::Response reply;
            if (!reply.ParseFromString(channel.receive(10s)) || !reply.has_load())
                throw std::invalid_argument(
                    reply.has_error() ? reply.error().message() : "strategy did not load");
            std::uint64_t sequence = 0;
            for (auto input : replay) {
                input.set_sequence(++sequence);
                decide(input, false);
            }
            bars = std::make_unique<StrategyBars>(
                configuration,
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count(),
                sequence);
            feed = std::make_unique<ctp::Feed>(
                sdk_owner,
                installation / "lib/ctp-md.so",
                root / "ctp-flow");
            connected = online = false;
            connect_deadline = std::chrono::steady_clock::now() + 30s;
            feed->connect(market, contracts);
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
};

StrategyHost::StrategyHost(fs::path installation, fs::path account, ctp::Configuration market)
    : state_(
          std::make_unique<State>(std::move(installation), std::move(account), std::move(market)))
{}

StrategyHost::~StrategyHost()
{
    worker_.shutdown();
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
    if (closing_)
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
