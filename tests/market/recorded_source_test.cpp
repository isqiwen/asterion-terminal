#include "acquisition/recorded.hpp"
#include "warehouse/composer.hpp"
#include "warehouse/research.hpp"
#include "acquisition/recorded_intake.hpp"
#include "acquisition/recorded_publication.hpp"
#include "acquisition/manager.hpp"
#include "recording/recorder.hpp"
#include "recorded/compaction.hpp"
#include "recorded/compactor.hpp"
#include <asterion/system/process/child.hpp>
#include <gtest/gtest.h>
#include <future>
#include <fstream>

namespace {

using namespace asterion;
using namespace std::chrono_literals;
namespace ac = market_data::acquisition;
namespace wh = market_data::warehouse;
namespace fs = std::filesystem;
constexpr std::int64_t base = 1'793'164'800'000;

class RecordedSource : public testing::Test
{
protected:
    fs::path root = fs::temp_directory_path() / ("recorded-source-" + unique_process_id());
    std::shared_ptr<std::shared_mutex> archive = std::make_shared<std::shared_mutex>();
    std::string trading_day = "20261028";

    void TearDown() override
    {
        fs::remove_all(root);
    }

    void record(
        const std::string& instance,
        std::int64_t from = base,
        InstrumentId id = {"SHFE", "rb2701"},
        const char* price = "3100.123456",
        const char* average = nullptr)
    {
        recording::Recorder recorder(root / "recordings", instance);
        IntradayBars minutes;
        for (int i = 0; i < 2; ++i) {
            MarketQuote q;
            q.instrument = id;
            q.trading_day = trading_day;
            q.source_ms = from + i * 60000;
            q.received_ms = q.source_ms + 5;
            q.last = Decimal::parse(price);
            if (average)
                q.average_price = Decimal::parse(average);
            q.volume = 9007199254740993LL + i;
            recorder.observe(q, false, minutes);
            minutes.observe(q);
        }
        recorder.close("stopped", from + 60010, minutes);
        ASSERT_TRUE(recorder.status().error.empty()) << recorder.status().error;
        // The day went quiet long before any `wall` these tests use.
        for (const auto& entry : fs::recursive_directory_iterator(root / "recordings"))
            if (entry.is_regular_file())
                fs::last_write_time(
                    entry.path(),
                    std::chrono::file_clock::from_sys(
                        std::chrono::system_clock::time_point(std::chrono::milliseconds(from))));
    }

    ac::wire::AcquisitionTask publish(
        ac::Manager& manager,
        const std::string& id,
        bool refresh = false,
        const std::string& contract = "SHFE/rb/2027-01")
    {
        ac::wire::StartAcquisition request;
        request.set_id(id);
        request.set_provider("recorder");
        request.set_table(::market::TABLE_MINUTES);
        if (!contract.empty())
            request.add_contracts(contract);
        request.set_first_day("2026-10-28");
        request.set_last_day("2026-10-29");
        request.set_refetch(refresh);
        manager.submit(request);
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        for (;;) {
            const auto task = manager.get(id);
            if (task.state() != ac::wire::TASK_STATE_RUNNING
                && task.state() != ac::wire::TASK_STATE_QUEUED)
                return task;
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("recorded publication did not finish");
            std::this_thread::sleep_for(5ms);
        }
    }

