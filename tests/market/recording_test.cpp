#include "parquet_engine.hpp"
#include "recorder.hpp"
#include <asterion/system/process/child.hpp>
#include <gtest/gtest.h>
#include <thread>

using namespace asterion;
namespace fs = std::filesystem;

namespace {

constexpr std::int64_t base = 1'790'000'040'000; // A minute boundary.

MarketQuote quote(std::int64_t source, const char* last, const char* day)
{
    MarketQuote q;
    q.instrument = {"SHFE", "rb2701"};
    q.trading_day = day;
    q.action_day = "20261028";
    q.update_time = "21:00:00";
    q.source_ms = source;
    q.received_ms = source + 5;
    q.last = Decimal::parse(last);
    q.volume = 10;
    q.bid_levels[0] = {Decimal::parse("3099.5"), 4};
    return q;
}

std::string query(const std::string& sql)
{
    parquet::Engine engine;
    const auto result = engine.run(sql);
    std::string text;
    for (duckdb::idx_t row = 0; row < result->RowCount(); ++row) {
        for (duckdb::idx_t column = 0; column < result->ColumnCount(); ++column)
            text += (column ? " " : "") + result->GetValue(column, row).ToString();
        text += "\n";
    }
    return text;
}

} // namespace

TEST(Recorder, AStalledDiskDropsAnIntervalInsteadOfBlockingTheMarketThread)
{
    using namespace std::chrono_literals;
    const auto root = fs::temp_directory_path() / ("recorder-" + unique_process_id());
    {
        recording::Recorder recorder(root, "instance");
        IntradayBars minutes;
        recorder.poll(base, minutes);
        recording::hold_writes_for_testing(true);
        for (int interval = 1; interval <= 6; ++interval) {
            const auto at = base + interval * 30000;
            recorder.observe(quote(at - 1000, "3100", "20261029"), false, minutes);
            const auto started = std::chrono::steady_clock::now();
            recorder.poll(at, minutes);
            EXPECT_LT(std::chrono::steady_clock::now() - started, 1s);
        }
        // Four writes wait on the disk; the fifth and sixth intervals are dropped.
        EXPECT_EQ(recorder.status().lost, 2u);
        EXPECT_FALSE(recorder.status().error.empty());
        recording::hold_writes_for_testing(false);
        recorder.close("stopped", base + 200000, minutes);
    }
    EXPECT_EQ(
        query(
            "SELECT \"end\" FROM read_parquet('" + (root / "segments/*/*.parquet").string()
            + "') ORDER BY segment"),
        "write_backlog\nwrite_backlog\n");
    fs::remove_all(root);
}

