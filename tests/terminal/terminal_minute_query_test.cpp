#include <asterion/research.pb.h>
#include <asterion/kernel/rpc_host.hpp>
#include <asterion/kernel/durable_file.hpp>
#include "node_enrollment.hpp"
#include "../apps/clients/terminal/native/application_impl.hpp"
#include "support/timing.hpp"
#include <asterion/kernel/environment.hpp>
#include <asterion/protocol/trading.hpp>
#include "terminal/blocking_service.hpp"
#include <asterion/kernel/process/artifact.hpp>
#include <future>
#include <latch>
#include <gtest/gtest.h>
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
    std::optional<std::string> previous_ = environment_variable("ASTERION_NODE_DIRECTORY");

    void SetUp() override
    {
        std::filesystem::create_directory(root_);
        ASSERT_EQ(::setenv("ASTERION_NODE_DIRECTORY", root_.c_str(), 1), 0);
    }

    void TearDown() override
    {
        if (previous_)
            ::setenv("ASTERION_NODE_DIRECTORY", previous_->c_str(), 1);
        else
            ::unsetenv("ASTERION_NODE_DIRECTORY");
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

    static void logger(Application& app, std::shared_ptr<Logger> sink)
    {
        app.impl_->service_io
            .submit<void>([&](std::stop_token) -> PolledTask<void> {
                app.impl_->logger = std::move(sink);
                co_return;
            })
            .get();
    }

    static void
    attach_live(Application& app, const std::string& id, const ServiceEndpoint& endpoint)
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

    static std::shared_ptr<NodeClient> connect_node(Application& app)
    {
        (void)app.dispatch(
            {{"version", 1}, {"method", "node.connect"}, {"params", {{"id", "fixture"}}}});
        return app.impl_->manager.node("fixture");
    }

    static void node(Application& app, std::shared_ptr<NodeClient> client)
    {
        (void)app.dispatch(
            {{"version", 1},
                {"method", client ? "node.connect" : "node.disconnect"},
                {"params", {{"id", "fixture"}}}});
    }

    // Changes what the core publishes, as an owner operation does.
    static void verification(Application& app, Json result)
    {
        app.impl_->service_io
            .submit<void>([&](std::stop_token) -> PolledTask<void> {
                app.impl_->credential_verification = std::move(result);
                app.impl_->publish();
                co_return;
            })
            .get();
    }

    static void data_only(Application& app, const ServiceEndpoint& endpoint)
    {
        auto data = DataClient::open(app.impl_->service_io, endpoint).get();
        app.impl_->service_io
            .submit<void>([&](std::stop_token) -> PolledTask<void> {
                app.impl_->adopt_research({});
                app.impl_->adopt_data(std::move(data));
                app.impl_->publish();
                co_return;
            })
            .get();
    }

    static void
    attach(Application& app, const ServiceEndpoint& endpoint, const ServiceEndpoint& data_endpoint)
    {
        auto client = ResearchClient::open(app.impl_->service_io, endpoint).get();
        auto data = DataClient::open(app.impl_->service_io, data_endpoint).get();
        app.impl_->service_io
            .submit<void>([&](std::stop_token) -> PolledTask<void> {
                app.impl_->adopt_data(std::move(data));
                app.impl_->adopt_research(std::move(client));
                app.impl_->publish();
                co_return;
            })
            .get();
    }
};

} // namespace asterion::terminal

namespace {

struct MinuteService
{
    std::filesystem::path root;
    std::string endpoint;
    std::mutex mutex;
    std::condition_variable condition;
    unsigned entered = 0;
    bool released = false;
    std::atomic<bool> wrong_identity{false}, reference{false};
    research::proto::ResearchSubmit submitted;
    data::proto::AcquisitionSubmit acquisition;
    unsigned requested_before_sequence = 0;
    unsigned research_cancels = 0, data_cancels = 0;
    data::proto::DownloadAuthorizationRequest captured_authorization;
    data::proto::DownloadBudgetConfiguration configured_budget;
    std::unique_ptr<testing_support::BlockingService> host;
    std::future<bool> running;
    std::unique_ptr<testing_support::BlockingService> data_host;
    std::future<bool> data_running;

