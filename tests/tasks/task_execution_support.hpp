#pragma once
// Inputs, fixtures and helpers shared by the task execution tests.

#include "strategy_store.hpp"
#include "research_submission.hpp"
#include "data_submission.hpp"
#include <asterion/research.pb.h>
#include "support/task_wait.hpp"
#include <asterion/protocol/data_client.hpp>
#include "tasks/paper_input.hpp"
#include <asterion/futures/performance.hpp>
#include "portfolio_budget.hpp"
#include "account_reconciliation.hpp"
#include "support/local_listener.hpp"
#include "support/data_discovery.hpp"
#include <asterion/protocol/factor.hpp>
#include "history_update.hpp"
#include <asterion/database/sqlite/sqlite_database.hpp>
#include "task_events.hpp"
#include <asterion/system/durable_file.hpp>
#include "engine.hpp"
#include "factor_engine.hpp"
#include <asterion/futures/order_limits.hpp>
#include "data/bar_fixture.hpp"
#include "data/shfe_margin_fixture.hpp"
#include "historical_margins.hpp"
#include "data/history_fixture.hpp"
#include "bar_dataset_source.hpp"
#include "task_store.hpp"
#include "research_host.hpp"
#include "research_backup.hpp"
#include "research_execution.hpp"
#include "program_files.hpp"
#include "research_data.hpp"
#include "research_configuration.hpp"
#include "tasks/task_store_support.hpp"
#include "support/timing.hpp"
#include "verification_slots.hpp"
#include <asterion/protocol/task_execution.hpp>
#include <asterion/protocol/research_variable.hpp>
#include <asterion/protocol/research_strategy.hpp>
#include <asterion/protocol/research_logic.hpp>
#include <asterion/protocol/resource.hpp>
#include <asterion/system/process/artifact.hpp>
#include <asterion/system/process/child.hpp>
#include <fstream>
#include <cstdlib>
#include <future>
#include <gtest/gtest.h>
#include <map>
using namespace asterion;
#include <asterion/system/ipc/local_channel.hpp>
#include <thread>
#include <array>
#include "history_coverage.hpp"