TEST(Recorder, WritesTicksMinutesAndSegmentsWithTheReasonEachSegmentEnded)
{
    const auto root = fs::temp_directory_path() / ("recorder-" + unique_process_id());
    {
        recording::Recorder recorder(root, "instance");
        IntradayBars minutes;
        const auto observe = [&](const MarketQuote& q) {
            recorder.observe(q, false, minutes);
            minutes.observe(q);
        };
        observe(quote(base, "3100", "20261029"));
        observe(quote(base + 60000, "3101", "20261029"));
        auto late = quote(base + 30000, "3090.125", "20261029");
        late.received_ms = base + 60010;
        recorder.observe(late, true, minutes);
        minutes.interrupt();
        recorder.interrupt("reconnecting");
        recorder.interrupt("reconnecting"); // Nothing observed since: no empty segment.
        recorder.poll(base + 60020, minutes);
        observe(quote(base + 120000, "3102", "20261029"));
        observe(quote(base + 180000, "3103", "20261030"));
        recorder.close("stopped", base + 190000, minutes);
        EXPECT_EQ(recorder.status().rows, 5u);
        EXPECT_TRUE(recorder.status().error.empty());
    }
    const auto read = [&](const char* kind) {
        return "read_parquet('" + (root / kind / "*/*.parquet").string()
            + "', hive_partitioning = true)";
    };
    EXPECT_EQ(
        query(
            "SELECT trading_day, segment, last, out_of_order, bid_price_2, bid_quantity_2," " bid_price_3 FROM "
            + read("ticks") + " ORDER BY received_ms"),
        "20261029 1 3100.00000000 false 3099.50000000 4 NULL\n" "20261029 1 3101.00000000 false 3099.50000000 4 NULL\n" "20261029 1 3090.12500000 true 3099.50000000 4 NULL\n" "20261029 2 3102.00000000 false 3099.50000000 4 NULL\n" "20261030 2 3103.00000000 false 3099.50000000 4 NULL\n");
    // The first minute began on its boundary and ended before the loss. The
    // next trading day completes the last bar of the previous one; the bar
    // forming at the stop is partial.
    EXPECT_EQ(
        query(
            "SELECT trading_day, symbol, (start_ms - " + std::to_string(base)
            + ") // 60000, close, partial FROM " + read("minutes") + " ORDER BY start_ms"),
        "20261029 rb2701 0 3100.00000000 false\n" "20261029 rb2701 1 3101.00000000 true\n" "20261029 rb2701 2 3102.00000000 true\n" "20261030 rb2701 3 3103.00000000 true\n");
    // The segment that crossed into the next trading day ends in both days.
    EXPECT_EQ(
        query(
            "SELECT trading_day, instance, segment, first_received_ms - " + std::to_string(base)
            + ", last_received_ms - " + std::to_string(base) + ", ticks, \"end\" FROM "
            + read("segments") + " ORDER BY trading_day, segment"),
        "20261029 instance 1 5 60010 3 reconnecting\n" "20261029 instance 2 120005 120005 1 stopped\n" "20261030 instance 2 180005 180005 1 stopped\n");
    fs::remove_all(root);
}

TEST(Recorder, TheLastMinuteIsWrittenWhenItsTradingDayEndsWhileRecordingContinues)
{
    using namespace std::chrono_literals;
    constexpr std::int64_t day = 1'793'164'800'000; // 2026-10-28 13:20 Beijing.
    const auto root = fs::temp_directory_path() / ("recorder-" + unique_process_id());
    const auto bars = [&] {
        if (!fs::exists(root / "minutes"))
            return std::string{};
        return query(
            "SELECT (start_ms - " + std::to_string(day)
            + ") // 60000, close, partial FROM read_parquet('"
            + (root / "minutes/*/*.parquet").string() + "') ORDER BY start_ms");
    };
    {
        recording::Recorder recorder(root, "instance");
        IntradayBars minutes;
        for (const auto& [at, last] : {std::pair{day, "3100"}, std::pair{day + 60000, "3101"}}) {
            const auto q = quote(at, last, "20261028");
            recorder.observe(q, false, minutes);
            minutes.observe(q);
        }
        // 15:31 Beijing: the day has ended and the last bar will not change.
        const auto closed = day + (2 * 60 + 11) * 60000;
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        for (int step = 0; bars() != "0 3100.00000000 false\n1 3101.00000000 false\n"; ++step) {
            ASSERT_LT(std::chrono::steady_clock::now(), deadline) << bars();
            recorder.poll(closed + step * 30000, minutes);
            std::this_thread::sleep_for(10ms);
        }
        recorder.close("stopped", closed + 600000, minutes);
    }
    EXPECT_EQ(bars(), "0 3100.00000000 false\n1 3101.00000000 false\n");
    fs::remove_all(root);
}

TEST(Recorder, AWarehouseReadInProgressDoesNotHoldBackRecordingWrites)
{
    using namespace std::chrono_literals;
    const auto root = fs::temp_directory_path() / ("recorder-" + unique_process_id());
    {
        recording::Recorder recorder(root, "instance");
        IntradayBars minutes;
        recorder.poll(base, minutes);
        {
            parquet::Engine reading; // A long warehouse query holds its lane.
            for (int interval = 1; interval <= 6; ++interval) {
                const auto at = base + interval * 30000;
                recorder.observe(quote(at - 1000, "3100", "20261029"), false, minutes);
                recorder.poll(at, minutes);
                std::this_thread::sleep_for(50ms);
            }
            EXPECT_EQ(recorder.status().lost, 0u);
        }
        recorder.close("stopped", base + 200000, minutes);
    }
    fs::remove_all(root);
}