    MinuteService()
    {
        service::reset_stop_request();
        root = std::filesystem::temp_directory_path()
            / ("ast-query-" + unique_process_id().substr(0, 12));
        endpoint = (root / "service").string();
        std::filesystem::create_directory(root);
        host = std::make_unique<testing_support::BlockingService>(
            service::Transport{endpoint, {}, 0, {}},
            [this](testing_support::Connection& connection, std::stop_token) {
                research::proto::ResearchRequest request;
                if (!request.ParseFromString(connection.receive(2s)))
                    throw std::runtime_error("bad request");
                task::proto::TaskResponse response;
                response.set_version(1);
                response.set_service_id(request.service_id());
                response.set_correlation_id(request.correlation_id());
                if (request.has_list()) {
                    std::lock_guard lock(mutex);
                    requested_before_sequence = request.list().before_sequence();
                    response.mutable_tasks()->mutable_capacity()->set_active_limit(1000);
                } else if (request.has_history_usage()) {
                    {
                        std::unique_lock lock(mutex);
                        ++entered;
                        condition.notify_all();
                        if (!condition.wait_for(lock, 4s, [&] {
                                return released;
                            }))
                            throw std::runtime_error("fixture query was not released");
                    }
                    response.mutable_history_usage()->set_dataset_id(
                        wrong_identity ? std::string(64, 'b') : request.history_usage().id());
                    if (reference) {
                        auto* row = response.mutable_history_usage()->add_references();
                        row->set_kind(data::proto::HISTORY_BACKTEST);
                        row->set_id("external-task");
                        row->add_roles(data::proto::HISTORY_MARKET);
                    }
                } else if (request.has_result()) {
                    {
                        std::unique_lock lock(mutex);
                        ++entered;
                        condition.notify_all();
                        if (!condition.wait_for(lock, 4s, [&] {
                                return released;
                            }))
                            throw std::runtime_error("fixture result was not released");
                    }
                    response.mutable_error()->set_code("invalid_request");
                    response.mutable_error()->set_message("task has no confirmed result");
                } else if (request.has_cancel()) {
                    std::lock_guard lock(mutex);
                    ++research_cancels;
                    response.mutable_task()->set_id(request.cancel().id());
                    response.mutable_task()->set_state(task::proto::CANCEL_REQUESTED);
                } else if (request.has_submit()) {
                    std::unique_lock lock(mutex);
                    if (request.submit().has_factor_request()) {
                        ++entered;
                        condition.notify_all();
                        if (!condition.wait_for(lock, 4s, [&] {
                                return released;
                            }))
                            throw std::runtime_error("fixture submission was not released");
                    }
                    submitted = request.submit();
                    response.mutable_task()->set_id(submitted.id());
                } else
                    throw std::runtime_error("unexpected fixture request");
                connection.send(response.SerializeAsString(), 2s);
            });
        running = std::async(std::launch::async, [&] {
            return host->run();
        });
        data_host = std::make_unique<testing_support::BlockingService>(
            service::Transport{endpoint + ".data", {}, 0, {}},
            [this](testing_support::Connection& connection, std::stop_token) {
                data::proto::DataRequest request;
                if (!request.ParseFromString(connection.receive(2s)))
                    throw std::runtime_error("bad data request");
                data::proto::DataResponse response;
                response.set_version(1);
                response.set_service_id(request.service_id());
                response.set_correlation_id(request.correlation_id());
                if (request.has_resources()) {
                    {
                        std::unique_lock lock(mutex);
                        ++entered;
                        condition.notify_all();
                        if (!condition.wait_for(lock, 4s, [&] {
                                return released;
                            }))
                            throw std::runtime_error("fixture catalogue was not released");
                    }
                    auto* page = response.mutable_resources();
                    page->mutable_owner()->set_node_id("fixture-node");
                    page->mutable_owner()->set_service_id(request.service_id());
                    page->mutable_owner()->set_domain("data");
                    page->set_kind(request.resources().kind());
                } else if (request.has_acquisition_task()) {
                    std::lock_guard lock(mutex);
                    const auto& operation = request.acquisition_task();
                    auto* result = response.mutable_acquisition_task();
                    if (operation.has_list()) {
                        result->mutable_tasks()->mutable_capacity()->set_active_limit(1000);
                    } else if (operation.has_cancel()) {
                        ++data_cancels;
                        result->mutable_task()->set_id(operation.cancel().id());
                        result->mutable_task()->set_state(task::proto::CANCEL_REQUESTED);
                    } else if (operation.has_submit()) {
                        acquisition = operation.submit();
                        result->mutable_task()->set_id(acquisition.id());
                    } else {
                        throw std::runtime_error("unexpected acquisition fixture request");
                    }
                } else if (request.has_configure_download_budget()) {
                    std::lock_guard lock(mutex);
                    configured_budget = request.configure_download_budget();
                    auto* policy = response.mutable_download_budget();
                    policy->set_provider_id("asterion.data.tushare");
                    policy->set_requests_per_minute(configured_budget.requests_per_minute());
                } else if (request.has_authorize_download()) {
                    std::lock_guard lock(mutex);
                    captured_authorization = request.authorize_download();
                    auto* authorized = response.mutable_download_authorization();
                    authorized->set_id(std::string(64, 'a'));
                    authorized->set_data_instance(request.service_id());
                    authorized->set_task_instance(captured_authorization.task_instance());
                    authorized->set_task_id(captured_authorization.task_id());
                } else if (request.has_sources()) {
                    auto* source = response.mutable_sources()->add_sources();
                    source->set_id("tushare.ft_mins");
                    source->set_name("Isolated minute fixture");
                    source->set_plugin_id("fixture.provider");
                    source->set_normalization("fixture.minutes");
                    source->set_timezone("Asia/Shanghai");
                    source->set_timestamp_semantics("bar_end");
                    source->add_venues("SHFE");
                    source->add_intervals(1);
                    source->set_max_requests_per_minute(500);
                } else if (request.has_minute_retirement()) {
                    {
                        std::unique_lock lock(mutex);
                        ++entered;
                        condition.notify_all();
                        if (!condition.wait_for(lock, 4s, [&] {
                                return released;
                            }))
                            throw std::runtime_error("fixture reference query was not released");
                    }
                    auto* result = response.mutable_minute_retirement();
                    result->set_id(
                        wrong_identity ? std::string(64, 'b') : request.minute_retirement().id());
                    result->set_state(
                        reference ? data::proto::MinuteRetirement::BLOCKED
                                  : data::proto::MinuteRetirement::AVAILABLE);
                    result->set_payload_bytes(2048);
                    result->mutable_internal_usage()->set_dataset_id(result->id());
                    if (reference) {
                        for (unsigned i = 0; i < 2; ++i) {
                            auto* hold = result->add_holds();
                            hold->set_phase(
                                i ? data::proto::DataRetentionRecord::CONFIRMED
                                  : data::proto::DataRetentionRecord::PREPARED);
                            auto* consumer = hold->mutable_retention()->mutable_consumer();
                            consumer->mutable_owner()->set_node_id("offline-consumer-node");
                            consumer->mutable_owner()->set_service_id("research-not-running");
                            consumer->mutable_owner()->set_domain("research");
                            consumer->set_kind("backtest");
                            consumer->set_id("retained-run-" + std::to_string(i));
                            consumer->set_version(std::string(64, 'c' + i));
                        }
                    }
                } else if (request.has_history_usage()) {
                    response.mutable_history_usage()->set_dataset_id(request.history_usage().id());
                } else if (request.has_update_plan()) {
                    {
                        std::unique_lock lock(mutex);
                        ++entered;
                        condition.notify_all();
                        if (!condition.wait_for(lock, 4s, [&] {
                                return released;
                            }))
                            throw std::runtime_error("fixture query was not released");
                    }
                    auto* plan = response.mutable_update_plan();
                    *plan->mutable_query() = request.update_plan();
                    auto* input = plan->mutable_minutes();
                    input->set_source("tushare.ft_mins");
                    input->set_source_instrument("CU2403.SHF");
                    input->set_contract_id("SHFE/cu/2024-03");
                    input->set_interval_minutes(1);
                    input->set_begin_ns(parse_shanghai_time("2024-03-01 00:00:00"));
                    input->set_end_ns(parse_shanghai_time(plan->query().end_day() + " 23:59:59"));
                    input->set_requests_per_minute(plan->query().requests_per_minute());
                    plan->set_id(sha256_bytes(plan->SerializeAsString()));
                } else if (request.has_minute_page()) {
                    {
                        std::unique_lock lock(mutex);
                        ++entered;
                        condition.notify_all();
                        if (!condition.wait_for(lock, 4s, [&] {
                                return released;
                            }))
                            throw std::runtime_error("fixture query was not released");
                    }
                    const auto& query = request.minute_page();
                    auto* page = response.mutable_minute_page();
                    page->set_task_id(query.dataset_id());
                    page->set_offset(query.offset());
                    page->set_limit(query.limit());
                    page->set_source("tushare.ft_mins");
                    page->set_contract_id("SHFE/cu/2023-10");
                    page->set_interval_minutes(1);
                    page->set_manifest_sha256(std::string(64, 'a'));
                    page->set_begin_ns(query.begin_ns() ? query.begin_ns() : 1);
                    page->set_end_ns(query.end_ns() ? query.end_ns() : 2);
                } else if (request.has_table_contracts()) {
                    // Copper's March month, as the source's contract list lists it.
                    const auto& query = request.table_contracts();
                    auto* list = response.mutable_table_contracts();
                    list->set_source(query.source());
                    if (query.exchange() == "SHFE" && query.product() == "cu") {
                        auto* item = list->add_items();
                        item->set_contract_id("SHFE/cu/2024-03");
                        item->set_source_instrument("CU2403.SHF");
                        item->set_name("copper");
                        item->set_list_date("2023-01-01");
                        item->set_delist_date("2024-03-15");
                        item->mutable_per_unit()->set_units(Decimal::parse("5.00000001").raw());
                        item->set_trade_unit("tonne");
                        item->set_quote_unit("CNY/tonne");
                    }
                } else if (request.has_table_daily()) {
                    // No candles: what is checked here is whose they are.
                    const auto& query = request.table_daily();
                    auto* page = response.mutable_table_daily();
                    *page->mutable_daily() = query.daily();
                    page->set_contract(wrong_identity ? "SHFE/cu/2024-04" : query.contract());
                    page->set_period(query.period());
                    page->set_limit(query.limit());
                } else {
                    throw std::runtime_error("unexpected data fixture request");
                }
                connection.send(response.SerializeAsString(), 2s);
            });
        data_running = std::async(std::launch::async, [&] {
            return data_host->run();
        });
    }

