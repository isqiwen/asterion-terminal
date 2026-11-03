#include <asterion/system/rpc_host.hpp>
#include <asterion/system/durable_file.hpp>
#include "../apps/clients/terminal/native/application_impl.hpp"
#include "support/timing.hpp"
#include "support/market_host.hpp"
#include <fstream>
#include <asterion/system/environment.hpp>
#include <asterion/protocol/trading.hpp>
#include "terminal/blocking_service.hpp"
#include <asterion/system/process/artifact.hpp>
#include <future>
#include <asio.hpp>
#include <latch>
#include <gtest/gtest.h>
#include <regex>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/base_sink.h>
using namespace asterion;
using namespace std::chrono_literals;

namespace {

// Application now locks the installed node directory before starting clients.
// These IPC doubles own a disposable installation root, never the user's node.
class IsolatedTerminalEnvironment final : public testing::Environment
{
    std::filesystem::path root_ =
        std::filesystem::temp_directory_path() / ("ast-terminal-query-" + unique_process_id());
    std::optional<std::string> previous_ = environment_variable("ASTERION_ROOT");

    void SetUp() override
    {
        std::filesystem::create_directory(root_);
        ASSERT_EQ(::setenv("ASTERION_ROOT", root_.c_str(), 1), 0);
    }

    void TearDown() override
    {
        if (previous_)
            ::setenv("ASTERION_ROOT", previous_->c_str(), 1);
        else
            ::unsetenv("ASTERION_ROOT");
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }
};

[[maybe_unused]] const auto isolated_terminal_environment =
    testing::AddGlobalTestEnvironment(new IsolatedTerminalEnvironment);

} // namespace

namespace asterion::terminal {

// Install a real IPC client without provisioning an Agent or touching user state.
struct ApplicationTestAccess
{
    static void snapshot_budget(Application& app, std::size_t bytes)
    {
        app.impl_->service_io
            .submit<void>([&](std::stop_token) -> PolledTask<void> {
                app.impl_->snapshot_payloads = PayloadBudget{bytes};
                co_return;
            })
            .get();
    }

    static std::function<Json()> capture(Application& app)
    {
        return app.impl_->service_io
            .submit<std::function<Json()>>(
                [&](std::stop_token) -> PolledTask<std::function<Json()>> {
                    auto snapshot = app.impl_->capture();
                    co_return [snapshot = std::move(snapshot)] {
                        return snapshot.render();
                    };
                })
            .get();
    }

    static std::future<Json> submit(Application& app, Json command)
    {
        return app.impl_->service_io.submit<Json>(
            [&app, command = std::move(command)](std::stop_token) -> PolledTask<Json> {
                auto response = co_await app.impl_->dispatch(command);
                co_return co_await app.impl_->service_io.read<Json>(
                    [&] {
                        return std::move(response).render();
                    },
                    ServiceIo::ReadLane::response);
            },
            ServiceIo::Lane::application);
    }

    static std::future<void> slow_settings(
        Application& app,
        std::promise<void>& entered,
        const std::shared_future<void>& released)
    {
        return app.impl_->service_io.submit<void>([&](std::stop_token) -> PolledTask<void> {
            co_await app.impl_->settings<void>([&] {
                entered.set_value();
                released.wait();
            });
        });
    }

    static std::future<void> slow_read(
        Application& app,
        std::latch& entered,
        const std::shared_future<void>& released,
        ServiceIo::ReadLane lane = ServiceIo::ReadLane::data)
    {
        return app.impl_->service_io.submit<void>([&, lane](std::stop_token) -> PolledTask<void> {
            co_await app.impl_->service_io.read<void>(
                [&] {
                    entered.count_down();
                    released.wait();
                },
                lane);
        });
    }

    // Changes what the core publishes, as an owner operation does.
    static void count_success(Application& app)
    {
        app.impl_->service_io
            .submit<void>([&](std::stop_token) -> PolledTask<void> {
                ++app.impl_->succeeded;
                app.impl_->publish();
                co_return;
            })
            .get();
    }
};

} // namespace asterion::terminal

namespace {

Json request(const std::string& method, Json params = Json::object())
{
    return {{"version", 1}, {"method", method}, {"params", std::move(params)}};
}

} // namespace

namespace {} // namespace

