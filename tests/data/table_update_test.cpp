#include "data/history_fixture.hpp"
#include "table_update.hpp"
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/protocol/data.hpp>
#include <asterion/protocol/data_client.hpp>
#include <asterion/protocol/task.hpp>
#include <asterion/v1/task.pb.h>
#include <fstream>
#include <gtest/gtest.h>
#include <thread>
#include <cstdlib>
#include <optional>
#include <future>
using namespace asterion;
using namespace std::chrono_literals;

namespace {

struct Folder
{
    std::filesystem::path path;

    explicit Folder(const char* prefix)
        : path(std::filesystem::path("/tmp") / (prefix + unique_process_id().substr(0, 12)))
    {
        std::filesystem::create_directory(path);
        std::filesystem::permissions(path, std::filesystem::perms::owner_all);
    }

    ~Folder()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

std::string contents(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}

// The data and task services of one node, with the fixture plugin as the only
// data source: an exchange with one contract and a week holding every outcome.
struct Node
{
    Folder warehouse{"ast-tw-"}, ledger{"ast-tl-"}, sockets{"ast-ts-"};
    const std::string endpoint = (sockets.path / "task.sock").string(),
                      workers = (sockets.path / "worker.sock").string(),
                      data_endpoint = (sockets.path / "data.sock").string(),
                      data_workers = (sockets.path / "data.workers").string();
    ChildProcess data_service{
        ASTERION_DATA_SERVICE_PATH,
        {"--directory",
            warehouse.path.string(),
            "--session",
            "fixture-data",
            "--task-instance",
            "table-test",
            "--endpoint",
            data_endpoint,
            "--worker-endpoint",
            data_workers,
            "--plugin-directory",
            PLUGIN_FIXTURE_DIRECTORY}
    };
    protocol::DataClient data{data_endpoint, "fixture-data"};
    ChildProcess task_service{
        ASTERION_TASK_SERVICE_PATH,
        {"--directory",
            ledger.path.string(),
            "--endpoint",
            endpoint,
            "--worker-endpoint",
            workers,
            "--session",
            "table-test",
            "--data-instance",
            "fixture-data",
            "--data-endpoint",
            data_workers}
    };

    task::v1::TaskResponse call(task::v1::TaskRequest request, bool worker = false)
    {
        request.set_version(1);
        request.set_service_id("table-test");
        request.set_correlation_id(unique_process_id());
        auto channel = ipc::Channel::connect(worker ? workers : endpoint, 2s);
        channel.send(request.SerializeAsString(), 2s);
        task::v1::TaskResponse reply;
        if (!reply.ParseFromString(channel.receive(5s)))
            throw std::runtime_error("bad response");
        return reply;
    }

    Node()
    {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        for (;;) {
            try {
                task::v1::TaskRequest ping;
                ping.mutable_heartbeat();
                data::v1::DataRequest data_ping;
                data_ping.mutable_heartbeat();
                if (call(ping, true).health().initialized() && call(ping).health().initialized()
                    && data.call(data_ping).health().initialized())
                    break;
            } catch (const std::exception&) {
                if (std::chrono::steady_clock::now() > deadline)
                    throw;
            }
            std::this_thread::sleep_for(20ms);
        }
        // The account's share of requests, as a user applies it before downloading.
        for (const auto* credential : {"fixture", "refused", "fixture-table-session-secret"}) {
            data::v1::DataRequest budget;
            budget.mutable_configure_download_budget()->set_source("fixture.minutes");
            budget.mutable_configure_download_budget()->set_credential(credential);
            budget.mutable_configure_download_budget()->set_requests_per_minute(60);
            (void)data.call(budget);
        }
    }