    terminal::ServiceEndpoint address() const
    {
        return {{}, "minute-fixture", 0, {}, endpoint, "local", "fixture-node"};
    }

    terminal::ServiceEndpoint data_address() const
    {
        return {{}, "data-fixture", 0, {}, endpoint + ".data", "local", "fixture-node"};
    }

    bool wait(unsigned count)
    {
        std::unique_lock lock(mutex);
        return condition.wait_for(lock, 1s, [&] {
            return entered >= count;
        });
    }

    void release()
    {
        std::lock_guard lock(mutex);
        released = true;
        condition.notify_all();
    }

    void reset()
    {
        std::lock_guard lock(mutex);
        entered = 0;
        released = false;
    }

    ~MinuteService()
    {
        release();
        service::request_stop();
        if (running.valid())
            running.wait();
        if (data_running.valid())
            data_running.wait();
        data_host.reset();
        host.reset();
        std::error_code error;
        std::filesystem::remove_all(root, error);
        service::reset_stop_request();
    }
};

Json request(const std::string& method, Json params = Json::object())
{
    return {{"version", 1}, {"method", method}, {"params", std::move(params)}};
}

Json query(const std::string& id)
{
    return request(
        "data.minutes.page",
        {{"id", id},
            {"offset", 0},
            {"limit", 100},
            {"start", ""},
            {"end", ""},
            {"include_macd", true}});
}

} // namespace

namespace {

class CommandLogSink final : public spdlog::sinks::base_sink<std::mutex>
{
public:
    std::vector<Json> records;
    bool fail = false;

private:
    void sink_it_(const spdlog::details::log_msg& message) override
    {
        if (fail)
            throw std::runtime_error("fixture log sink failed");
        records.push_back(
            Json::parse(std::string_view(message.payload.data(), message.payload.size())));
    }

    void flush_() override {}
};

} // namespace

TEST(TerminalCommands, TracesContainOnlyMetadataAndLoggingFailurePreservesResults)
{
    auto sink = std::make_shared<CommandLogSink>();
    auto logger = std::make_shared<Logger>(std::make_shared<spdlog::logger>("commands", sink));
    terminal::Application app;
    terminal::ApplicationTestAccess::logger(app, logger);
    const std::string outer = "fixture.outer";
    TraceScope trace(outer);
    EXPECT_TRUE(app.dispatch(request("data.dataset.clear")).at("datasets").empty());
    EXPECT_THROW(app.dispatch(request("missing", {{"password", "fixture-secret"}})), Error);
    EXPECT_THROW(
        app.dispatch(request("data.dataset.clear", {{"token", "fixture-secret"}})),
        std::invalid_argument);
    EXPECT_EQ(current_trace_id(), outer);
    ASSERT_EQ(sink->records.size(), 3U);
    std::set<std::string> ids;
    for (const auto& record : sink->records) {
        const auto& fields = record.at("fields");
        EXPECT_EQ(fields.size(), 3U);
        EXPECT_GE(fields.at("duration_ns").get<std::int64_t>(), 0);
        const auto id = fields.at("trace_id").get<std::string>();
        EXPECT_NE(id, outer);
        EXPECT_NO_THROW(validate_id(id));
        EXPECT_TRUE(ids.insert(id).second);
        EXPECT_EQ(record.dump().find("fixture-secret"), std::string::npos);
    }
    EXPECT_TRUE(sink->records[0].at("fields").at("success"));
    EXPECT_FALSE(sink->records[1].at("fields").at("success"));
    EXPECT_EQ(sink->records[1].at("event"), "terminal.unknown");
    sink->fail = true;
    EXPECT_TRUE(app.dispatch(request("data.dataset.clear")).at("datasets").empty());
    EXPECT_GT(logger->failures(), 0U);
    const auto diagnostics = app.dispatch(request("runtime.snapshot")).at("diagnostics");
    EXPECT_EQ(diagnostics.at("succeeded"), 2);
    EXPECT_EQ(diagnostics.at("failed"), 2);
    EXPECT_EQ(current_trace_id(), outer);
}

TEST(TerminalServiceClients, DataQueriesAndTaskControlRemainIndependentWhenAPeerIsOffline)
{
    MinuteService service;
    service.release();
    terminal::Application app;
    auto missing_task = service.address();
    missing_task.endpoint += ".missing";
    terminal::ApplicationTestAccess::attach(app, missing_task, service.data_address());
    auto snapshot = app.dispatch(request("runtime.snapshot"));
    EXPECT_FALSE(snapshot.at("research_service").at("online"));
    EXPECT_TRUE(snapshot.at("data").at("online"));
    EXPECT_EQ(
        app.dispatch(query("published-version")).at("history_page").at("id"),
        "published-version");
    auto missing_data = service.data_address();
    missing_data.endpoint += ".missing";
    terminal::ApplicationTestAccess::attach(app, service.address(), missing_data);
    snapshot = app.dispatch(request("runtime.snapshot"));
    EXPECT_TRUE(snapshot.at("research_service").at("online"));
    EXPECT_FALSE(snapshot.at("data").at("online"));
    const auto& owner = snapshot.at("research_service");
    EXPECT_NO_THROW(app.dispatch(request(
        "task.action",
        {{"id", "task"},
            {"action", "cancel"},
            {"owner",
                {{"kind", "research"},
                    {"node_id", owner.at("node_id")},
                    {"service", owner.at("service")},
                    {"connection_id", owner.at("connection_id")}}}})));
}