TEST(TerminalCommands, TracesContainOnlyMetadata)
{
    terminal::Application app;
    // The Terminal's log, written to a file of its own here.
    const auto installation =
        std::filesystem::temp_directory_path() / ("ast-terminal-log-" + unique_process_id());
    std::filesystem::create_directories(installation / "config");
    std::ofstream(installation / "config" / "logging.json")
        << R"({"console":{"enabled":false,"level":"info"},)" R"("file":{"enabled":true,"level":"info","pattern":"%v","path":"../logs/commands.log","retention_days":1}})";
    log::init(installation, "terminal");
    const std::string outer = "fixture.outer";
    TraceScope trace(outer);
    EXPECT_TRUE(app.dispatch(request("market.endpoint.clear")).at("market_endpoint").is_null());
    EXPECT_THROW(app.dispatch(request("missing", {{"password", "fixture-secret"}})), Error);
    EXPECT_THROW(
        app.dispatch(request("market.endpoint.clear", {{"token", "fixture-secret"}})),
        std::invalid_argument);
    EXPECT_EQ(current_trace_id(), outer);
    std::vector<std::string> lines;
    for (const auto& entry : std::filesystem::directory_iterator(installation / "logs")) {
        std::ifstream input(entry.path());
        for (std::string line; std::getline(input, line);)
            lines.push_back(line);
    }
    ASSERT_EQ(lines.size(), 3U);
    std::set<std::string> ids;
    const std::regex shape(R"((\S+) (completed|failed): trace_id=(\S+) duration_ns=\d+)");
    for (const auto& line : lines) {
        std::smatch match;
        ASSERT_TRUE(std::regex_match(line, match, shape)) << line;
        EXPECT_NE(match[3].str(), outer);
        EXPECT_NO_THROW(validate_id(match[3].str()));
        EXPECT_TRUE(ids.insert(match[3].str()).second);
        EXPECT_EQ(line.find("fixture-secret"), std::string::npos);
    }
    EXPECT_NE(lines[0].find(" completed: "), std::string::npos);
    EXPECT_TRUE(lines[1].starts_with("terminal.unknown failed: ")) << lines[1];
    EXPECT_TRUE(app.dispatch(request("market.endpoint.clear")).at("market_endpoint").is_null());
    const auto diagnostics = app.dispatch(request("runtime.snapshot")).at("diagnostics");
    EXPECT_EQ(diagnostics.at("succeeded"), 2);
    EXPECT_EQ(diagnostics.at("failed"), 2);
    EXPECT_EQ(current_trace_id(), outer);
    std::filesystem::remove_all(installation);
}

namespace {

// A real account IPC channel with an explicitly held mutation. No Agent or
// broker state is touched: this exercises Terminal concurrency at its boundary.
struct LiveService
{
    std::filesystem::path root =
        std::filesystem::path("/tmp") / ("ast-live-" + unique_process_id().substr(0, 12));
    std::string endpoint = (root / "service").string();
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false, released;
    std::unique_ptr<service::RpcHost> host;
    Progress progress;
    terminal::TradingEndpoint target;
    std::future<bool> running;