    void submit_definition(
        const std::string& id,
        const data::v1::TableUpdate& definition,
        const std::string& credential = "fixture-table-session-secret")
    {
        data::v1::DataRequest authorize;
        auto* request = authorize.mutable_authorize_download();
        request->set_task_instance("table-test");
        request->set_task_id(id);
        request->set_credential(credential);
        *request->mutable_table_update() = definition;
        task::v1::TaskRequest submit;
        submit.mutable_submit()->set_id(id);
        submit.mutable_submit()->set_download_authorization(
            data.call(authorize).download_authorization().id());
        const auto submitted = call(submit);
        EXPECT_TRUE(submitted.has_task()) << submitted.DebugString();
        EXPECT_EQ(submitted.task().kind(), task::v1::TABLE_UPDATE);
        EXPECT_EQ(submitted.task().total(), 100U);
        EXPECT_EQ(submitted.SerializeAsString().find(credential), std::string::npos);
    }

    task::v1::Task update(
        const std::string& id,
        const std::string& begin,
        const std::string& credential = "fixture")
    {
        submit_definition(
            id,
            protocol::encode_table_update(
                {{"provider", "test.independent.c"},
                    {"tables", {"futures.daily"}},
                    {"begin_day", begin},
                    {"end_day", "2024-03-07"},
                    {"requests_per_minute", 500}}),
            credential);
        return run(id);
    }