TEST(TerminalServiceClients, CollidingTaskIdsRequireTheExactSelectedOwner)
{
    MinuteService service;
    service.release();
    terminal::Application app;
    terminal::ApplicationTestAccess::attach(app, service.address(), service.data_address());
    const auto snapshot = app.dispatch(request("runtime.snapshot"));
    auto owner = [&](const char* key, const char* kind) {
        const auto& selected = snapshot.at(key);
        return Json{
            {"kind", kind},
            {"node_id", selected.at("node_id")},
            {"service", selected.at("service")},
            {"connection_id", selected.at("connection_id")}
        };
    };
    const auto research = owner("research_service", "research");
    const auto data = owner("data", "data");
    EXPECT_NO_THROW(app.dispatch(
        request("task.action", {{"owner", research}, {"id", "same-id"}, {"action", "cancel"}})));
    EXPECT_NO_THROW(app.dispatch(
        request("task.action", {{"owner", data}, {"id", "same-id"}, {"action", "cancel"}})));
    auto stale = data;
    stale["connection_id"] = "obsolete-connection";
    EXPECT_THROW(
        app.dispatch(
            request("task.action", {{"owner", stale}, {"id", "same-id"}, {"action", "cancel"}})),
        Error);
    auto wrong = data;
    wrong["kind"] = "research";
    EXPECT_THROW(
        app.dispatch(
            request("task.action", {{"owner", wrong}, {"id", "same-id"}, {"action", "cancel"}})),
        Error);
    std::lock_guard lock(service.mutex);
    EXPECT_EQ(service.research_cancels, 1);
    EXPECT_EQ(service.data_cancels, 1);
}

TEST(TerminalServiceClients, ReadPoolBackpressureLeavesTaskAndDataControlsAvailable)
{
    MinuteService service;
    service.release();
    std::latch release(1);
    std::mutex mutex;
    std::condition_variable entered;
    unsigned running = 0;
    std::thread::id owner;
    terminal::ServiceIo io;
    auto client = terminal::ResearchClient::open(io, service.address()).get();
    auto data = terminal::DataClient::open(io, service.data_address()).get();
    std::vector<std::future<int>> results;
    owner = io.submit<std::thread::id>([](std::stop_token) -> PolledTask<std::thread::id> {
                  co_return std::this_thread::get_id();
              }).get();
    {
        struct Release
        {
            std::latch& gate;

            ~Release()
            {
                gate.count_down();
            }
        } guard{release};

        const std::string trace_id = "fixture.read";
        TraceScope trace(trace_id);
        auto work = [&](std::stop_token) -> PolledTask<int> {
            const auto value = co_await io.read<int>([&] {
                EXPECT_NE(std::this_thread::get_id(), owner);
                EXPECT_EQ(current_trace_id(), "fixture.read");
                {
                    std::lock_guard lock(mutex);
                    ++running;
                    entered.notify_all();
                }
                release.wait();
                return 7;
            });
            EXPECT_EQ(std::this_thread::get_id(), owner);
            EXPECT_EQ(current_trace_id(), "fixture.read");
            co_return value;
        };
        for (unsigned i = 0; i < 2; ++i)
            results.push_back(io.submit<int>(work));
        {
            std::unique_lock lock(mutex);
            ASSERT_TRUE(entered.wait_for(lock, testing_support::bound(1s), [&] {
                return running == 2;
            }));
        }
        // Eight queued reads plus the two active workers exhaust the read budget.
        for (unsigned i = 0; i < 8; ++i)
            results.push_back(io.submit<int>(work));
        auto rejected = io.submit<int>(work);
        ASSERT_EQ(rejected.wait_for(testing_support::bound(500ms)), std::future_status::ready);
        EXPECT_THROW((void)rejected.get(), Error);
        auto cancel = client->action("running-task", "cancel");
        ASSERT_EQ(cancel.wait_for(testing_support::bound(500ms)), std::future_status::ready);
        EXPECT_NO_THROW(cancel.get());
        EXPECT_TRUE(client->status().get().at("online"));
        data::proto::DownloadBudgetConfiguration budget;
        budget.set_source("tushare.ft_mins");
        budget.set_requests_per_minute(60);
        auto configure = data->configure_download_budget(budget);
        ASSERT_EQ(configure.wait_for(testing_support::bound(500ms)), std::future_status::ready);
        EXPECT_NO_THROW(configure.get());
        EXPECT_TRUE(data->status().get().at("online"));
    }
    for (auto& result : results)
        EXPECT_EQ(result.get(), 7);
    EXPECT_EQ(
        client->history_usage(std::string(64, 'a')).get().at("dataset_id"),
        std::string(64, 'a'));
}

TEST(TerminalBacktest, ResultVerificationDoesNotBlockTaskCancellationOrSnapshots)
{
    MinuteService source;
    terminal::Application app;
    terminal::ApplicationTestAccess::attach(app, source.address(), source.data_address());
    const auto selected = app.dispatch(request("runtime.snapshot")).at("research_service");
    const Json owner{
        {"kind", "research"},
        {"node_id", selected.at("node_id")},
        {"service", selected.at("service")},
        {"connection_id", selected.at("connection_id")}
    };
    auto result = std::async(std::launch::async, [&] {
        return app.dispatch(request("task.result", {{"id", "completed"}, {"owner", owner}}));
    });
    ASSERT_TRUE(source.wait(1));
    auto cancel = std::async(std::launch::async, [&] {
        return app.dispatch(
            request("task.action", {{"id", "running"}, {"action", "cancel"}, {"owner", owner}}));
    });
    const auto ready = cancel.wait_for(testing_support::bound(500ms));
    const auto snapshot = app.dispatch(request("runtime.snapshot"));
    source.release();
    EXPECT_EQ(ready, std::future_status::ready);
    EXPECT_EQ(snapshot.at("protocol"), 1);
    EXPECT_NO_THROW(cancel.get());
    EXPECT_THROW(result.get(), Error);
}

TEST(TerminalMinuteQueries, ConcurrentReadersDoNotBlockCommandsOrPublishOldServicePages)
{
    MinuteService source;
    MinuteService replacement;
    terminal::Application app;
    terminal::ApplicationTestAccess::attach(app, source.address(), source.data_address());
    auto first = std::async(std::launch::async, [&] {
        return app.dispatch(query("first"));
    });
    EXPECT_TRUE(source.wait(1));
    auto second = std::async(std::launch::async, [&] {
        return app.dispatch(query("second"));
    });
    EXPECT_TRUE(source.wait(2))
        << "both chart reads must reach the service before either completes";
    const auto started = std::chrono::steady_clock::now();
    const auto snapshot = app.dispatch(request("runtime.snapshot"));
    EXPECT_EQ(snapshot.at("protocol"), 1);
    EXPECT_TRUE(snapshot.at("history_page").is_null());
    EXPECT_TRUE(app.dispatch(request("market.disconnect")).contains("protocol"));
    // This deliberately invalid mutation must reach validation, not fail as a busy command.
    EXPECT_THROW(app.dispatch(request("futures.inspect_csv")), std::exception);
    EXPECT_LT(std::chrono::steady_clock::now() - started, testing_support::bound(500ms));
    source.release();
    EXPECT_NO_THROW({ EXPECT_EQ(first.get().at("history_page").at("id"), "first"); });
    EXPECT_NO_THROW({ EXPECT_EQ(second.get().at("history_page").at("id"), "second"); });
    EXPECT_TRUE(app.dispatch(request("runtime.snapshot")).at("history_page").is_null());
    source.reset();
    auto obsolete = std::async(std::launch::async, [&] {
        return app.dispatch(query("obsolete"));
    });
    EXPECT_TRUE(source.wait(1));
    terminal::ApplicationTestAccess::attach(app, replacement.address(), replacement.data_address());
    source.release();
    try {
        (void)obsolete.get();
        FAIL() << "old service reply was accepted";
    } catch (const Error& error) {
        EXPECT_EQ(error.code(), ErrorCode::conflict);
        EXPECT_EQ(std::string(error.what()), "data service selection changed during minute query");
    }
    replacement.release();
    EXPECT_EQ(app.dispatch(query("current")).at("history_page").at("id"), "current");
    EXPECT_TRUE(app.dispatch(request("runtime.snapshot")).at("history_page").is_null());
}