    explicit LiveService(bool block)
        : released(!block)
    {
        service::reset_stop_request();
        std::filesystem::create_directory(root);
        ipc::create_identity(root / "server.crt", root / "server.key", "test server");
        ipc::create_identity(root / "client.crt", root / "client.key", "test client");
        asio::io_context io;
        asio::ip::tcp::acceptor reserve(io, {asio::ip::make_address("127.0.0.1"), 0});
        const auto port = reserve.local_endpoint().port();
        reserve.close();
        target = {
            "127.0.0.1",
            "live-fixture",
            port,
            {(root / "client.crt").string(),
                (root / "client.key").string(),
                {ipc::certificate_fingerprint(root / "server.crt")}}
        };
        host = std::make_unique<service::RpcHost>(
            service::Transport{
                {},
                "127.0.0.1",
                port,
                {(root / "server.crt").string(),
                    (root / "server.key").string(),
                    {ipc::certificate_fingerprint(root / "client.crt")}}
            },
            [this](const service::RpcHost::Peer&, std::string frame) -> service::RpcHost::Reply {
                auto work = std::make_shared<std::future<std::string>>(
                    std::async(std::launch::async, [this, frame = std::move(frame)] {
                        protocol::proto::Request message;
                        if (!message.ParseFromString(frame))
                            throw std::runtime_error("invalid fixture request");
                        protocol::proto::Response response;
                        response.set_version(1);
                        response.mutable_account()->set_account_id("fixture-record");
                        response.mutable_account()->set_environment(
                            protocol::proto::BROKER_SIMULATION);
                        response.mutable_account()->mutable_broker()->set_user_id("000001");
                        response.mutable_account()->mutable_broker()->set_broker_id("9999");
                        response.mutable_account()->mutable_broker()->set_app_id("test");
                        response.mutable_account()->mutable_broker()->set_front(
                            "tcp://127.0.0.1:1");
                        response.set_session_id(message.session_id());
                        response.set_correlation_id(message.correlation_id());
                        if (message.has_heartbeat()) {
                            response.mutable_health()->set_initialized(true);
                            response.mutable_health()->set_instance_id("live-fixture");
                            response.mutable_health()->set_version("0.1.0");
                            auto* execution = response.mutable_health()->mutable_execution();
                            execution->mutable_io()->set_observed(true);
                            execution->mutable_state()->set_observed(true);
                            execution->mutable_persistence()->set_observed(true);
                            execution->set_business_ready(true);
                        } else {
                            {
                                std::unique_lock lock(mutex);
                                // Reads are answered at once; anything else may be held.
                                if (!message.has_attach()) {
                                    entered = true;
                                    condition.notify_all();
                                    if (!condition.wait_for(lock, 10s, [&] {
                                            return released;
                                        }))
                                        throw std::runtime_error(
                                            "fixture mutation was not released");
                                }
                            }
                            const auto input = protocol::encode_live_input(
                                {{"account_id", "fixture-record"},
                                    {"type", "live_ctp"},
                                    {"environment", "simulation"},
                                    {"broker",
                                        {{"front", "tcp://127.0.0.1:1"},
                                            {"broker_id", "9999"},
                                            {"user_id", "000001"},
                                            {"app_id", "test"}}},
                                    {"policy",
                                        {{"risk",
                                             {{"max_order_quantity", "5"},
                                                 {"max_gross_quantity", "10"},
                                                 {"max_working_orders", std::uint64_t{3}}}},
                                            {"max_price_deviation", "0.02"},
                                            {"contracts",
                                                Json::array(
                                                    {{{"venue", "SHFE"},
                                                        {"symbol", "rb2610"},
                                                        {"currency", "CNY"},
                                                        {"price_increment", "1"},
                                                        {"multiplier", "10"},
                                                        {"quantity_increment", "1"},
                                                        {"product", "rb"},
                                                        {"delivery_month", "2026-10"}}})}}}});
                            auto* snapshot = response.mutable_live();
                            snapshot->set_account_id("fixture-record");
                            snapshot->set_environment(input.environment());
                            snapshot->set_policy_revision("fixture-policy");
                            *snapshot->mutable_broker() = input.broker();
                            *snapshot->mutable_risk() = input.policy().risk();
                            *snapshot->mutable_contracts() = input.policy().contracts();
                            *snapshot->mutable_max_price_deviation() =
                                input.policy().max_price_deviation();
                            snapshot->set_phase("ready");
                            snapshot->set_trading_day("20261009");
                            snapshot->mutable_capacity()->set_records_limit(100001);
                            snapshot->mutable_capacity()->set_bytes_limit(268435456);
                        }
                        return response.SerializeAsString();
                    }));
                return [work]() -> std::optional<service::RpcHost::Message> {
                    if (work->wait_for(0ms) != std::future_status::ready)
                        return std::nullopt;
                    return work->get();
                };
            },
            service::RpcHost::Options{},
            progress);
        running = std::async(std::launch::async, [&] {
            return host->run();
        });
    }

    terminal::TradingEndpoint address() const
    {
        return target;
    }

    bool wait()
    {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, testing_support::bound(5s), [&] {
            return entered;
        });
    }

    void release()
    {
        std::lock_guard lock(mutex);
        released = true;
        condition.notify_all();
    }

    ~LiveService()
    {
        release();
        service::request_stop();
        if (running.valid())
            running.wait();
        host.reset();
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        service::reset_stop_request();
    }
};

} // namespace