namespace {

inline Decimal d(const char* text)
{
    return Decimal::parse(text);
}

// Compares the input's strategies as a request for them would: before a
// holdout from `from`, or by rolling when training days are given.
inline void compare(
    backtest::proto::BacktestInput& input,
    const std::string& from,
    unsigned training = 0,
    unsigned validation = 0)
{
    backtest::proto::BacktestRequest request;
    *request.mutable_strategies() = input.strategies();
    // This comparison fixture explicitly chooses its numerical scoring assumptions.
    request.mutable_performance_assumptions()->set_annual_periods(252);
    request.mutable_performance_assumptions()->set_risk_free(
        backtest::proto::PerformanceAssumptions::EXPLICIT_ZERO);
    request.set_holdout_from(from);
    if (training) {
        request.mutable_walk_forward()->set_training_days(training);
        request.mutable_walk_forward()->set_validation_days(validation);
    }
    protocol::set_backtest_strategies(input, request);
}

inline backtest::proto::BacktestInput input(
    const std::vector<int>& prices = {100, 101, 102, 101, 100, 101, 103},
    const char* settlement = "103")
{
    std::vector<MarketBar> bars;
    std::int64_t time = 1790298000000000000LL;
    for (const auto price : prices) {
        bars.push_back(test::flat("2026-09-25", time, std::to_string(price).c_str(), "10"));
        time += 1000000000;
    }
    return testing_support::backtest_input(test::dataset(bars, {{"2026-09-25", d(settlement)}}));
}

// Thirty trading days of one bar each, rising two a day with a dip every
// third. A bar opens at the close before it and trades one below, so a limit
// at that close fills. Two moving averages of 1 and 2 are compared on the 25
// days before 2026-10-20: one may only be short, the other only long.
inline backtest::proto::BacktestInput comparison()
{
    using namespace std::chrono;
    std::vector<MarketBar> bars;
    std::vector<DaySettlement> settlements;
    int previous = 100;
    for (int i = 0; i < 30; ++i) {
        const int close = 100 + 2 * i - (i % 3 == 2 ? 3 : 0);
        const auto day =
            format_trading_date(year_month_day{sys_days{year{2026} / September / 25} + days{i}});
        const auto text = [](int value) {
            return std::to_string(value);
        };
        bars.push_back(
            test::bar(
                day,
                1790298000000000000LL + i * 86400LL * 1000000000,
                text(previous).c_str(),
                text(std::max(previous, close)).c_str(),
                text(std::min(previous, close) - 1).c_str(),
                text(close).c_str(),
                "10"));
        settlements.push_back({day, d(text(close).c_str())});
        previous = close;
    }
    auto result = testing_support::backtest_input(test::dataset(bars, settlements));
    *result.mutable_strategies(0) = testing_support::moving_average(1, 2, "1", "short");
    *result.add_strategies() = testing_support::moving_average(1, 2, "1", "long");
    compare(result, "2026-10-20");
    return result;
}

// Sixty such days: the first thirty rise as above, the last thirty fall two a
// day with a bump every third. The same two averages are compared by rolling,
// twenty days of training and ten of validation: four rounds, the
// last of which trains on falling days alone.
inline backtest::proto::BacktestInput rolling_comparison()
{
    using namespace std::chrono;
    std::vector<MarketBar> bars;
    std::vector<DaySettlement> settlements;
    int previous = 100;
    for (int i = 0; i < 60; ++i) {
        const int close =
            i < 30 ? 100 + 2 * i - (i % 3 == 2 ? 3 : 0) : 158 - 2 * (i - 29) + (i % 3 == 2 ? 3 : 0);
        const auto day =
            format_trading_date(year_month_day{sys_days{year{2026} / September / 25} + days{i}});
        const auto text = [](int value) {
            return std::to_string(value);
        };
        bars.push_back(
            test::bar(
                day,
                1790298000000000000LL + i * 86400LL * 1000000000,
                text(previous).c_str(),
                text(std::max(previous, close)).c_str(),
                text(std::min(previous, close) - 1).c_str(),
                text(close).c_str(),
                "10"));
        settlements.push_back({day, d(text(close).c_str())});
        previous = close;
    }
    auto result = testing_support::backtest_input(test::dataset(bars, settlements));
    *result.mutable_strategies(0) = testing_support::moving_average(1, 2, "1", "short");
    *result.add_strategies() = testing_support::moving_average(1, 2, "1", "long");
    compare(result, "", 20, 10);
    return result;
}

// The bars of a factor input's one series, which is added when there is none yet.
inline data::proto::BarDataset* factor_bars(factor::proto::FactorInput& input)
{
    if (input.series().empty())
        input.add_series();
    return input.mutable_series(0)->mutable_bars();
}

inline factor::proto::FactorInput maximum_factor_input()
{
    factor::proto::FactorInput spec;
    spec.set_factor(protocol::proto::PRICE_MOMENTUM);
    spec.set_full_sample(true);
    spec.add_lookbacks(2);
    spec.set_horizon(1);
    auto* dataset = factor_bars(spec);
    *dataset = input().paper().contracts(0).dataset();
    const auto first = dataset->bars(0);
    dataset->clear_bars();
    for (std::size_t i = 0; i < protocol::max_dataset_bars; ++i) {
        auto* bar = dataset->add_bars();
        *bar = first;
        bar->set_timestamp_ns(first.timestamp_ns() + static_cast<std::int64_t>(i) * 1000000);
    }
    dataset->set_revision(protocol::bar_dataset_revision(*dataset));
    spec.set_dataset_revision(dataset->revision());
    return spec;
}

} // namespace

namespace {

struct TaskDirectory
{
    std::filesystem::path path = std::filesystem::temp_directory_path()
        / ("asterion-backtest-factor-" + unique_process_id());