TEST(TerminalMinuteQueries, OptionalTimeBoundsAreTypedAndDoNotRelaxOtherFields)
{
    MinuteService source;
    source.release();
    terminal::Application app;
    terminal::ApplicationTestAccess::attach(app, source.address(), source.data_address());
    const auto empty = app.dispatch(query("unfiltered"));
    EXPECT_EQ(empty.at("history_page").at("begin_ns"), "1");
    auto filtered = query("filtered");
    filtered["params"]["start"] = "2023-08-25 09:00:00";
    filtered["params"]["end"] = "2023-08-25 10:00:00";
    const auto result = app.dispatch(filtered);
    EXPECT_EQ(
        result.at("history_page").at("begin_ns"),
        std::to_string(parse_shanghai_time("2023-08-25 09:00:00")));
    for (const auto* key : {"start", "end"}) {
        auto invalid = query("invalid");
        invalid["params"][key] = 0;
        EXPECT_THROW(app.dispatch(invalid), std::invalid_argument);
        invalid["params"][key] = " ";
        EXPECT_THROW(app.dispatch(invalid), std::invalid_argument);
    }
    auto invalid = query("");
    EXPECT_THROW(app.dispatch(invalid), std::invalid_argument);
    invalid = query("invalid");
    invalid["params"]["limit"] = 201;
    EXPECT_THROW(app.dispatch(invalid), std::invalid_argument);
    invalid = query("invalid");
    invalid["params"]["unexpected"] = true;
    EXPECT_THROW(app.dispatch(invalid), std::invalid_argument);
}

TEST(TerminalDailyQueries, AChartOfTheDailyTableIsTypedAndOnlyItsOwnReplyIsPublished)
{
    MinuteService source;
    source.release();
    terminal::Application app;
    terminal::ApplicationTestAccess::attach(app, source.address(), source.data_address());
    auto chart = request(
        "data.tables.daily",
        {{"daily",
             {{"snapshot", std::string(64, 'b')},
                 {"daily_source", "fixture.daily.v1"},
                 {"calendar_source", "fixture.calendar.v1"},
                 {"contracts_source", "fixture.contracts.v1"}}},
            {"contract", "SHFE/cu/2024-03"},
            {"period", "week"},
            {"offset", 0},
            {"limit", 100}});
    const auto page = app.dispatch(chart).at("table_daily");
    EXPECT_EQ(page.at("contract"), "SHFE/cu/2024-03");
    EXPECT_EQ(page.at("period"), "week");
    EXPECT_EQ(page.at("daily").at("snapshot"), std::string(64, 'b'));
    EXPECT_FALSE(app.dispatch(request("runtime.snapshot")).contains("table_daily"));
    // A product has no candles of its own, and a period is one of five.
    for (const auto& change :
        std::vector<Json>{{{"contract", "SHFE/cu"}}, {{"period", "decade"}}, {{"limit", 201}}}) {
        auto bad = chart;
        bad["params"].update(change);
        EXPECT_THROW(app.dispatch(bad), std::invalid_argument);
    }
    source.wrong_identity = true;
    try {
        (void)app.dispatch(chart);
        FAIL() << "another contract's chart was accepted";
    } catch (const Error& error) {
        EXPECT_EQ(error.code(), ErrorCode::unavailable);
        EXPECT_EQ(std::string(error.what()), "daily table chart identity mismatch");
    }
}

// A download takes its contract's code and days from the contract list of
// the source's own data source, under the snapshot it was chosen from.
TEST(TerminalMinuteDownloads, SubmissionReadsTheContractListAndSendsTypedTimesToService)
{
    MinuteService source;
    source.release();
    terminal::Application app;
    terminal::ApplicationTestAccess::attach(app, source.address(), source.data_address());
    const auto snapshot = std::string(64, 'b');
    const auto listed = app.dispatch(request(
                                         "data.tables.contracts",
                                         {{"snapshot", snapshot},
                                             {"source", "tushare.ft_mins"},
                                             {"exchange", "SHFE"},
                                             {"product", "cu"}}))
                            .at("table_contracts");
    EXPECT_EQ(listed.at("snapshot"), snapshot);
    const auto& item = listed.at("items").at(0);
    EXPECT_EQ(item.at("code"), "SHFE/cu/2024-03");
    EXPECT_TRUE(item.at("multiplier").is_null());
    EXPECT_EQ(item.at("per_unit"), "5.00000001");
    EXPECT_EQ(item.at("trade_unit"), "tonne");
    EXPECT_EQ(item.at("quote_unit"), "CNY/tonne");
    const auto cutoff = parse_shanghai_time("2023-06-01 10:00:00");
    auto submission = request(
        "data.download.minutes.submit",
        {{"id", "minutes"},
            {"contract_id", "SHFE/cu/2024-04"},
            {"source", "tushare.ft_mins"},
            {"interval_minutes", 5},
            {"requests_per_minute", 60},
            {"token", "fixture-secret"},
            {"snapshot", snapshot},
            {"cutoff_ns", std::to_string(cutoff + 500000000)}});
    // A month the list does not have cannot be downloaded.
    EXPECT_THROW(app.dispatch(submission), std::invalid_argument);
    submission["params"]["contract_id"] = "SHFE/cu/2024-03";
    EXPECT_EQ(
        app.dispatch(request(
                         "data.download.budget.configure",
                         {{"source", "tushare.ft_mins"},
                             {"token", "fixture-secret"},
                             {"requests_per_minute", 20}}))
            .dump()
            .find("fixture-secret"),
        std::string::npos);
    const auto result = app.dispatch(submission);
    EXPECT_EQ(result.dump().find("fixture-secret"), std::string::npos);
    {
        std::lock_guard lock(source.mutex);
        ASSERT_TRUE(source.captured_authorization.has_minutes());
        EXPECT_EQ(source.configured_budget.credential(), "fixture-secret");
        EXPECT_EQ(source.configured_budget.requests_per_minute(), 20);
        // Submitting a task with its own pacing does not overwrite the shared budget.
        const auto& minutes = source.captured_authorization.minutes();
        EXPECT_EQ(minutes.requests_per_minute(), 60);
        EXPECT_EQ(minutes.interval_minutes(), 5U);
        EXPECT_EQ(minutes.source_instrument(), "CU2403.SHF");
        // From the day it was listed to the whole second of the cutoff.
        EXPECT_EQ(minutes.begin_ns(), parse_shanghai_time("2023-01-01 00:00:00"));
        EXPECT_EQ(minutes.end_ns(), cutoff);
        EXPECT_EQ(source.captured_authorization.credential(), "fixture-secret");
        EXPECT_EQ(source.acquisition.authorization(), std::string(64, 'a'));
        EXPECT_EQ(source.acquisition.SerializeAsString().find("fixture-secret"), std::string::npos);
    }
    for (const auto& change : std::vector<Json>{
             {{"cutoff_ns", "0"}},
             {{"cutoff_ns", "9999999999999999999"}},
             {{"snapshot", "b"}},
             {{"contract_id", "CU9999.SHF"}},
             {{"requests_per_minute", 501}},
             {{"requests_per_minute", 1.5}},
             {{"interval_minutes", 0}},
             {{"start", "2020-01-01"}}
         }) {
        auto bad = submission;
        bad["params"].update(change);
        EXPECT_THROW(app.dispatch(bad), std::invalid_argument);
    }
}