TEST(TerminalApplication, DelayedSnapshotKeepsItsRevisionWithoutReplacingANewerPublication)
{
    terminal::Application app;
    app.dispatch(request("runtime.snapshot"));
    std::future<void> old;
    Payload encoded;
    std::exception_ptr failure;
    {
        std::latch entered(1);
        std::promise<void> release;
        auto released = release.get_future().share();
        std::future<void> pending;

        struct Release
        {
            std::promise<void>& signal;
            std::future<void>& pending;
            std::future<void>& old;

            ~Release()
            {
                signal.set_value();
                if (pending.valid())
                    pending.wait();
                if (old.valid())
                    old.wait();
            }
        } guard{release, pending, old};

        pending = terminal::ApplicationTestAccess::slow_read(
            app,
            entered,
            released,
            terminal::ServiceIo::ReadLane::response);
        entered.wait();
        old = app.request(
            request("runtime.snapshot").dump(),
            [&](Payload value, std::exception_ptr error) noexcept {
                encoded = std::move(value);
                failure = error;
            });
        // This owner operation follows capture of the old request, while its renderer
        // is still waiting. Publishing cannot depend on the response worker.
        terminal::ApplicationTestAccess::count_success(app);
        EXPECT_EQ(old.wait_for(0s), std::future_status::timeout);
    }
    old.get();
    ASSERT_FALSE(failure);
    const auto delayed = parse_json(*encoded).at("result");
    const auto current = app.dispatch(request("runtime.snapshot", {{"since", std::uint64_t{0}}}));
    EXPECT_GT(current.at("diagnostics").at("succeeded"), delayed.at("diagnostics").at("succeeded"));
    EXPECT_GT(current.at("revision"), delayed.at("revision"));
}

TEST(TerminalApplication, ClosingDrainsAdmittedRequestsBeforeReleasingApplicationState)
{
    auto app = std::make_unique<terminal::Application>();
    app->dispatch(request("runtime.snapshot"));
    std::promise<void> entered, release;
    const auto released = release.get_future().share();
    auto held = terminal::ApplicationTestAccess::slow_settings(*app, entered, released);
    entered.get_future().wait();
    std::atomic<unsigned> completions{0};
    auto pending = app->request(
        request("market.endpoint.clear").dump(),
        [&](Payload, std::exception_ptr) noexcept {
            ++completions;
        });
    std::promise<void> closing;
    auto stopped = std::async(std::launch::async, [owned = std::move(app), &closing]() mutable {
        closing.set_value();
        owned.reset();
    });
    closing.get_future().wait();
    EXPECT_EQ(stopped.wait_for(30ms), std::future_status::timeout);
    release.set_value();
    EXPECT_NO_THROW(stopped.get());
    EXPECT_NO_THROW(held.get());
    EXPECT_NO_THROW(pending.get());
    EXPECT_EQ(completions.load(), 1U);
}

TEST(TerminalApplication, WireResponsesCompleteWhileReadWorkersAreBlocked)
{
    terminal::Application app;
    std::latch entered(2);
    std::promise<void> release;
    const auto released = release.get_future().share();
    Payload response;
    std::exception_ptr failure;
    std::vector<std::future<void>> pending;

    struct Release
    {
        std::promise<void>& signal;
        std::vector<std::future<void>>& pending;

        ~Release()
        {
            signal.set_value();
            for (auto& operation : pending)
                operation.wait();
        }
    } guard{release, pending};

    for (unsigned i = 0; i < 2; ++i)
        pending.push_back(terminal::ApplicationTestAccess::slow_read(app, entered, released));
    entered.wait();
    auto complete = [&](Payload value, std::exception_ptr error) noexcept {
        response = std::move(value);
        failure = error;
    };
    pending.push_back(app.request(request("runtime.snapshot").dump(), complete));
    ASSERT_EQ(pending.back().wait_for(testing_support::bound(1s)), std::future_status::ready);
    EXPECT_FALSE(failure);
    EXPECT_TRUE(parse_json(*response).contains("result"));
    pending.push_back(app.request(request("market.endpoint.clear").dump(), complete));
    ASSERT_EQ(pending.back().wait_for(testing_support::bound(1s)), std::future_status::ready);
    EXPECT_FALSE(failure);
    EXPECT_TRUE(parse_json(*response).contains("result"));
    pending.push_back(app.request("{", complete));
    ASSERT_EQ(pending.back().wait_for(testing_support::bound(1s)), std::future_status::ready);
    EXPECT_FALSE(failure);
    EXPECT_EQ(parse_json(*response).at("error").at("code"), "invalid_request");
}