    TaskDirectory()
    {
        std::filesystem::create_directory(path);
    }

    ~TaskDirectory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

} // namespace

namespace {

using namespace std::chrono_literals;
namespace task_wire = task::proto;

struct TaskProcess : testing::Test
{
    TaskDirectory directory, warehouse;
    const std::string fixture_program_artifact = sha256_file(ASTERION_RESEARCH_PATH);
    std::unique_ptr<ChildProcess> service, data_service;
    std::unique_ptr<testing_support::DataDiscovery> discovery;
    std::filesystem::path sockets;
    std::string endpoint;

    void SetUp() override
    {
        sockets = std::filesystem::path("/tmp") / ("ast-r-" + unique_process_id().substr(0, 12));
        std::filesystem::create_directory(sockets);
        std::filesystem::permissions(sockets, std::filesystem::perms::owner_all);
        endpoint = (sockets / "task.sock").string();
        replace_file_durably(sockets / "research.json", R"({"task_concurrency":2})");
        discovery = std::make_unique<testing_support::DataDiscovery>(
            endpoint + ".discovery",
            "historical-data",
            endpoint + ".data");
        start();
    }

    void TearDown() override
    {
        service.reset();
        data_service.reset();
        discovery.reset();
        if (!sockets.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(sockets, ec);
        }
    }

    task_wire::TaskResponse call(
        research::proto::ResearchRequest request,
        const std::string& target = "",
        std::chrono::milliseconds timeout = 2s)
    {
        if (request.has_submit() && !request.submit().has_data_owner()
            && !request.submit().has_statistics() && !request.submit().has_logic_trial())
            request.mutable_submit()->mutable_data_owner()->set_service_id("historical-data");
        request.set_version(1);
        request.set_service_id("task");
        request.set_correlation_id(unique_process_id());
        auto channel =
            ipc::Channel::connect(target.empty() ? endpoint : target, testing_support::bound(2s));
        channel.send(request.SerializeAsString(), testing_support::bound(2s));
        task_wire::TaskResponse response;
        if (!response.ParseFromString(channel.receive(testing_support::bound(timeout))))
            throw std::runtime_error("bad task response");
        protocol::validate_message(response);
        if (response.version() != 1 || response.service_id() != request.service_id()
            || response.correlation_id() != request.correlation_id())
            throw std::runtime_error("wrong task response identity");
        if (response.has_error())
            throw std::runtime_error(response.error().message());
        return response;
    }

    void start_data()
    {
        data_service = std::make_unique<ChildProcess>(
            ASTERION_DATA_SERVICE_PATH,
            std::vector<std::string>{
                "--directory",
                warehouse.path.string(),
                "--session",
                "historical-data",
                "--node-id",
                "fixture-node",
                "--endpoint",
                endpoint + ".data",
                "--internal-endpoint",
                endpoint + ".data-workers",
                "--file-workers",
                "1"
            });
        protocol::DataClient client(endpoint + ".data", "historical-data");
        const auto until = std::chrono::steady_clock::now() + 10s;
        for (;;) {
            if (std::chrono::steady_clock::now() >= until)
                throw std::runtime_error("data fixture did not initialize");
            try {
                data::proto::DataRequest request;
                request.mutable_heartbeat();
                if (client.call(request, 1s).health().initialized())
                    return;
            } catch (const Error&) {
                if (data_service->exited() || std::chrono::steady_clock::now() >= until)
                    throw;
            }
            std::this_thread::sleep_for(20ms);
        }
    }