TEST(TerminalTableFactor, SubmissionDoesNotBlockOtherWindowsAndRejectsChangedService)
{
    MinuteService source, replacement;
    terminal::Application app;
    terminal::ApplicationTestAccess::attach(app, source.address(), source.data_address());
    const auto first_id =
        app.dispatch(request("runtime.snapshot")).at("research_service").at("connection_id");
    EXPECT_TRUE(first_id.is_string());
    EXPECT_FALSE(first_id.get<std::string>().empty());
    EXPECT_EQ(
        app.dispatch(request("runtime.snapshot")).at("research_service").at("connection_id"),
        first_id);
    const auto submit = request(
        "factor.submit",
        {{"id", "analysis"},
            {"series",
                {{"kind", "tables"},
                    {"snapshot", std::string(64, 'b')},
                    {"daily_source", "fixture.daily.v1"},
                    {"calendar_source", "fixture.calendar.v1"},
                    {"end_day", "2023-03-31"},
                    {"ranges",
                        {{{"begin_day", "2023-01-01"}, {"instruments", {"SHFE/cu/2024-03"}}}}}}},
            {"factor", "momentum"},
            {"lookbacks", {20}},
            {"horizon", 5},
            {"evaluation", {{"mode", "full_sample"}}}});
    auto pending = std::async(std::launch::async, [&] {
        return app.dispatch(submit);
    });
    ASSERT_TRUE(source.wait(1));
    EXPECT_TRUE(app.dispatch(request("market.disconnect")).contains("protocol"));
    EXPECT_EQ(app.dispatch(request("runtime.snapshot")).at("protocol"), 1);
    terminal::ApplicationTestAccess::attach(app, replacement.address(), replacement.data_address());
    EXPECT_NE(
        app.dispatch(request("runtime.snapshot")).at("research_service").at("connection_id"),
        first_id);
    source.release();
    try {
        (void)pending.get();
        FAIL() << "obsolete service response must not be published";
    } catch (const Error& error) {
        EXPECT_EQ(
            std::string(error.what()),
            "data/research service selection changed during operation; " "inspect the original services before retrying");
    }
    replacement.release();
    EXPECT_TRUE(app.dispatch(submit).contains("protocol"));
    EXPECT_EQ(replacement.submitted.data_owner().node_id(), "fixture-node");
    EXPECT_EQ(replacement.submitted.data_owner().service_id(), "data-fixture");
    const auto& accepted = replacement.submitted.factor_request();
    ASSERT_EQ(accepted.series_size(), 1);
    EXPECT_EQ(accepted.series(0).tables().snapshot(), std::string(64, 'b'));
    EXPECT_EQ(accepted.lookbacks(0), 20U);
    auto bad = submit;
    bad["params"]["lookbacks"] = {1.5};
    EXPECT_THROW(app.dispatch(bad), std::invalid_argument);
    // A client names what Data reads; it cannot hand in observations.
    bad = submit;
    bad["params"]["series"]["bars"] = Json::array();
    EXPECT_THROW(app.dispatch(bad), std::invalid_argument);
    bad = submit;
    bad["params"]["series"] = {{"kind", "uploaded"}};
    EXPECT_THROW(app.dispatch(bad), std::invalid_argument);
    // A published daily version is no factor series any more.
    bad = submit;
    bad["params"]["series"] = {{"kind", "daily"}, {"dataset_id", std::string(64, 'a')}};
    EXPECT_THROW(app.dispatch(bad), std::invalid_argument);
}

TEST(TerminalHistoryUpdate, DataPlansAndAdmissionRemainAvailableWithResearchOffline)
{
    MinuteService source;
    source.release();
    terminal::Application app;
    auto offline = source.address();
    offline.endpoint += ".offline";
    terminal::ApplicationTestAccess::attach(app, offline, source.data_address());
    const Json params = {
        {"dataset_id", std::string(64, 'a')},
        {"daily", nullptr},
        {"mode", "extend"},
        {"end_day", "2024-03-02"},
        {"requests_per_minute", 60}
    };
    const auto response = app.dispatch(request("data.download.update.plan", params));
    EXPECT_TRUE(response.at("data").at("online"));
    EXPECT_FALSE(response.at("research_service").at("online"));
    const auto& plan = response.at("history_update_plan");
    EXPECT_EQ(plan.at("query").at("dataset_id"), params.at("dataset_id"));
    const auto submit = request(
        "data.download.update.submit",
        {{"id", "offline-plan"}, {"query", params}, {"plan_id", plan.at("id")}, {"token", ""}});
    EXPECT_NO_THROW(app.dispatch(submit));
    EXPECT_EQ(source.acquisition.id(), "offline-plan");
    EXPECT_EQ(source.acquisition.authorization(), std::string(64, 'a'));
    EXPECT_EQ(source.captured_authorization.task_instance(), source.data_address().session);
    EXPECT_TRUE(source.submitted.id().empty());
}

TEST(TerminalHistoryUpdate, SlowPlanDoesNotBlockAndObsoleteServiceCannotSubmit)
{
    MinuteService source;
    MinuteService replacement;
    terminal::Application app;
    terminal::ApplicationTestAccess::attach(app, source.address(), source.data_address());
    const Json params = {
        {"dataset_id", std::string(64, 'a')},
        {"daily", nullptr},
        {"mode", "extend"},
        {"end_day", "2024-03-02"},
        {"requests_per_minute", 60}
    };
    auto pending = std::async(std::launch::async, [&] {
        return app.dispatch(request("data.download.update.plan", params));
    });
    EXPECT_TRUE(source.wait(1));
    const auto started = std::chrono::steady_clock::now();
    EXPECT_EQ(app.dispatch(request("runtime.snapshot")).at("protocol"), 1);
    EXPECT_TRUE(app.dispatch(request("market.disconnect")).contains("protocol"));
    EXPECT_LT(std::chrono::steady_clock::now() - started, testing_support::bound(500ms));
    terminal::ApplicationTestAccess::attach(app, replacement.address(), replacement.data_address());
    source.release();
    EXPECT_THROW((void)pending.get(), Error);
    replacement.release();
    const auto plan =
        app.dispatch(request("data.download.update.plan", params)).at("history_update_plan");
    auto submit = request(
        "data.download.update.submit",
        {{"id", "update"}, {"query", params}, {"plan_id", plan.at("id")}, {"token", ""}});
    replacement.reset();
    auto stale = std::async(std::launch::async, [&] {
        return app.dispatch(submit);
    });
    EXPECT_TRUE(replacement.wait(1));
    terminal::ApplicationTestAccess::attach(app, source.address(), source.data_address());
    replacement.release();
    EXPECT_THROW((void)stale.get(), Error);
    EXPECT_TRUE(replacement.acquisition.id().empty());
    submit["params"]["plan_id"] = std::string(64, '0');
    EXPECT_THROW(app.dispatch(submit), std::invalid_argument);
    EXPECT_TRUE(source.submitted.id().empty());
}

