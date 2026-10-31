#include <asterion/system/rpc_host.hpp>
#include <asterion/system/durable_file.hpp>
#include "../apps/clients/terminal/native/application_impl.hpp"
#include "support/timing.hpp"
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
                        return snapshot.render({});
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

    static void
    attach_live(Application& app, const std::string& id, const TradingEndpoint& endpoint)
    {
        auto account = std::make_shared<Application::Impl::LiveAccount>(
            TradingClient::open(app.impl_->service_io, endpoint).get());
        app.impl_->service_io
            .submit<void>([&](std::stop_token) -> PolledTask<void> {
                app.impl_->live[id] = std::move(account);
                co_return;
            })
            .get();
    }

    static void select_market(Application& app, const ServiceEndpoint& endpoint)
    {
        auto client = MarketClient::open(app.impl_->service_io, endpoint).get();
        app.impl_->service_io
            .submit<void>([&](std::stop_token) -> PolledTask<void> {
                app.impl_->market = std::move(client);
                co_return;
            })
            .get();
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
    EXPECT_TRUE(
        app.dispatch(request("research.repository.clear")).at("strategy_repository").is_null());
    EXPECT_THROW(app.dispatch(request("missing", {{"password", "fixture-secret"}})), Error);
    EXPECT_THROW(
        app.dispatch(request("research.repository.clear", {{"token", "fixture-secret"}})),
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
    EXPECT_TRUE(
        app.dispatch(request("research.repository.clear")).at("strategy_repository").is_null());
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
    bool entered = false, released, block_observations = false, revoke_received = false;
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
                                // A revoke is admitted when it arrives and answered in its turn,
                                // as the account service does.
                                if (message.command().has_live_revoke()) {
                                    revoke_received = true;
                                    condition.notify_all();
                                }
                                // A cancel is answered at once, as the account service answers
                                // one while another command waits.
                                if (!message.command().has_cancel()
                                    && (!message.has_attach() || block_observations)) {
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

    bool wait_revoke()
    {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, testing_support::bound(5s), [&] {
            return revoke_received;
        });
    }

    void hold_observations()
    {
        std::lock_guard lock(mutex);
        block_observations = true;
        released = entered = false;
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

TEST(TerminalLive, DelayedAccountDoesNotBlockOtherAccountControlsOrSnapshots)
{
    LiveService slow(true), other(false);
    terminal::Application app;
    terminal::ApplicationTestAccess::attach_live(app, "slow", slow.address());
    terminal::ApplicationTestAccess::attach_live(app, "other", other.address());
    auto pending = std::async(std::launch::async, [&] {
        return app.dispatch(request("live.costs", {{"account", "slow"}}));
    });
    ASSERT_TRUE(slow.wait());
    const auto started = std::chrono::steady_clock::now();
    for (const auto& operation : std::vector<Json>{
             request(
                 "live.act",
                 {{"account", "other"},
                     {"account_id", "fixture-record"},
                     {"policy_revision", "fixture-policy"},
                     {"request_id", "revoke"},
                     {"action", "live_revoke"}}),
             request(
                 "live.act",
                 {{"account", "other"},
                     {"account_id", "fixture-record"},
                     {"policy_revision", "fixture-policy"},
                     {"request_id", "cancel"},
                     {"action", "cancel"},
                     {"order_id", "resting"}}),
             request("live.disconnect", {{"account", "other"}}),
             request("runtime.snapshot"),
             request("research.repository.clear")
         })
        EXPECT_NO_THROW(app.dispatch(operation));
    EXPECT_LT(std::chrono::steady_clock::now() - started, testing_support::bound(1s));
    EXPECT_EQ(pending.wait_for(0s), std::future_status::timeout);
    EXPECT_THROW(app.dispatch(request("live.disconnect", {{"account", "slow"}})), Error);
    EXPECT_THROW(app.dispatch(request("live.close", {{"account", "slow"}})), Error);
    slow.release();
    ASSERT_EQ(pending.wait_for(3s), std::future_status::ready);
    EXPECT_NO_THROW((void)pending.get());
    EXPECT_NO_THROW(app.dispatch(request("live.close", {{"account", "slow"}})));
    EXPECT_FALSE(app.dispatch(request("runtime.snapshot")).at("live").contains("slow"));
}

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

TEST(TerminalApplication, WireResponsesCompleteWhileReadWorkersAreBlocked)
{
    LiveService service(false);
    terminal::Application app;
    terminal::ApplicationTestAccess::attach_live(app, "account", service.address());
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
    pending.push_back(
        app.request(request("live.disconnect", {{"account", "account"}}).dump(), complete));
    ASSERT_EQ(pending.back().wait_for(testing_support::bound(1s)), std::future_status::ready);
    EXPECT_FALSE(failure);
    EXPECT_TRUE(parse_json(*response).contains("result"));
    EXPECT_TRUE(service.wait());
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

TEST(TerminalLive, SlowSettingsWorkDoesNotBlockAccountControlsOrPublication)
{
    LiveService service(false);
    std::promise<void> entered, release;
    auto released = release.get_future().share();
    terminal::Application app;
    terminal::ApplicationTestAccess::attach_live(app, "account", service.address());
    std::future<void> pending;
    std::future<Json> selection;

    struct Release
    {
        std::promise<void>& signal;
        std::future<void>& pending;
        std::future<Json>& selection;

        ~Release()
        {
            signal.set_value();
            if (pending.valid())
                pending.wait();
            if (selection.valid())
                selection.wait();
        }
    } guard{release, pending, selection};

    pending = terminal::ApplicationTestAccess::slow_settings(app, entered, released);
    ASSERT_EQ(entered.get_future().wait_for(testing_support::bound(2s)), std::future_status::ready);
    selection = terminal::ApplicationTestAccess::submit(
        app,
        request("ctp.connections.market", {{"id", "unavailable"}}));
    auto overlapping = terminal::ApplicationTestAccess::submit(app, request("market.disconnect"));
    EXPECT_THROW((void)overlapping.get(), Error);
    EXPECT_EQ(selection.wait_for(0s), std::future_status::timeout);
    const auto before = app.dispatch(request("runtime.snapshot"));
    const auto start = std::chrono::steady_clock::now();
    EXPECT_NO_THROW(app.dispatch(request("live.disconnect", {{"account", "account"}})));
    const auto after = app.dispatch(request("runtime.snapshot"));
    EXPECT_GT(after.at("revision"), before.at("revision"));
    EXPECT_LT(std::chrono::steady_clock::now() - start, testing_support::bound(1s));
    EXPECT_EQ(pending.wait_for(0s), std::future_status::timeout);
}

TEST(TerminalLive, BackgroundPollDoesNotRejectAnAccountCommandAsConflicting)
{
    LiveService slow(false), other(false);
    terminal::Application app;
    terminal::ApplicationTestAccess::attach_live(app, "slow", slow.address());
    terminal::ApplicationTestAccess::attach_live(app, "other", other.address());
    slow.hold_observations();
    ASSERT_TRUE(slow.wait()) << "background refresh must enter its account poll";
    auto pending = std::async(std::launch::async, [&] {
        return app.dispatch(request("live.disconnect", {{"account", "slow"}}));
    });
    EXPECT_EQ(pending.wait_for(100ms), std::future_status::timeout);
    const auto started = std::chrono::steady_clock::now();
    EXPECT_NO_THROW(app.dispatch(request("live.disconnect", {{"account", "other"}})));
    EXPECT_LT(std::chrono::steady_clock::now() - started, testing_support::bound(1s));
    slow.release();
    ASSERT_EQ(pending.wait_for(3s), std::future_status::ready);
    EXPECT_NO_THROW((void)pending.get());
}

TEST(TerminalLive, ACancelAndARevokeAreNotHeldBackByAnAccountCommandInFlight)
{
    LiveService slow(false);
    terminal::Application app;
    terminal::ApplicationTestAccess::attach_live(app, "slow", slow.address());
    slow.hold_observations();
    const Json identity{
        {"account", "slow"},
        {"account_id", "fixture-record"},
        {"policy_revision", "fixture-policy"}
    };
    auto order = identity;
    order.update(
        {{"request_id", "submit.held"},
            {"action", "submit"},
            {"order_id", "held"},
            {"venue", "SHFE"},
            {"symbol", "rb2610"},
            {"side", "buy"},
            {"offset", "open"},
            {"quantity", "1"},
            {"price", "3500"}});
    auto pending = std::async(std::launch::async, [&] {
        return app.dispatch(request("live.act", std::move(order)));
    });
    ASSERT_TRUE(slow.wait());
    EXPECT_EQ(pending.wait_for(100ms), std::future_status::timeout);
    auto cancel = identity;
    cancel.update(
        {{"request_id", "cancel.resting"}, {"action", "cancel"}, {"order_id", "resting"}});
    const auto started = std::chrono::steady_clock::now();
    EXPECT_NO_THROW(app.dispatch(request("live.act", cancel)));
    EXPECT_LT(std::chrono::steady_clock::now() - started, testing_support::bound(1s));
    // Nor is a revoke: it reaches the account's service, which closes the
    // permission to send on admitting it, while the order is still on its way.
    // Its reply follows that order's.
    auto revoke = identity;
    revoke.update({{"request_id", "revoke.now"}, {"action", "live_revoke"}});
    auto revoking = std::async(std::launch::async, [&] {
        return app.dispatch(request("live.act", std::move(revoke)));
    });
    EXPECT_TRUE(slow.wait_revoke());
    EXPECT_EQ(pending.wait_for(0ms), std::future_status::timeout);
    EXPECT_EQ(revoking.wait_for(0ms), std::future_status::timeout);
    slow.release();
    ASSERT_EQ(pending.wait_for(3s), std::future_status::ready);
    EXPECT_NO_THROW((void)pending.get());
    ASSERT_EQ(revoking.wait_for(3s), std::future_status::ready);
    EXPECT_NO_THROW((void)revoking.get());
}

TEST(TerminalLive, ClosingAnAccountCancelsAnUnansweredObservation)
{
    LiveService slow(false);
    terminal::Application app;
    terminal::ApplicationTestAccess::attach_live(app, "slow", slow.address());
    slow.hold_observations();
    ASSERT_TRUE(slow.wait());
    const auto started = std::chrono::steady_clock::now();
    EXPECT_NO_THROW(app.dispatch(request("live.close", {{"account", "slow"}})));
    EXPECT_LT(std::chrono::steady_clock::now() - started, testing_support::bound(1s));
    EXPECT_FALSE(app.dispatch(request("runtime.snapshot")).at("live").contains("slow"));
}

TEST(TerminalLive, OwnerCanAwaitCommandsAndCloseWhileThePeerIsUnresponsive)
{
    LiveService slow(true);
    auto io = std::make_unique<terminal::ServiceIo>();
    auto client = terminal::TradingClient::open(*io, slow.address()).get();
    auto command = client->query_costs();
    ASSERT_TRUE(slow.wait());
    EXPECT_EQ(command.wait_for(0ms), std::future_status::timeout);
    auto closing = io->submit<void>([&](std::stop_token) -> PolledTask<void> {
        const auto view = co_await PollFuture{client->view()};
        EXPECT_EQ(view.at("session").at("account_id"), "fixture-record");
        client.reset();
    });
    ASSERT_EQ(closing.wait_for(testing_support::bound(500ms)), std::future_status::ready);
    EXPECT_NO_THROW(closing.get());
    ASSERT_EQ(command.wait_for(testing_support::bound(500ms)), std::future_status::ready);
    EXPECT_THROW(command.get(), Error);
    const auto started = std::chrono::steady_clock::now();
    io.reset(); // The lifetime coordinator drains cancellation before releasing transports.
    EXPECT_LT(std::chrono::steady_clock::now() - started, testing_support::bound(500ms));
}

namespace {

// A market service as the Terminal reads it: the state of one instance, and
// the minute series of any contract, which it may hold back.
struct QuoteService
{
    service::Transport address;
    Progress progress;
    const std::string instance;
    std::mutex mutex;
    std::condition_variable condition;
    bool asked = false, held;
    std::uint64_t sequence = 0;
    service::RpcHost host;
    std::jthread server;

    QuoteService(const std::string& name, bool hold)
        : address{
              "/tmp/ast-quotes-" + name + "-" + unique_process_id().substr(0, 12) + ".sock",
              {},
              0,
              {}
          },
          instance("instance-" + name),
          held(hold),
          host(
              address,
              [this](const auto&, std::string bytes) {
                  return serve(bytes);
              },
              {},
              progress),
          server([this] {
              (void)host.run();
          })
    {}

    ~QuoteService()
    {
        service::request_stop();
    }

    terminal::ServiceEndpoint endpoint(const std::string& service) const
    {
        return {"", service, 0, address.endpoint};
    }

    bool wait()
    {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, testing_support::bound(2s), [&] {
            return asked;
        });
    }

    void release()
    {
        std::lock_guard lock(mutex);
        held = false;
    }

private:
    service::RpcHost::Reply serve(const std::string& bytes)
    {
        market::proto::Request request;
        if (!request.ParseFromString(bytes))
            throw std::runtime_error("invalid fixture market request");
        market::proto::Response response;
        response.set_version(1);
        response.set_service_id(request.service_id());
        response.set_correlation_id(request.correlation_id());
        if (request.has_events())
            return [] {
                return std::optional<service::RpcHost::Message>{};
            };
        if (request.has_minutes()) {
            auto* series = response.mutable_minutes();
            *series->mutable_instrument() = request.minutes().instrument();
            series->set_trading_day("20261013");
            series->set_first_observation_ms(1);
            {
                std::lock_guard lock(mutex);
                asked = true;
            }
            condition.notify_all();
            return [this, reply = response.SerializeAsString()]()
                       -> std::optional<service::RpcHost::Message> {
                std::lock_guard lock(mutex);
                if (held)
                    return {};
                return service::RpcHost::Message(reply);
            };
        }
        return
            [this,
                response = std::move(response),
                watch = request.has_watch()]() mutable -> std::optional<service::RpcHost::Message> {
                LiveMarketSnapshot state;
                state.phase = MarketPhase::connected;
                state.sequence = ++sequence;
                *response.mutable_snapshot() = protocol::encode_market(state, instance);
                response.mutable_snapshot()->mutable_catalog()->set_phase("unconfigured");
                response.mutable_snapshot()->set_catalog_revision(1);
                return service::RpcHost::Message(response.SerializeAsString(), watch);
            };
    }
};

} // namespace

TEST(TerminalMarket, AMinuteSeriesNamesItsServiceAndIsRefusedOnceAnotherIsSelected)
{
    service::reset_stop_request();
    QuoteService first("a", true), second("b", false);
    terminal::Application app;
    const auto minutes = request("market.minutes", {{"venue", "SHFE"}, {"symbol", "rb2610"}});
    terminal::ApplicationTestAccess::select_market(app, first.endpoint("market-a"));
    auto late = std::async(std::launch::async, [&] {
        return app.dispatch(minutes);
    });
    ASSERT_TRUE(first.wait());
    // The other service is selected while the first has not answered.
    terminal::ApplicationTestAccess::select_market(app, second.endpoint("market-b"));
    first.release();
    ASSERT_EQ(late.wait_for(testing_support::bound(2s)), std::future_status::ready);
    try {
        (void)late.get();
        FAIL() << "the series of a service no longer selected was returned";
    } catch (const Error& error) {
        EXPECT_EQ(error.code(), ErrorCode::conflict);
    }
    const auto series = app.dispatch(minutes).at("intraday");
    EXPECT_EQ(series.at("service"), "market-b");
    EXPECT_EQ(series.at("instance_id"), "instance-b");
    EXPECT_EQ(series.at("symbol"), "rb2610");
}

TEST(TerminalMarket, SlowHistoryAndReadBackpressureLeaveControlsAvailable)
{
    service::reset_stop_request();
    service::Transport address;
    address.endpoint = "/tmp/ast-market-view-" + unique_process_id().substr(0, 12) + ".sock";
    Progress progress;
    std::promise<void> history_entered, minutes_entered;
    bool history_started = false, connected = true;
    std::uint64_t sequence = 0;
    service::RpcHost host(
        address,
        [&](const auto&, std::string bytes) -> service::RpcHost::Reply {
            market::proto::Request request;
            if (!request.ParseFromString(bytes))
                throw std::runtime_error("invalid fixture market request");
            market::proto::Response response;
            response.set_version(1);
            response.set_service_id(request.service_id());
            response.set_correlation_id(request.correlation_id());
            if (request.has_events()) {
                if (!history_started) {
                    history_started = true;
                    history_entered.set_value();
                }
                return [] {
                    return std::optional<std::string>{};
                };
            }
            if (request.has_minutes()) {
                minutes_entered.set_value();
                return [] {
                    return std::optional<std::string>{};
                };
            }
            if (request.has_disconnect())
                connected = false;
            return [&, response = std::move(response), watch = request.has_watch()]() mutable
                       -> std::optional<service::RpcHost::Message> {
                LiveMarketSnapshot state;
                state.phase = connected ? MarketPhase::connected : MarketPhase::disconnected;
                state.sequence = ++sequence;
                *response.mutable_snapshot() = protocol::encode_market(state, "fixture-instance");
                response.mutable_snapshot()->mutable_catalog()->set_phase("unconfigured");
                response.mutable_snapshot()->set_catalog_revision(1);
                return service::RpcHost::Message(response.SerializeAsString(), watch);
            };
        },
        {},
        progress);
    std::jthread server([&] {
        (void)host.run();
    });

    struct Stop
    {
        ~Stop()
        {
            service::request_stop();
        }
    } stop;

    terminal::ServiceIo io;
    auto client = terminal::MarketClient::open(
        io,
        terminal::ServiceEndpoint{"", "fixture", 0, address.endpoint})
                      .get();
    ASSERT_EQ(
        history_entered.get_future().wait_for(testing_support::bound(1s)),
        std::future_status::ready);
    const auto before = client->snapshot().get().at("sequence").get<std::uint64_t>();
    const auto until = std::chrono::steady_clock::now() + testing_support::bound(1s);
    while (client->snapshot().get().at("sequence").get<std::uint64_t>() <= before
        && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(2ms);
    EXPECT_GT(client->snapshot().get().at("sequence").get<std::uint64_t>(), before);
    auto minutes = client->minutes("SHFE", "rb2610");
    ASSERT_EQ(
        minutes_entered.get_future().wait_for(testing_support::bound(1s)),
        std::future_status::ready);
    std::latch reads_entered(2);
    std::promise<void> release_reads;
    const auto reads_released = release_reads.get_future().share();
    std::vector<std::future<void>> reads;

    struct ReleaseReads
    {
        std::promise<void>& signal;
        std::vector<std::future<void>>& reads;

        ~ReleaseReads()
        {
            signal.set_value();
            for (auto& work : reads)
                work.wait();
        }
    } release_guard{release_reads, reads};

    for (unsigned i = 0; i < 2; ++i)
        reads.push_back(io.submit<void>([&](std::stop_token) -> PolledTask<void> {
            co_await io.read<void>([&] {
                reads_entered.count_down();
                reads_released.wait();
            });
        }));
    reads_entered.wait();
    auto control = io.submit<void>([client](std::stop_token) -> PolledTask<void> {
        co_await PollFuture{client->disconnect()};
        const auto state = co_await PollFuture{client->snapshot()};
        EXPECT_EQ(state.at("phase"), "disconnected");
    });
    ASSERT_EQ(control.wait_for(testing_support::bound(500ms)), std::future_status::ready);
    EXPECT_NO_THROW(control.get());
    EXPECT_EQ(client->snapshot().get().at("phase"), "disconnected");
    auto close = io.submit<void>([&](std::stop_token) -> PolledTask<void> {
        client.reset();
        co_return;
    });
    EXPECT_EQ(close.wait_for(testing_support::bound(500ms)), std::future_status::ready);
    close.get();
    ASSERT_EQ(minutes.wait_for(testing_support::bound(500ms)), std::future_status::ready);
    EXPECT_THROW((void)minutes.get(), Error);
}