TEST(TerminalApplication, QueuedSnapshotRepliesKeepTheirAllowanceAndLeaveCommandRepliesAvailable)
{
    Payload held;
    {
        terminal::Application app;
        const auto call = [&](std::string wire) {
            Payload response;
            std::exception_ptr failure;
            app.request(
                   std::move(wire),
                   [&](Payload value, std::exception_ptr error) noexcept {
                       response = std::move(value);
                       failure = error;
                   })
                .get();
            if (failure)
                std::rethrow_exception(failure);
            return response;
        };
        const auto wire = request("runtime.snapshot").dump();
        const auto size = call(wire)->size();
        terminal::ApplicationTestAccess::snapshot_budget(app, size + size / 2);
        held = call(wire);
        EXPECT_TRUE(parse_json(*held).contains("result"));
        try {
            (void)call(wire);
            FAIL() << "queued snapshot reply lost its byte allowance";
        } catch (const Error& error) {
            EXPECT_EQ(error.code(), ErrorCode::resource_exhausted);
            EXPECT_STREQ(
                error.what(),
                "Native response capacity reached; command outcome may be unknown");
        }
        EXPECT_EQ(parse_json(*call("{")).at("error").at("code"), "invalid_request");
        std::jthread consumer([reply = std::move(held)]() mutable {
            reply.reset();
        });
        consumer.join();
        held = call(wire);
    }
    EXPECT_TRUE(parse_json(*held).contains("result"));
}

TEST(TerminalTrading, ARequestWaitingOnOneAccountDoesNotHoldBackAnother)
{
    LiveService slow(true), other(false);
    terminal::ServiceIo io;
    auto pending = terminal::call_trading(io, slow.address(), {{"liveCosts", Json::object()}});
    ASSERT_TRUE(slow.wait());
    const auto started = std::chrono::steady_clock::now();
    const auto reply = terminal::call_trading(
        io,
        other.address(),
        {{"accountId", "fixture-record"},
            {"policyRevision", "fixture-policy"},
            {"command", {{"requestId", "revoke"}, {"liveRevoke", Json::object()}}}})
                           .get();
    EXPECT_LT(std::chrono::steady_clock::now() - started, testing_support::bound(1s));
    EXPECT_EQ(reply.at("live").at("accountId"), "fixture-record");
    EXPECT_EQ(pending.wait_for(0s), std::future_status::timeout);
    slow.release();
    ASSERT_EQ(pending.wait_for(3s), std::future_status::ready);
    EXPECT_NO_THROW((void)pending.get());
}

TEST(TerminalTrading, ClosingCancelsAnUnansweredRequest)
{
    LiveService slow(true);
    auto io = std::make_unique<terminal::ServiceIo>();
    auto pending = terminal::call_trading(*io, slow.address(), {{"liveCosts", Json::object()}});
    ASSERT_TRUE(slow.wait());
    const auto started = std::chrono::steady_clock::now();
    io.reset();
    EXPECT_LT(std::chrono::steady_clock::now() - started, testing_support::bound(500ms));
    EXPECT_THROW(pending.get(), Error);
}