TEST(TerminalHistoryUsage, SlowQueryDoesNotBlockAndRejectsStaleOrWrongIdentity)
{
    MinuteService source;
    MinuteService replacement;

    struct Environment
    {
        std::optional<std::string> previous = environment_variable("ASTERION_NODE_DIRECTORY");

        ~Environment()
        {
            if (previous)
                setenv("ASTERION_NODE_DIRECTORY", previous->c_str(), 1);
            else
                unsetenv("ASTERION_NODE_DIRECTORY");
        }
    } restore;

    setenv("ASTERION_NODE_DIRECTORY", source.root.c_str(), 1);
    terminal::Application app;
    terminal::ApplicationTestAccess::attach(app, source.address(), source.data_address());
    const auto query = request("data.history.usage", {{"id", std::string(64, 'a')}});
    auto pending = std::async(std::launch::async, [&] {
        return app.dispatch(query);
    });
    EXPECT_TRUE(source.wait(1));
    const auto started = std::chrono::steady_clock::now();
    EXPECT_EQ(app.dispatch(request("runtime.snapshot")).at("protocol"), 1);
    EXPECT_TRUE(app.dispatch(request("market.disconnect")).contains("protocol"));
    EXPECT_LT(std::chrono::steady_clock::now() - started, testing_support::bound(500ms));
    terminal::ApplicationTestAccess::attach(app, replacement.address(), replacement.data_address());
    source.release();
    EXPECT_THROW((void)pending.get(), Error);
    replacement.release();
    const auto result = app.dispatch(query).at("history_usage");
    EXPECT_EQ(result.at("dataset_id"), std::string(64, 'a'));
    EXPECT_TRUE(result.at("references").empty());
    EXPECT_TRUE(result.at("selected_roles").empty());
    replacement.wrong_identity = true;
    EXPECT_THROW(app.dispatch(query), Error);
}

// A Manager protocol fixture isolates UI behavior from OS registration and Agent ownership.
static std::string manager_fixture_socket(const std::filesystem::path& root)
{
    const auto identity = unique_process_id();
    create_directories_durably(root / "manager");
    replace_file_durably(root / "manager/ipc-id", identity);
    const auto sockets = std::filesystem::path("/tmp") / ("ast-manager-" + identity);
    std::filesystem::create_directories(sockets);
    std::filesystem::permissions(
        sockets,
        std::filesystem::perms::owner_all,
        std::filesystem::perm_options::replace);
    return (sockets / "manager.sock").string();
}

TEST(TerminalNodeCommands, SlowServiceActionDoesNotBlockNodeObservationOrUnrelatedCommands)
{
    MinuteService source;

    struct Environment
    {
        std::optional<std::string> previous = environment_variable("ASTERION_NODE_DIRECTORY");

        ~Environment()
        {
            if (previous)
                setenv("ASTERION_NODE_DIRECTORY", previous->c_str(), 1);
            else
                unsetenv("ASTERION_NODE_DIRECTORY");
        }
    } restore;

    setenv("ASTERION_NODE_DIRECTORY", source.root.c_str(), 1);
    std::promise<void> entered, release;
    const auto released = release.get_future().share();
    std::atomic<unsigned> actions{0};
    std::atomic<bool> busy{false}, connected{true};
    const auto socket = manager_fixture_socket(source.root);
    testing_support::BlockingService host(
        {socket, {}, 0, {}},
        [&](testing_support::Connection& connection, std::stop_token) {
            manager::proto::Request request;
            if (!request.ParseFromString(connection.receive(2s)))
                throw std::runtime_error("invalid fixture request");
            manager::proto::Response response;
            response.set_version(1);
            response.set_correlation_id(request.correlation_id());
            if (request.has_snapshot())
                response.mutable_snapshot();
            else if (request.has_node() && request.node().has_history())
                response.mutable_history();
            else if (request.has_node() && request.node().has_action()) {
                if (busy.exchange(true)) {
                    response.mutable_error()->set_code("conflict");
                    response.mutable_error()->set_message(
                        "another node operation is in progress; retry after it completes");
                } else {
                    if (actions.fetch_add(1) == 0)
                        entered.set_value();
                    if (released.wait_for(testing_support::bound(4s)) != std::future_status::ready)
                        throw std::runtime_error("fixture action was not released");
                    response.mutable_accepted();
                    busy = false;
                }
            } else {
                if (request.node().has_disconnect())
                    connected = false;
                else
                    connected = true;
                response.mutable_accepted();
            }
            auto* view =
                request.has_snapshot() ? response.mutable_snapshot() : response.mutable_view();
            if (connected) {
                auto* node = view->add_nodes();
                node->set_id("fixture");
                node->set_online(true);
                auto* status = node->mutable_health();
                status->set_instance_id(actions.load() ? "restarted-fixture" : "fixture");
                status->set_phase(node::proto::Status::READY);
                status->set_os("macos");
                status->set_arch("arm64");
            }
            connection.send(response.SerializeAsString(), 2s);
        });
    auto running = std::async(std::launch::async, [&] {
        return host.run();
    });

    struct Stop
    {
        std::future<bool>& running;

        ~Stop()
        {
            service::request_stop();
            running.wait();
        }
    } stop{running};

    terminal::Application app;
    auto node = terminal::ApplicationTestAccess::connect_node(app);
    terminal::ApplicationTestAccess::node(app, node);
    (void)app.dispatch(request("runtime.snapshot"));
    auto captured = terminal::ApplicationTestAccess::capture(app);
    const auto action =
        request("node.action", {{"id", "fixture"}, {"service", "task"}, {"action", "restart"}});
    auto pending = std::async(std::launch::async, [&] {
        return app.dispatch(action);
    });
    EXPECT_EQ(entered.get_future().wait_for(testing_support::bound(1s)), std::future_status::ready);
    auto observation = node->history_inventory();
    EXPECT_EQ(observation.wait_for(testing_support::bound(500ms)), std::future_status::ready);
    const auto started = std::chrono::steady_clock::now();
    EXPECT_NO_THROW((void)app.dispatch(request("node.local")));
    EXPECT_NO_THROW((void)app.dispatch(request("market.disconnect")));
    EXPECT_EQ(app.dispatch(request("runtime.snapshot")).at("protocol"), 1);
    EXPECT_LT(std::chrono::steady_clock::now() - started, testing_support::bound(500ms));
    try {
        (void)app.dispatch(action);
        ADD_FAILURE() << "concurrent service mutation accepted";
    } catch (const Error& error) {
        EXPECT_STREQ(
            error.what(),
            "another node operation is in progress; retry after it completes");
    }
    EXPECT_EQ(actions.load(), 1U);
    release.set_value();
    EXPECT_NO_THROW((void)observation.get());
    EXPECT_NO_THROW((void)pending.get());
    EXPECT_NO_THROW((void)app.dispatch(action));
    EXPECT_EQ(actions.load(), 2U);
    EXPECT_EQ(
        app.dispatch(request("node.connect", {{"id", "fixture"}}))
            .at("nodes")
            .at(0)
            .at("health")
            .at("instance_id"),
        "restarted-fixture");
    terminal::ApplicationTestAccess::node(app, {});
    node.reset();
    auto retained = std::async(std::launch::async, std::move(captured));
    EXPECT_EQ(retained.get().at("nodes").at(0).at("health").at("instance_id"), "fixture");
}