    task::v1::Task run(
        const std::string& id,
        const std::filesystem::path& cancel_control = {},
        unsigned launch_slots = 1)
    {
        task::v1::TaskRequest dispatch;
        dispatch.mutable_dispatch()->set_launch_slots(launch_slots);
        auto launches = call(dispatch, true);
        for (const auto deadline = std::chrono::steady_clock::now() + 10s;
            launches.launches().launches().empty() && std::chrono::steady_clock::now() < deadline;
            launches = call(dispatch, true))
            std::this_thread::sleep_for(20ms);
        EXPECT_EQ(launches.launches().launches_size(), 1) << launches.DebugString();
        EXPECT_TRUE(launches.launches().launches(0).table_update());
        EXPECT_EQ(launches.launches().launches(0).provider_artifact().size(), 64U);
        const auto log = ledger.path / (id + "-" + unique_process_id() + ".log");
        ChildProcess worker(
            ASTERION_DATA_PIPELINE_PATH,
            {"--endpoint",
                workers,
                "--session",
                "table-test",
                "--task",
                id,
                "--table-update",
                "--plugin-directory",
                PLUGIN_FIXTURE_DIRECTORY},
            false,
            log,
            true);
        if (!cancel_control.empty()) {
            const auto deadline = std::chrono::steady_clock::now() + 10s;
            while (contents(cancel_control) != "waiting"
                && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(10ms);
            EXPECT_EQ(contents(cancel_control), "waiting");
            task::v1::TaskRequest cancel;
            cancel.mutable_cancel()->set_id(id);
            EXPECT_TRUE(call(cancel).has_task());
        }
        EXPECT_TRUE(worker.wait(20s)) << contents(log);
        task::v1::TaskRequest get;
        get.mutable_get()->set_id(id);
        return call(get).task();
    }

    data::v1::TableUpdateResult result(const std::string& id)
    {
        task::v1::TaskRequest request;
        request.mutable_result()->set_id(id);
        const auto reply = call(request);
        EXPECT_TRUE(reply.has_table_update()) << reply.DebugString();
        EXPECT_EQ(protocol::decode_task_result(reply, id).at("kind"), "table_update");
        return reply.table_update();
    }

    data::v1::TablePage page()
    {
        data::v1::DataRequest request;
        *request.mutable_table_page() = protocol::encode_table_page_query(
            {{"table", "futures.daily"},
                {"source", "fixture.daily.v1"},
                {"exchange", "SHFE"},
                {"period", "2024-03-01"},
                {"offset", 0},
                {"limit", 1}});
        return data.call(request).table_page();
    }

    data::v1::TableRefreshPlan preview()
    {
        data::v1::DataRequest request;
        auto* query = request.mutable_table_refresh_plan();
        query->set_provider("test.independent.c");
        query->set_source("fixture.daily.v1");
        query->set_previous_snapshot(page().revision().snapshot());
        query->set_requests_per_minute(60);
        auto* key = query->mutable_partition();
        key->set_table("futures.daily");
        key->set_exchange("SHFE");
        key->set_period("2024-03-01");
        return data.call(request).table_refresh_plan();
    }

    std::string frozen(const std::string& snapshot)
    {
        data::v1::DataRequest request;
        auto* query = request.mutable_table_series();
        query->set_snapshot(snapshot);
        query->set_daily_source("fixture.daily.v1");
        query->set_calendar_source("fixture.calendar.v1");
        query->set_begin_day("2024-03-01");
        query->set_end_day("2024-03-01");
        query->add_instruments("SHFE/cu/2024-03");
        return data.call(request).table_series().SerializeAsString();
    }

    data::v1::TableCoverage coverage()
    {
        data::v1::DataRequest request;
        request.mutable_table_coverage();
        return data.call(request).table_coverage();
    }
};

} // namespace

TEST(TableUpdates, AnUpdateStoresWhatItCanAndSaysWhatItCouldNot)
{
    Node node;
    // The source has no calendar for the year before the exchange had a contract.
    const auto first = node.update("tables", "2023-06-01");
    ASSERT_EQ(first.state(), task::v1::SUCCEEDED) << first.error();
    EXPECT_EQ(first.completed(), 100U);
    const auto done = node.result("tables");
    // The contract list, the year of the calendar, and the three ordinary days
    // from the first listing on: nothing is asked about the time before it.
    EXPECT_EQ(done.stored(), 5U);
    EXPECT_EQ(done.unchanged(), 0U);
    ASSERT_EQ(done.rejected_size(), 2);
    EXPECT_EQ(done.rejected(0).partition().period(), "2024-03-05");
    EXPECT_EQ(
        done.rejected(0).reason(),
        "futures.daily SHFE 2024-03-05: row 1 \"SHFE/cu/2024-03\" column volume: " "\"1200.5\" has more than 0 decimal places");
    EXPECT_EQ(done.rejected(1).partition().period(), "2024-03-06");
    EXPECT_EQ(
        done.rejected(1).reason(),
        "futures.daily SHFE 2024-03-06: vendor/cu2499 is not in the contract list");
    EXPECT_EQ(done.unpublished(), 1U);
    ASSERT_EQ(done.unpublished_examples_size(), 1);
    EXPECT_EQ(done.unpublished_examples(0).period(), "2024-03-07");
    EXPECT_EQ(done.snapshot().size(), 64U);

    auto held = node.coverage();
    EXPECT_EQ(held.snapshot(), done.snapshot());
    ASSERT_EQ(held.sources_size(), 3);
    const auto daily =
        std::ranges::find(held.rows(), "futures.daily", &data::v1::TableCoverageRow::table);
    ASSERT_NE(daily, held.rows().end());
    EXPECT_EQ(daily->exchange(), "SHFE");
    EXPECT_EQ(daily->source(), "fixture.daily.v1");
    EXPECT_EQ(daily->partitions(), 3U);
    EXPECT_EQ(daily->first_period(), "2024-02-29");
    EXPECT_EQ(daily->last_period(), "2024-03-04");
    ASSERT_TRUE(daily->has_missing_days());
    EXPECT_EQ(daily->missing_days(), 0U) << "the weekend between is not a gap";

    data::v1::DataRequest page;
    *page.mutable_table_page() = protocol::encode_table_page_query(
        {{"table", "futures.daily"},
            {"source", "fixture.daily.v1"},
            {"exchange", "SHFE"},
            {"period", "2024-03-01"},
            {"offset", 0},
            {"limit", 100}});
    const auto rows = protocol::decode_table_page(node.data.call(page).table_page());
    ASSERT_EQ(rows.at("rows").size(), 1U);
    EXPECT_EQ(rows.at("columns").at(9), "amount");
    EXPECT_EQ(rows.at("rows").at(0).at(0), "SHFE/cu/2024-03");
    EXPECT_EQ(rows.at("rows").at(0).at(9), "432000000000.25") << "beyond an eight-place int64";
    EXPECT_TRUE(rows.at("rows").at(0).at(1).is_null()) << "no open price was published";

    // Continuing reads only what follows the last stored day, and what was
    // already held is not stored twice.
    const auto second = node.update("again", "");
    ASSERT_EQ(second.state(), task::v1::SUCCEEDED) << second.error();
    const auto repeated = node.result("again");
    EXPECT_EQ(repeated.stored(), 0U);
    EXPECT_EQ(repeated.unchanged(), 1U) << "the contract list, read again and found the same";
    EXPECT_EQ(repeated.rejected_size(), 2);
    EXPECT_EQ(repeated.unpublished(), 1U);
    EXPECT_EQ(node.coverage().snapshot(), done.snapshot()) << "nothing was appended";
}

TEST(TableUpdates, ARefusedRequestFailsTheTaskInTheSourcesOwnWords)
{
    Node node;
    const auto task = node.update("denied", "2024-02-01", "refused");
    EXPECT_EQ(task.state(), task::v1::FAILED);
    EXPECT_EQ(task.error(), "fixture refused the request: the account has no permission");
    EXPECT_TRUE(node.coverage().rows().empty());
}

namespace {

struct SourceControl
{
    Folder directory{"ast-source-"};
    std::filesystem::path path = directory.path / "response";
    std::optional<std::string> previous;

