#include "parquet_engine.hpp"
#include "recorder.hpp"
#include <asterion/kernel/process/child.hpp>
#include <gtest/gtest.h>

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