TEST(TerminalHistoryUsage, DataAuthorityRetainsOfflineConsumersWithoutResearchOrNodeDiscovery)
{
    MinuteService source;
    source.release();
    source.reference = true;
    terminal::Application app;
    terminal::ApplicationTestAccess::data_only(app, source.data_address());
    const auto query = request("data.history.usage", {{"id", std::string(64, 'a')}});
    const auto result = app.dispatch(query);
    const auto& usage = result.at("history_usage");
    EXPECT_EQ(usage.at("dataset_id"), std::string(64, 'a'));
    EXPECT_EQ(usage.at("state"), "blocked");
    ASSERT_EQ(usage.at("consumers").size(), 2U);
    EXPECT_EQ(usage.at("consumers")[0].at("node"), "offline-consumer-node");
    EXPECT_EQ(usage.at("consumers")[0].at("phase"), "prepared");
    EXPECT_EQ(usage.at("consumers")[1].at("phase"), "confirmed");
    EXPECT_TRUE(result.at("research_service").is_null());
    EXPECT_TRUE(result.at("nodes").empty());
    source.wrong_identity = true;
    EXPECT_THROW(app.dispatch(query), Error);
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
    std::unique_ptr<testing_support::BlockingService> host;
    std::future<bool> running;

    explicit LiveService(bool block)
        : released(!block)
    {
        service::reset_stop_request();
        std::filesystem::create_directory(root);
        host = std::make_unique<testing_support::BlockingService>(
            service::Transport{endpoint, {}, 0, {}},
            [this](testing_support::Connection& connection, std::stop_token stop) {
                while (!stop.stop_requested()) {
                    protocol::proto::Request message;
                    std::string frame;
                    try {
                        // Terminal polls idle live accounts every two seconds. The
                        // fixture must not close at that same deadline before the next
                        // legitimate poll arrives.
                        frame = connection.receive(testing_support::bound(5s));
                    } catch (const Error&) {
                        return;
                    }
                    if (!message.ParseFromString(frame))
                        throw std::runtime_error("invalid fixture request");
                    protocol::proto::Response response;
                    response.set_version(1);
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
                                    throw std::runtime_error("fixture mutation was not released");
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
                        snapshot->set_risk_artifact(std::string(64, 'a'));
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
                    connection.send(response.SerializeAsString(), 2s);
                }
            });
        running = std::async(std::launch::async, [&] {
            return host->run();
        });
    }

    terminal::ServiceEndpoint address() const
    {
        return {{}, "live-fixture", 0, {}, endpoint};
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
             request("data.dataset.clear")
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
    const auto initial = app.dispatch(request("runtime.snapshot"));
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
        terminal::ApplicationTestAccess::verification(app, {{"source", "tushare.ft_mins"}});
        EXPECT_EQ(old.wait_for(0s), std::future_status::timeout);
    }
    old.get();
    ASSERT_FALSE(failure);
    const auto delayed = parse_json(*encoded).at("result");
    const auto current = app.dispatch(request("runtime.snapshot", {{"since", std::uint64_t{0}}}));
    EXPECT_EQ(delayed.at("credential_verification"), initial.at("credential_verification"));
    EXPECT_EQ(current.at("credential_verification").at("source"), "tushare.ft_mins");
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

TEST(TerminalTaskPages, NativeCommandSelectsHistoryWithoutChangingServiceIdentity)
{
    MinuteService source;
    source.release();
    std::function<Json()> held;
    Json expected;
    {
        terminal::Application app;
        terminal::ApplicationTestAccess::attach(app, source.address(), source.data_address());
        const auto before = app.dispatch(request("runtime.snapshot")).at("research_service");
        const Json owner{
            {"kind", "research"},
            {"node_id", before.at("node_id")},
            {"service", before.at("service")},
            {"connection_id", before.at("connection_id")}
        };
        const auto page =
            app.dispatch(request("task.page", {{"owner", owner}, {"before_sequence", 42}}))
                .at("research_service");
        EXPECT_EQ(page.at("connection_id"), before.at("connection_id"));
        EXPECT_EQ(page.at("before_sequence"), 42);
        held = terminal::ApplicationTestAccess::capture(app);
        expected = page;
        {
            std::lock_guard lock(source.mutex);
            EXPECT_EQ(source.requested_before_sequence, 42);
        }
        const auto recent =
            app.dispatch(request("task.page", {{"owner", owner}, {"before_sequence", 0}}))
                .at("research_service");
        EXPECT_EQ(recent.at("before_sequence"), 0);
        EXPECT_THROW(
            app.dispatch(request("task.page", {{"owner", owner}, {"before_sequence", -1}})),
            std::invalid_argument);
    }
    // An in-flight renderer owns its selected page even after pagination and client release.
    EXPECT_EQ(std::async(std::launch::async, held).get().at("research_service"), expected);
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
        return {"", service, 0, {}, address.endpoint};
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
        terminal::ServiceEndpoint{"", "fixture", 0, {}, address.endpoint})
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

TEST(TerminalServiceClients, SlowResourceCatalogueLeavesDataCancellationAndObservationAvailable)
{
    MinuteService service;
    terminal::ServiceIo io;
    auto data = terminal::DataClient::open(io, service.data_address()).get();
    resource::proto::CatalogueQuery query;
    query.set_kind("minute_version");
    auto catalogue = data->resources(query);
    ASSERT_TRUE(service.wait(1));
    auto cancel = data->action("running-task", "cancel");
    const auto ready = cancel.wait_for(testing_support::bound(500ms));
    auto observation = data->page(0);
    const auto observed = observation.wait_for(testing_support::bound(500ms));
    service.release();
    EXPECT_EQ(ready, std::future_status::ready);
    EXPECT_NO_THROW(cancel.get());
    EXPECT_EQ(observed, std::future_status::ready);
    EXPECT_NO_THROW(observation.get());
    EXPECT_TRUE(catalogue.get().at("items").empty());
    std::lock_guard lock(service.mutex);
    EXPECT_EQ(service.data_cancels, 1);
}