    void launch(bool publish_process = true, const std::filesystem::path& storage_root = {})
    {
        std::filesystem::remove(endpoint); // This test exclusively owns the private directory.
        std::filesystem::remove(endpoint + ".health");
        const auto directory_utf8 =
            (storage_root.empty() ? directory.path : storage_root).u8string();
        service = std::make_unique<ChildProcess>(
            ASTERION_RESEARCH_PATH,
            std::vector<std::string>{
                "--config",
                (sockets / "research.json").string(),
                "--directory",
                std::string(directory_utf8.begin(), directory_utf8.end()),
                "--endpoint",
                endpoint,
                "--health-endpoint",
                endpoint + ".health",
                "--session",
                "task",
                "--node-id",
                "fixture-node",
                "--discovery-endpoint",
                endpoint + ".discovery",
                "--file-workers",
                "1"
            });
        if (publish_process)
            discovery->research("task", service->id(), fixture_program_artifact);
    }

    void start(std::chrono::milliseconds timeout = 10s)
    {
        launch();
        wait_for_initialization(timeout);
    }

    void wait_for_initialization(std::chrono::milliseconds timeout = 10s)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            try {
                research::proto::ResearchRequest req;
                req.mutable_heartbeat();
                if (call(req).health().initialized())
                    break;
            } catch (const std::exception&) {
                if (service->exited())
                    throw;
            }
            if (std::chrono::steady_clock::now() > deadline)
                throw std::runtime_error("task fixture did not initialize");
            std::this_thread::sleep_for(20ms);
        }
    }

    std::optional<backtest::proto::BacktestInput> fixed_input;

    data::proto::BarDataset published_dataset(const std::vector<int>& prices, const std::string& id)
    {
        data_service.reset();
        std::vector<std::string> days;
        const auto first = std::chrono::sys_days(std::chrono::year(2024) / 1 / 1);
        for (std::size_t day = 0; day < (prices.size() + 239) / 240; ++day)
            days.push_back(
                format_trading_date(std::chrono::year_month_day(first + std::chrono::days(day))));
        const auto selection = test::seed_history(warehouse.path, prices, id, days);
        data::proto::BarDatasetRequest request;
        for (const auto& version : selection.at("source_dataset_ids"))
            request.add_source_dataset_ids(version.get<std::string>());
        *request.mutable_daily() = protocol::encode_daily_table(selection.at("daily"));
        request.set_contract("SHFE/rb/2026-10");
        data::proto::BarDataset dataset;
        {
            data::Store store(warehouse.path, "historical-data");
            dataset = data::resolve_bar_dataset(store.tables(), store.sources(request));
        }
        start_data();
        return dataset;
    }

    const backtest::proto::BacktestInput& process_input()
    {
        if (!fixed_input) {
            fixed_input = input();
            *fixed_input->mutable_paper()->mutable_contracts(0)->mutable_dataset() =
                published_dataset({100, 101, 102, 101, 100, 101, 103}, "worker-input");
            fixed_input->set_dataset_revision(protocol::dataset_revision(fixed_input->paper()));
        }
        return *fixed_input;
    }

    std::string preserved_program;

    void preserve_program()
    {
        if (preserved_program.empty()) {
            resource::proto::Owner owner;
            owner.set_node_id("fixture-node");
            owner.set_service_id("task");
            owner.set_domain("research");
            preserved_program = programs::Files(directory.path / "programs", owner)
                                    .preserve(ASTERION_RESEARCH_PATH, fixture_program_artifact)
                                    .digest;
        }
    }

    template <class Input>
    task::proto::Task persist(tasks::Store& store, const std::string& id, Input input)
    {
        preserve_program();
        auto submission = task_submission(store, id, std::move(input));
        asterion::tasks::set_program_artifact(submission, preserved_program);
        return tasks::submit(store, std::move(submission));
    }

    void submit(const std::string& id)
    {
        service.reset();
        {
            tasks::Store store(
                asterion::research::task_domain(),
                directory.path,
                tasks::Identity{"task"});
            persist(store, id, process_input());
        }
        start();
    }
};

} // namespace

namespace {

struct TaskClock final : Clock
{
    Nanoseconds value = 1790298000000000000LL;

    Nanoseconds utc_now() const override
    {
        return value;
    }

    Nanoseconds monotonic_now() const override
    {
        return 0;
    }
};

} // namespace