    SourceControl()
    {
        if (const auto* value = std::getenv("ASTERION_TABLE_FIXTURE_CONTROL"))
            previous = value;
        set("base");
        ::setenv("ASTERION_TABLE_FIXTURE_CONTROL", path.c_str(), 1);
    }

    ~SourceControl()
    {
        if (previous)
            ::setenv("ASTERION_TABLE_FIXTURE_CONTROL", previous->c_str(), 1);
        else
            ::unsetenv("ASTERION_TABLE_FIXTURE_CONTROL");
    }

    void set(const char* value)
    {
        std::ofstream(path) << value;
    }
};

} // namespace

TEST(TableRefresh, CorrectionUnchangedAndEmptyResponsesPreserveOldSnapshotsAndTasks)
{
    SourceControl control;
    Node node;
    ASSERT_EQ(node.update("seed", "2024-02-29").state(), task::v1::SUCCEEDED);
    const auto old_result = node.result("seed").SerializeAsString();
    const auto old_snapshot = node.coverage().snapshot();
    const auto old_data = node.frozen(old_snapshot);
    const auto old_page = node.page();
    const auto preview = node.preview();
    EXPECT_EQ(preview.previous().snapshot(), old_page.revision().snapshot());
    EXPECT_EQ(node.coverage().snapshot(), old_snapshot);
    EXPECT_TRUE(preview.update().has_refresh());
    auto wrong = preview;
    wrong.mutable_update()->set_provider("another.provider");
    wrong.clear_id();
    wrong.set_id(sha256_bytes(wrong.SerializeAsString()));
    EXPECT_THROW(protocol::decode_table_refresh_plan(wrong), std::invalid_argument);

    EXPECT_EQ(
        protocol::encode_table_update(protocol::decode_table_update(preview.update()))
            .SerializeAsString(),
        preview.update().SerializeAsString());
    control.set("corrected");
    node.submit_definition("correct", preview.update());
    ASSERT_EQ(node.run("correct").state(), task::v1::SUCCEEDED);
    const auto changed = node.result("correct");
    EXPECT_EQ(changed.stored(), 1);
    EXPECT_EQ(changed.unchanged(), 0);
    ASSERT_TRUE(changed.has_refreshed_revision());
    EXPECT_NE(changed.refreshed_revision().content(), old_page.revision().content());
    ASSERT_TRUE(changed.has_refresh_observation());
    const auto observation = changed.refresh_observation();
    EXPECT_EQ(observation.selected_snapshot(), old_page.revision().snapshot());
    EXPECT_EQ(observation.revision().snapshot(), changed.refreshed_revision().snapshot());
    EXPECT_FALSE(observation.unchanged());
    EXPECT_GT(observation.response().received_at_ns(), 0);
    // Re-delivery of the exact successful request is idempotent, including the receipt.
    const auto captured = node.page();
    data::v1::DataRequest duplicate;
    auto* delivery = duplicate.mutable_store_table_partition();
    auto* identity = delivery->mutable_identity();
    identity->set_data_instance(observation.data_instance());
    identity->set_task_instance(observation.task_instance());
    identity->set_task_id(observation.task_id());
    identity->set_attempt(observation.attempt());
    delivery->mutable_partition()->set_table(observation.revision().table());
    delivery->mutable_partition()->set_exchange(observation.revision().exchange());
    delivery->mutable_partition()->set_period(observation.revision().period());
    *delivery->mutable_rows() = captured.rows();
    *delivery->mutable_response() = observation.response();
    protocol::DataClient worker_data(node.data_workers, "fixture-data");
    const auto repeated = worker_data.call(duplicate).table_partition();
    EXPECT_EQ(repeated.observation().SerializeAsString(), observation.SerializeAsString());
    EXPECT_FALSE(repeated.unchanged());

    EXPECT_EQ(node.frozen(old_snapshot), old_data);
    EXPECT_EQ(node.result("seed").SerializeAsString(), old_result);
    const auto current = node.coverage().snapshot();
    auto repeat = node.preview();
    node.submit_definition("same", repeat.update());
    ASSERT_EQ(node.run("same").state(), task::v1::SUCCEEDED);
    EXPECT_EQ(node.result("same").unchanged(), 1);
    EXPECT_EQ(node.result("same").stored(), 0);
    ASSERT_TRUE(node.result("same").has_refresh_observation());
    const auto same_observation = node.result("same").refresh_observation();
    EXPECT_NE(same_observation.id(), observation.id());
    EXPECT_TRUE(same_observation.unchanged());
    EXPECT_EQ(same_observation.revision().snapshot(), changed.refreshed_revision().snapshot());
    EXPECT_EQ(same_observation.selected_content(), same_observation.revision().content());
    EXPECT_EQ(node.page().latest_refresh_observation().id(), same_observation.id());

    EXPECT_EQ(node.coverage().snapshot(), current);
    control.set("empty");
    node.submit_definition("empty", node.preview().update());
    ASSERT_EQ(node.run("empty").state(), task::v1::SUCCEEDED);
    EXPECT_EQ(node.result("empty").unpublished(), 1);
    EXPECT_FALSE(node.result("empty").has_refreshed_revision());
    EXPECT_EQ(node.coverage().snapshot(), current);
    EXPECT_EQ(node.frozen(old_snapshot), old_data);
}

TEST(TableRefresh, TransientFailureAndCancellationCanBeExplicitlyRetriedWithTheSameFixedInput)
{
    SourceControl control;
    Node node;
    ASSERT_EQ(node.update("seed", "2024-02-29").state(), task::v1::SUCCEEDED);
    const auto before = node.coverage().snapshot();
    const auto plan = node.preview();
    control.set("fail-once");
    node.submit_definition("retry", plan.update());
    const auto failed = node.run("retry");
    ASSERT_EQ(failed.state(), task::v1::FAILED);
    EXPECT_EQ(failed.error_code(), "unavailable");
    EXPECT_EQ(node.coverage().snapshot(), before);
    task::v1::TaskRequest retry;
    retry.mutable_retry()->set_id("retry");
    ASSERT_TRUE(node.call(retry).has_task());
    const auto completed = node.run("retry");
    ASSERT_EQ(completed.state(), task::v1::SUCCEEDED);
    EXPECT_EQ(completed.attempt(), 2);
    EXPECT_TRUE(completed.error_code().empty());
    EXPECT_EQ(completed.table_update().SerializeAsString(), plan.update().SerializeAsString());
    const auto after = node.coverage().snapshot();
    const auto cancel_plan = node.preview();
    control.set("wait");
    node.submit_definition("cancel", cancel_plan.update());
    ASSERT_EQ(node.run("cancel", control.path).state(), task::v1::CANCELLED);
    EXPECT_EQ(node.coverage().snapshot(), after);
    control.set("corrected");
    retry.mutable_retry()->set_id("cancel");
    ASSERT_TRUE(node.call(retry).has_task());
    ASSERT_EQ(node.run("cancel").state(), task::v1::SUCCEEDED);
    EXPECT_EQ(node.result("cancel").unchanged(), 1);
    EXPECT_EQ(node.coverage().snapshot(), after);
}

TEST(TableRefresh, ConcurrentFixedSelectionsCannotOverwriteAnotherCorrection)
{
    SourceControl control;
    Node node;
    ASSERT_EQ(node.update("seed", "2024-02-29").state(), task::v1::SUCCEEDED);
    const auto plan = node.preview();
    node.submit_definition("first", plan.update());
    node.submit_definition("stale", plan.update());
    control.set("corrected");
    ASSERT_EQ(node.run("first").state(), task::v1::SUCCEEDED);
    const auto snapshot = node.coverage().snapshot();
    const auto content = node.page().revision().content();
    control.set("base");
    const auto stale = node.run("stale");
    EXPECT_EQ(stale.state(), task::v1::FAILED);
    EXPECT_NE(stale.error().find("selected table partition changed"), std::string::npos);
    EXPECT_EQ(node.coverage().snapshot(), snapshot);
    EXPECT_EQ(node.page().revision().content(), content);
    data::v1::DataRequest preview;
    *preview.mutable_table_refresh_plan() = plan.query();
    EXPECT_THROW(node.data.call(preview), Error);
}

TEST(TableRefresh, ADelayedMissingOnlyFetchCannotReplaceANewlyStoredPartition)
{
    SourceControl control;
    Node node;
    const auto definition = protocol::encode_table_update(
        {{"provider", "test.independent.c"},
            {"tables", {"futures.daily"}},
            {"begin_day", "2024-03-01"},
            {"end_day", "2024-03-01"},
            {"requests_per_minute", 60}});
    node.submit_definition("seed", definition);
    ASSERT_EQ(node.run("seed").state(), task::v1::SUCCEEDED);
    auto next = definition;
    next.set_begin_day("2024-03-04");
    next.set_end_day("2024-03-04");
    node.submit_definition("late", next);
    control.set("hold");
    auto late = std::async(std::launch::async, [&] {
        return node.run("late");
    });
    const auto deadline = std::chrono::steady_clock::now() + 8s;
    while (contents(control.path) != "holding" && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(10ms);
    ASSERT_EQ(contents(control.path), "holding");
    control.set("corrected");
    node.submit_definition("creator", next);
    ASSERT_EQ(node.run("creator", {}, 2).state(), task::v1::SUCCEEDED);
    const auto snapshot = node.coverage().snapshot();
    control.set("release");
    ASSERT_EQ(late.get().state(), task::v1::SUCCEEDED);
    const auto result = node.result("late");
    ASSERT_EQ(result.rejected_size(), 1);
    EXPECT_EQ(result.rejected(0).reason(), "day partition is already stored; use explicit refresh");
    EXPECT_EQ(node.coverage().snapshot(), snapshot);
}

TEST(TableRefresh, PreviewSelectsTheExactProviderAndSourceAmongInstalledOffers)
{
    Folder folder{"ast-select-"};
    const auto daily = test::seed_daily_table(
        folder.path,
        "SHFE/cu/2024-03",
        std::vector<std::string>{"2024-03-01"},
        100,
        10);
    history_files::TableStore store(folder.path / "tables");
    const auto source = daily.at("daily_source").get<std::string>();
    const history_files::TableSeries series{"futures.daily", 1, source};
    const auto held = store.current(series, "SHFE", "2024-03-01");
    ASSERT_TRUE(held);
    const std::vector<data::TableOffer> offers{
        {{"futures.daily", 1, "other.daily.v1"}, "other.provider", {"SHFE"}},
        {series, "selected.provider", {"SHFE"}}
    };
    const auto query = protocol::encode_table_refresh_query(
        {{"provider", "selected.provider"},
            {"source", source},
            {"table", "futures.daily"},
            {"exchange", "SHFE"},
            {"period", "2024-03-01"},
            {"previous_snapshot", held->snapshot},
            {"requests_per_minute", 60}});
    const auto plan =
        data::table_refresh_plan(store, offers, query, std::string(64, 'a'), "2024-03-02");
    EXPECT_EQ(plan.update().provider(), "selected.provider");
    EXPECT_EQ(plan.update().refresh().source(), source);
    EXPECT_EQ(store.head(), daily.at("snapshot").get<std::string>());
}