    ac::Factory factory()
    {
        return [this](std::stop_token stop) {
            return std::make_unique<ac::Recorded>(root / "recordings", archive, stop);
        };
    }
};

TEST_F(RecordedSource, PublicationRetainsPartialBarsGapsAndOldVersionsAcrossCompaction)
{
    record("first");
    ac::Manager manager(root, {{"recorder", factory()}});
    const auto task = publish(manager, "first");
    ASSERT_EQ(task.state(), ac::wire::TASK_STATE_SUCCEEDED) << task.error().message();
    EXPECT_EQ(task.items_done(), 2);
    EXPECT_EQ(task.items_empty(), 1);
    wh::Store store(wh::source_directory(root, "recorder"), "recorder");
    const auto original = store.current();
    EXPECT_EQ(original.identity, task.published_version());
    ASSERT_EQ(original.partitions.size(), 2);
    const std::string key = "minutes/SHFE/2026-10-28/rb/2027-01";
    const std::string observations = "observations/SHFE/2026-10-28/rb/2027-01";
    const auto rows = store.read(original, key);
    ASSERT_EQ(rows.size(), 2);
    EXPECT_FALSE(std::get<bool>(rows[0][5]));
    EXPECT_TRUE(std::get<bool>(rows[1][5]));
    EXPECT_EQ(std::get<std::string>(rows[0][9]), "3100.123456");
    EXPECT_TRUE(std::holds_alternative<std::monostate>(rows[0][11]));
    const auto segments = store.read(original, observations);
    ASSERT_EQ(segments.size(), 1);
    EXPECT_EQ(std::get<std::string>(segments[0][8]), "stopped");
    record("later", base + 300000);
    EXPECT_EQ(publish(manager, "skip").state(), ac::wire::TASK_STATE_SUCCEEDED);
    EXPECT_EQ(store.current().identity, original.identity);
    {
        std::unique_lock lock(*archive);
        recording::compact_day(root / "recordings", "20261028");
    }
    ASSERT_FALSE(fs::exists(root / "recordings/minutes/trading_day=20261028/first-1.parquet"));
    const auto updated = publish(manager, "refresh", true);
    ASSERT_EQ(updated.state(), ac::wire::TASK_STATE_SUCCEEDED) << updated.error().message();
    EXPECT_NE(updated.published_version(), original.identity);
    EXPECT_EQ(store.read(store.current(), key).size(), 4);
    EXPECT_EQ(store.read(store.current(), observations).size(), 2);
    EXPECT_EQ(store.read(original, key), rows);
    EXPECT_EQ(store.read(original, observations), segments);
}

TEST_F(
    RecordedSource,
    NativeSymbolsKeepTheirDeliveryAndCancellationDoesNotWaitForArchiveReplacement)
{
    record("czce", base, {"CZCE", "AP701"});
    record("cffex", base, {"CFFEX", "IF2701"});
    auto source = factory()({});
    EXPECT_EQ(source->minutes("CZCE/ap/2027-01", "2026-10-28").rows.size(), 2);
    EXPECT_EQ(source->minutes("CFFEX/if/2027-01", "2026-10-28").rows.size(), 2);
    EXPECT_THROW(source->minutes("CZCE/ap/2037-01", "2026-10-28"), ac::SourceError);
    EXPECT_TRUE(source->minutes("SHFE/rb/2027-01", "2026-10-28").rows.empty());
    std::stop_source stop;
    auto waiting = factory()(stop.get_token());
    std::unique_lock lock(*archive);
    auto read = std::async(std::launch::async, [&] {
        return waiting->minutes("CZCE/ap/2027-01", "2026-10-28");
    });
    EXPECT_EQ(read.wait_for(30ms), std::future_status::timeout);
    stop.request_stop();
    const auto cancelled = read.wait_for(2s);
    lock.unlock(); // Always release on failure so the test can report the regression.
    ASSERT_EQ(cancelled, std::future_status::ready);
    EXPECT_THROW(static_cast<void>(read.get()), ac::SourceError);
}

TEST_F(RecordedSource, MaintenanceWaitDoesNotBlockRecordingOrFixedVersions)
{
    record("first");
    ac::Manager manager(root, {{"recorder", factory()}});
    ASSERT_EQ(publish(manager, "initial").state(), ac::wire::TASK_STATE_SUCCEEDED);
    wh::Store store(wh::source_directory(root, "recorder"), "recorder");
    const auto original = store.current();
    const std::string key = "minutes/SHFE/2026-10-28/rb/2027-01";
    const auto baseline = store.read(original, key);
    recording::Compactor compactor(root / "recordings", archive, 30);
    const auto now = recording::Compactor::Clock::now();
    const auto wall = std::chrono::sys_days{std::chrono::year{2026} / 10 / 29};
    std::unique_lock held(*archive);
    compactor.poll(now, wall);
    const auto deadline = now + 5s;
    while (compactor.status().day.empty() && recording::Compactor::Clock::now() < deadline)
        std::this_thread::sleep_for(5ms);
    ASSERT_EQ(compactor.status().day, "20261028");
    // The feed's writer and published research versions use independent lanes.
    record("during-merge", base + 300000);
    EXPECT_EQ(store.read(original, key), baseline);
    EXPECT_TRUE(compactor.running());
    held.unlock();
    while (compactor.running() && recording::Compactor::Clock::now() < deadline) {
        compactor.poll(now, wall);
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_FALSE(compactor.running());
    EXPECT_TRUE(compactor.status().error.empty()) << compactor.status().error;
    EXPECT_TRUE(fs::exists(root / "recordings/minutes/trading_day=20261028/SHFE.parquet"));
    ASSERT_EQ(publish(manager, "after-merge", true).state(), ac::wire::TASK_STATE_SUCCEEDED);
    EXPECT_EQ(store.read(store.current(), key).size(), 4);
    EXPECT_EQ(store.read(original, key), baseline);
}

TEST_F(RecordedSource, AMergedDayIsReportedAndEveryContractOfItReachesResearch)
{
    record("first");
    record("copper", base, {"SHFE", "cu2612"}, "80000");
    recording::Compactor compactor(root / "recordings", archive, 30);
    auto now = recording::Compactor::Clock::now();
    const auto wall = std::chrono::sys_days{std::chrono::year{2026} / 10 / 29};
    std::optional<std::string> merged;
    const auto deadline = now + 5s;
    while (!merged && recording::Compactor::Clock::now() < deadline) {
        merged = compactor.poll(now, wall);
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_EQ(merged, "20261028") << compactor.status().error;
    ac::Manager manager(root, {{"recorder", factory()}});
    const auto task = publish(manager, "day", true, "");
    ASSERT_EQ(task.state(), ac::wire::TASK_STATE_SUCCEEDED) << task.error().message();
    std::map<std::string, std::string> research;
    wh::Store store(wh::source_directory(root, "research"), "research");
    wh::compose_research(root, store);
    for (const auto& [key, part] : store.current().partitions)
        research[key] = part.source;
    EXPECT_EQ(
        research,
        (std::map<std::string, std::string>{
            {"minutes/SHFE/2026-10-28/cu/2026-12", "recorder"},
            {"minutes/SHFE/2026-10-28/rb/2027-01", "recorder"},
            {"observations/SHFE/2026-10-28/cu/2026-12", "recorder"},
            {"observations/SHFE/2026-10-28/rb/2027-01", "recorder"}
        }));
}

TEST_F(RecordedSource, MaintenanceShutdownDoesNotWaitForAnArchiveReader)
{
    record("first");
    auto compactor = std::make_unique<recording::Compactor>(root / "recordings", archive, 30);
    const auto now = recording::Compactor::Clock::now();
    const auto wall = std::chrono::sys_days{std::chrono::year{2026} / 10 / 29};
    std::shared_lock held(*archive);
    compactor->poll(now, wall);
    const auto deadline = now + 5s;
    while (compactor->status().day.empty() && recording::Compactor::Clock::now() < deadline)
        std::this_thread::sleep_for(5ms);
    ASSERT_EQ(compactor->status().day, "20261028");
    auto stopped = std::async(std::launch::async, [&] {
        compactor.reset();
    });
    const auto ready = stopped.wait_for(2s);
    held.unlock();
    stopped.get();
    EXPECT_EQ(ready, std::future_status::ready);
    EXPECT_FALSE(fs::exists(root / "recordings/minutes/trading_day=20261028/SHFE.parquet"));
    EXPECT_EQ(factory()({})->minutes("SHFE/rb/2027-01", "2026-10-28").rows.size(), 2);
}

TEST_F(RecordedSource, AScanFailureIsReportedAndRetriedAfterBackoff)
{
    const auto damaged = root / "recordings/ticks/trading_day=20261028";
    fs::create_directories(damaged.parent_path());
    std::ofstream(damaged) << "not a directory";
    recording::Compactor compactor(root / "recordings", archive, 30);
    const auto now = recording::Compactor::Clock::now();
    const auto wall = std::chrono::sys_days{std::chrono::year{2026} / 10 / 29};
    const auto finish = [&](auto when) {
        const auto deadline = recording::Compactor::Clock::now() + 5s;
        while (compactor.running() && recording::Compactor::Clock::now() < deadline) {
            compactor.poll(when, wall);
            std::this_thread::sleep_for(5ms);
        }
        ASSERT_FALSE(compactor.running());
    };
    compactor.poll(now, wall);
    finish(now);
    ASSERT_FALSE(compactor.status().error.empty());
    fs::remove(damaged);
    record("repaired");
    compactor.poll(now + 59min, wall);
    EXPECT_FALSE(compactor.running());
    compactor.poll(now + 1h, wall);
    finish(now + 1h);
    EXPECT_TRUE(compactor.status().error.empty());
    EXPECT_TRUE(fs::exists(root / "recordings/minutes/trading_day=20261028/SHFE.parquet"));
}

TEST_F(RecordedSource, ADayThatFailsToMergeDoesNotHoldBackLaterDays)
{
    record("first");
    const auto damaged = root / "recordings/ticks/trading_day=20261027/damaged.parquet";
    fs::create_directories(damaged.parent_path());
    std::ofstream(damaged) << "not parquet";
    fs::last_write_time(
        damaged,
        fs::last_write_time(root / "recordings/segments/trading_day=20261028/first.parquet"));
    recording::Compactor compactor(root / "recordings", archive, 30);
    auto now = recording::Compactor::Clock::now();
    const auto wall = std::chrono::sys_days{std::chrono::year{2026} / 10 / 29};
    const auto next = [&] {
        now += 2min;
        std::optional<std::string> merged;
        compactor.poll(now, wall);
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (compactor.running() && std::chrono::steady_clock::now() < deadline) {
            merged = compactor.poll(now, wall);
            std::this_thread::sleep_for(5ms);
        }
        return merged;
    };
    EXPECT_FALSE(next());
    EXPECT_FALSE(compactor.status().error.empty());
    EXPECT_EQ(next(), "20261028");
    EXPECT_FALSE(compactor.status().error.empty());
    EXPECT_TRUE(fs::exists(damaged));
}

TEST_F(RecordedSource, ADayThatFailsToPublishDoesNotHoldBackLaterDays)
{
    trading_day = "20261027";
    record("too-precise", base, {"SHFE", "rb2701"}, "3100.12345678");
    trading_day = "20261028";
    record("first");
    ac::Manager manager(root, {{"recorder", factory()}});
    ac::RecordedPublication queue(root / "unpublished.json", manager);
    queue.add("20261027");
    queue.add("20261028");
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (queue.pending().size() == 2 && std::chrono::steady_clock::now() < deadline) {
        queue.poll(std::chrono::steady_clock::now());
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_EQ(queue.pending(), std::set<std::string>{"20261027"});
}

TEST_F(RecordedSource, IntakeTakesAFinishedDayFromTheDiskAlone)
{
    record("first");
    ac::Manager manager(root, {{"recorder", factory()}});
    ac::RecordedIntake intake(root / "recordings", archive, 30, manager);
    wh::Store store(wh::source_directory(root, "recorder"), "recorder");
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (store.current().partitions.empty() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(20ms);
    EXPECT_TRUE(store.current().partitions.contains("minutes/SHFE/2026-10-28/rb/2027-01"));
    EXPECT_TRUE(fs::exists(root / "recordings/minutes/trading_day=20261028/SHFE.parquet"));
    intake.stop();
}

TEST_F(RecordedSource, ResearchFollowsACommitWithoutTheTaskDoingTheWork)
{
    record("first");
    wh::Composer composer(root);
    ac::Manager manager(root, {{"recorder", factory()}}, [&] {
        composer.request();
    });
    ASSERT_EQ(publish(manager, "first").state(), ac::wire::TASK_STATE_SUCCEEDED);
    const wh::Store research(wh::source_directory(root, "research"), "research");
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (research.current().partitions.empty() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);
    EXPECT_EQ(
        research.current().partitions.at("minutes/SHFE/2026-10-28/rb/2027-01").source,
        "recorder");
}

TEST_F(RecordedSource, ADaysContractSnapshotBecomesThatDaysPriceTicksAndOutranksReports)
{
    ::market::Catalog catalog;
    auto* contract = catalog.add_contracts();
    contract->set_contract("SHFE/rb/2027-01");
    contract->mutable_price_tick()->set_text("1");
    fs::create_directories(root / "recordings/catalog");
    std::ofstream(root / "recordings/catalog/20261028.pb", std::ios::binary)
        << catalog.SerializeAsString();
    ac::Manager manager(root, {{"recorder", factory()}});
    ac::wire::StartAcquisition request;
    request.set_id("ticks");
    request.set_provider("recorder");
    request.set_table(::market::TABLE_PRICE_TICKS);
    request.set_first_day("2026-10-28");
    request.set_last_day("2026-10-28");
    manager.submit(request);
    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (manager.get("ticks").state() != ac::wire::TASK_STATE_SUCCEEDED
        && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);
    wh::Store report(wh::source_directory(root, "gtja"), "gtja");
    {
        auto writer = report.writer();
        writer.put("price_ticks/SHFE/2026-10-28", std::vector<wh::Row>{}, {"2026-10-28"});
        writer.commit();
    }
    wh::Store research(wh::source_directory(root, "research"), "research");
    const auto version = wh::compose_research(root, research);
    EXPECT_EQ(version.partitions.at("price_ticks/SHFE/2026-10-28").source, "recorder");
    const auto rows = research.read(version, "price_ticks/SHFE/2026-10-28");
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(std::get<std::string>(rows[0][2]), "SHFE/rb/2027-01");
    EXPECT_EQ(std::get<std::string>(rows[0][3]), "2026-10-28");
    EXPECT_EQ(std::get<std::string>(rows[0][4]), "2026-10-29");
    EXPECT_EQ(std::get<std::string>(rows[0][5]), "1.00000000");
}

TEST_F(RecordedSource, ATradedAverageWithEightPlacesIsRoundedToSix)
{
    record("average", base, {"SHFE", "rb2701"}, "3100.123456", "666.66733333");
    ac::Manager manager(root, {{"recorder", factory()}});
    const auto task = publish(manager, "average");
    ASSERT_EQ(task.state(), ac::wire::TASK_STATE_SUCCEEDED) << task.error().message();
    wh::Store store(wh::source_directory(root, "recorder"), "recorder");
    const auto rows = store.read(store.current(), "minutes/SHFE/2026-10-28/rb/2027-01");
    ASSERT_FALSE(rows.empty());
    EXPECT_EQ(std::get<std::string>(rows[0][11]), "666.667333");
}

TEST_F(RecordedSource, AMergedDayStaysPendingAcrossARestartUntilItsPublicationSucceeds)
{
    record("first");
    ac::Manager manager(root, {{"recorder", factory()}});
    const auto file = root / "unpublished.json";
    const auto settle = [&](ac::RecordedPublication& queue) {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (!queue.pending().empty()) {
            queue.poll(std::chrono::steady_clock::now());
            ASSERT_LT(std::chrono::steady_clock::now(), deadline);
            std::this_thread::sleep_for(5ms);
        }
    };
    {
        ac::RecordedPublication queue(file, manager);
        queue.add("20261028");
    }
    // A restart before the publication keeps the day.
    ac::RecordedPublication queue(file, manager);
    EXPECT_EQ(queue.pending(), std::set<std::string>{"20261028"});
    settle(queue);
    EXPECT_TRUE(queue.pending().empty());
    wh::Store store(wh::source_directory(root, "recorder"), "recorder");
    EXPECT_FALSE(store.read(store.current(), "minutes/SHFE/2026-10-28/rb/2027-01").empty());

    // A failed publication keeps the day for a later attempt.
    record("too-precise", base + 300000, {"SHFE", "rb2701"}, "3100.12345678");
    ac::RecordedPublication retry(root / "retry.json", manager);
    retry.add("20261028");
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    for (;;) {
        retry.poll(std::chrono::steady_clock::now());
        const auto tasks = manager.list({});
        if (std::any_of(tasks.tasks().begin(), tasks.tasks().end(), [](const auto& task) {
                return task.state() == ac::wire::TASK_STATE_FAILED;
            }))
            break;
        ASSERT_LT(std::chrono::steady_clock::now(), deadline);
        std::this_thread::sleep_for(5ms);
    }
    retry.poll(std::chrono::steady_clock::now());
    EXPECT_EQ(retry.pending(), std::set<std::string>{"20261028"});
}

TEST_F(RecordedSource, UnrepresentableDecimalsDoNotPublishHalfAVersion)
{
    record("exact");
    ac::Manager manager(root, {{"recorder", factory()}});
    ASSERT_EQ(publish(manager, "initial").state(), ac::wire::TASK_STATE_SUCCEEDED);
    wh::Store store(wh::source_directory(root, "recorder"), "recorder");
    const auto original = store.current();
    record("too-precise", base + 300000, {"SHFE", "rb2701"}, "3100.12345678");
    EXPECT_EQ(publish(manager, "rejected", true).state(), ac::wire::TASK_STATE_FAILED);
    EXPECT_EQ(store.current().identity, original.identity);
}

} // namespace