TEST(TerminalMarket, RequestsAndTheWatchUseSavedTlsWithoutStartingLab)
{
    const auto root =
        std::filesystem::temp_directory_path() / ("asterion-data-ui-" + unique_process_id());

    struct Cleanup
    {
        std::filesystem::path root;

        ~Cleanup()
        {
            std::error_code ignored;
            std::filesystem::remove_all(root, ignored);
        }
    } cleanup{root};

    testing_support::MarketHost host(root);
    terminal::Application app;
    const auto identity = app.dispatch(request("trading.identity.prepare"));
    std::ifstream input(root / "config/market.json");
    auto config = Json::parse(input);
    config["clients"].push_back(
        {{"fingerprint", identity.at("trading_fingerprint")}, {"role", "read"}});
    config["acquisition"]["exchanges"] = {"SHFE", "GFEX"};
    replace_file_durably(root / "config/market.json", config.dump());
    host.start();
    {
        auto ready = host.open();
    }
    app.dispatch(request(
        "market.endpoint.save",
        {{"host", "127.0.0.1"},
            {"port", static_cast<unsigned>(host.port)},
            {"fingerprint", host.server_pin}}));
    const auto call = [](Json operation) {
        return request("market.call", {{"request", std::move(operation)}});
    };
    auto first = terminal::ApplicationTestAccess::submit(
        app,
        call({{"acquisitionSources", Json::object()}}));
    auto second =
        terminal::ApplicationTestAccess::submit(app, call({{"datasets", Json::object()}}));
    const auto catalog = first.get();
    const auto& sources = catalog.at("market").at("acquisitionSources");
    EXPECT_FALSE(sources.value("canManage", false));
    ASSERT_EQ(sources.at("sources").size(), 3);
    EXPECT_EQ(sources.at("sources")[0].at("provider"), "gfex");
    EXPECT_EQ(sources.at("sources")[0].at("tables"), Json::array({"daily"}));
    EXPECT_EQ(sources.at("sources")[1].at("provider"), "recorder");
    EXPECT_EQ(sources.at("sources")[1].at("tables"), Json::array({"minutes"}));
    EXPECT_EQ(sources.at("sources")[2].at("provider"), "shfe");
    EXPECT_EQ(sources.at("sources")[2].at("tables"), Json::array({"daily", "settlement"}));
    EXPECT_TRUE(second.get().at("market").contains("datasets"));
    try {
        app.dispatch(call(
            {{"acquisitionStart",
                {{"id", "forbidden"},
                    {"provider", "shfe"},
                    {"table", "daily"},
                    {"beginDay", "2026-10-30"},
                    {"endDay", "2026-10-30"}}}}));
        FAIL() << "a read-only client started acquisition";
    } catch (const Error& error) {
        EXPECT_EQ(error.code(), ErrorCode::permission_denied);
    }
    EXPECT_THROW(app.dispatch(call({{"watch", Json::object()}})), std::invalid_argument);
    EXPECT_TRUE(
        app.dispatch(request("market.watch", {{"restart", false}})).at("market_watch").is_null());
    // An open watch hands the page its frames in order, the first one complete.
    app.dispatch(request("market.open"));
    Json frames = Json::array();
    const auto until = std::chrono::steady_clock::now() + testing_support::bound(5s);
    while (frames.empty() && std::chrono::steady_clock::now() < until) {
        frames = app.dispatch(request("market.watch", {{"restart", false}}))
                     .at("market_watch")
                     .at("frames");
        std::this_thread::sleep_for(50ms);
    }
    ASSERT_FALSE(frames.empty());
    EXPECT_FALSE(frames[0].value("subscriptionsDelta", false));
    EXPECT_TRUE(frames[0].contains("catalog"));
    for (std::size_t i = 1; i < frames.size(); ++i)
        if (frames[i].value("subscriptionsDelta", false))
            EXPECT_EQ(frames[i].at("baseSequence"), frames[i - 1].at("sequence"));
    EXPECT_FALSE(app.dispatch(request("runtime.snapshot")).contains("market"));
    app.dispatch(request("market.close"));
    EXPECT_FALSE(std::filesystem::exists(root / "data/acquisition/forbidden.pb"));
    app.dispatch(request("market.endpoint.clear"));
}

TEST(StrategyDirectory, BuiltInModulesAreReadOnlyCopiesThatFollowTheShippedVersion)
{
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / ("ast-research-" + unique_process_id());
    const auto builtin = root / "builtin", research = root / "research";
    const auto write = [](const fs::path& path, std::string_view text) {
        fs::create_directories(path.parent_path());
        std::ofstream(path) << text;
    };
    const auto read = [](const fs::path& path) {
        std::ifstream input(path);
        return std::string{std::istreambuf_iterator<char>(input), {}};
    };
    write(builtin / "strategies/ma.py", "v1");
    write(builtin / "factors/old.py", "old");
    write(research / "strategies/mine.py", "mine");
    write(research / "strategies/clash.py", "my clash");
    write(builtin / "strategies/clash.py", "built-in clash");
    terminal::sync_builtin_research(builtin, research);
    EXPECT_EQ(read(research / "strategies/ma.py"), "v1");
    EXPECT_EQ(
        fs::status(research / "strategies/ma.py").permissions() & fs::perms::owner_write,
        fs::perms::none);
    // A new version replaces the copy and drops what it no longer ships.
    write(builtin / "strategies/ma.py", "v2");
    fs::remove(builtin / "factors/old.py");
    terminal::sync_builtin_research(builtin, research);
    EXPECT_EQ(read(research / "strategies/ma.py"), "v2");
    EXPECT_FALSE(fs::exists(research / "factors/old.py"));
    // The user's own modules stay, even one named like a built-in.
    EXPECT_EQ(read(research / "strategies/mine.py"), "mine");
    EXPECT_EQ(read(research / "strategies/clash.py"), "my clash");
    fs::permissions(research / "strategies/ma.py", fs::perms::owner_write, fs::perm_options::add);
    fs::remove_all(root);
}