namespace {

inline backtest::proto::BacktestInput multiday_input()
{
    auto spec = input();
    spec.mutable_paper()->mutable_contracts(0)->mutable_dataset()->clear_bars();
    const std::int64_t first = 1790298000000000000LL;
    const std::int64_t next = first + 3LL * 86400 * 1000000000;
    int index = 0;
    for (const auto price : {100, 101, 102, 101, 104, 103, 102, 103}) {
        auto* tick = spec.mutable_paper()->mutable_contracts(0)->mutable_dataset()->add_bars();
        tick->set_trading_day(index < 4 ? "2026-09-25" : "2026-09-28");
        tick->set_timestamp_ns((index < 4 ? first : next) + (index % 4) * 1000000000LL);
        for (auto* p :
            {tick->mutable_open(),
                tick->mutable_high(),
                tick->mutable_low(),
                tick->mutable_close()})
            p->set_units(d(std::to_string(price).c_str()).raw());
        tick->mutable_volume()->set_units(d("10").raw());
        ++index;
    }
    spec.mutable_paper()
        ->mutable_contracts(0)
        ->mutable_dataset()
        ->mutable_days(0)
        ->mutable_settlement_price()
        ->set_units(d("105").raw());
    auto* day = spec.mutable_paper()->mutable_contracts(0)->mutable_dataset()->add_days();
    *day = spec.paper().contracts(0).dataset().days(0);
    day->set_trading_day("2026-09-28");
    day->mutable_settlement_price()->set_units(d("110").raw());
    spec.mutable_paper()->mutable_contracts(0)->mutable_dataset()->set_revision(
        protocol::bar_dataset_revision(spec.paper().contracts(0).dataset()));
    spec.set_dataset_revision(spec.paper().contracts(0).dataset().revision());
    return spec;
}

} // namespace

namespace {

// The days a dataset settles on without bars: the contract did not trade on
// them, or their bars are missing.
inline std::vector<std::string> days_without_bars(const data::proto::BarDataset& dataset)
{
    std::set<std::string> traded;
    for (const auto& bar : dataset.bars())
        traded.insert(bar.trading_day());
    std::vector<std::string> result;
    for (const auto& day : dataset.days())
        if (!traded.contains(day.trading_day()))
            result.push_back(day.trading_day());
    return result;
}

} // namespace

namespace {

inline data::proto::BarDatasetRequest composed_request(const std::vector<Json>& selections)
{
    std::set<std::string> ids;
    for (const auto& selected : selections)
        for (const auto& id : selected.at("source_dataset_ids"))
            ids.insert(id.get<std::string>());
    return protocol::encode_bar_dataset_request(
        {{"source_dataset_ids", ids},
            {"daily", test::latest_daily(selections)},
            {"begin_day", ""},
            {"end_day", ""},
            {"contract", "SHFE/rb/2026-10"}});
}

} // namespace

namespace {

inline data::proto::DownloadAuthorization refresh_authorization(const std::string& id)
{
    data::proto::DownloadAuthorization authorization;
    authorization.set_id(std::string(64, 'a'));
    authorization.set_provider_artifact(std::string(64, 'b'));
    authorization.set_data_instance("historical-data");
    authorization.set_task_instance("historical-data");
    authorization.set_task_id(id);
    *authorization.mutable_table_update() = protocol::encode_table_update(
        {{"provider", "test.independent.c"},
            {"tables", {"futures.daily"}},
            {"begin_day", "2024-03-01"},
            {"end_day", "2024-03-01"},
            {"requests_per_minute", 60},
            {"refresh",
                {{"partition",
                     {{"table", "futures.daily"}, {"exchange", "SHFE"}, {"period", "2024-03-01"}}},
                    {"source", "fixture.daily.v1"},
                    {"previous_snapshot", std::string(64, 'c')},
                    {"previous_content", std::string(64, 'd')},
                    {"provider_artifact", std::string(64, 'b')}}}});
    return authorization;
}

} // namespace

namespace {

inline backtest::proto::BacktestInput settlement_margin_input()
{
    std::vector<MarketBar> bars;
    for (int i = 0; i < 4; ++i) {
        const auto day = "2026-09-" + std::to_string(25 + i);
        const auto open = std::to_string(99 + i), close = std::to_string(100 + i);
        bars.push_back(
            test::bar(
                day,
                trading_day_close_ns(day),
                open.c_str(),
                close.c_str(),
                open.c_str(),
                close.c_str(),
                "100"));
    }
    auto input = testing_support::backtest_input(test::dataset(bars, {}, test::contract(), 0));
    *input.mutable_strategies(0) = testing_support::moving_average(1, 2);
    auto* costs = input.mutable_paper()->mutable_contracts(0)->mutable_cost_schedule();
    costs->mutable_versions(0)->mutable_values()->mutable_margin_per_lot()->set_units(0);
    costs->mutable_versions(0)->mutable_values()->mutable_margin_rate()->set_units(d("0.14").raw());
    const auto next = costs->versions(0);
    *costs->add_versions() = next;
    costs->mutable_versions(1)->set_effective_from("2026-09-28");
    costs->mutable_versions(1)->mutable_values()->mutable_margin_rate()->set_units(d("0.17").raw());
    for (const auto& [day, rate] : std::vector<std::pair<std::string, const char*>>{
             {"2026-09-27", "0.16"},
             {"2026-09-28", "0.18"}
         }) {
        auto* event = input.add_settlement_margins();
        event->set_trading_day(day);
        event->mutable_rate()->set_units(d(rate).raw());
        event->set_source("Test-only explicit model assumption");
    }
    return input;
}

} // namespace

namespace {

inline resource::proto::Object prepared_resource(const task::proto::PreparedTaskSubmission& value)
{
    resource::proto::Object resource;
    resource.mutable_owner()->set_domain("research");
    resource.mutable_owner()->set_node_id("fixture-node");
    resource.mutable_owner()->set_service_id(value.instance_id());
    resource.set_kind("backtest");
    resource.set_id(value.record().task().id());
    resource.set_version(value.record().input_digest());
    return resource;
}

} // namespace

namespace {

inline backtest::proto::BacktestInput fixed_weight_input()
{
    const std::vector<std::string>
        days{"2026-09-21", "2026-09-22", "2026-09-23", "2026-09-24", "2026-09-25", "2026-09-28"};
    std::vector<MarketBar> bars;
    for (std::size_t i = 0; i < days.size(); ++i)
        bars.push_back(
            test::flat(
                days[i],
                1'790'000'000'000'000'000LL + static_cast<std::int64_t>(i) * 86'400'000'000'000LL,
                "1000",
                "100"));
    auto spec = testing_support::backtest_input(test::dataset(bars, {}, test::contract(), 0));
    spec.mutable_paper()->mutable_deposit()->set_units(d("100000").raw());
    auto* second = spec.mutable_paper()->add_contracts();
    *second = spec.paper().contracts(0);
    *second->mutable_dataset()->mutable_contract() =
        test::contract("SHFE", "hc2610", "hc", "2026-10");
    second->mutable_dataset()->set_revision(protocol::bar_dataset_revision(second->dataset()));
    spec.set_dataset_revision(protocol::dataset_revision(spec.paper()));
    const Json members = Json::array(
        {Json{{"venue", "SHFE"}, {"symbol", "rb2610"}, {"weight", "0.4"}},
            Json{{"venue", "SHFE"}, {"symbol", "hc2610"}, {"weight", "-0.2"}}});
    const Json rule{
        {"kind", "fixed_weights"},
        {"rebalance", 2},
        {"gross_weight_limit", "0.8"},
        {"net_weight_limit", "0.3"},
        {"members", members}
    };
    *spec.mutable_strategies(0) =
        protocol::encode_strategy(Json{{"sides", "both"}, {"rule", rule}});
    return spec;
}

} // namespace
