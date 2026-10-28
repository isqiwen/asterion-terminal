#include "parquet_engine.hpp"
#include "recorder.hpp"
#include <asterion/kernel/process/child.hpp>
#include <gtest/gtest.h>

using namespace asterion;
namespace fs = std::filesystem;

namespace {

MarketQuote quote(std::int64_t received, const char* last, const char* day)
{
    MarketQuote q;
    q.instrument = {"SHFE", "rb2701"};
    q.trading_day = day;
    q.action_day = "20261028";
    q.update_time = "21:00:00";
    q.source_ms = received - 5;
    q.received_ms = received;
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

TEST(Recorder, WritesTicksBySegmentAndTradingDayWithTheReasonEachSegmentEnded)
{
    const auto root = fs::temp_directory_path() / ("recorder-" + unique_process_id());
    {
        market_data::Recorder recorder(root, "instance");
        recorder.observe(quote(1000, "3100", "20261029"), false);
        auto late = quote(1100, "3090.125", "20261029");
        late.source_ms = 900;
        recorder.observe(late, true);
        recorder.interrupt("reconnecting");
        recorder.interrupt("reconnecting"); // Nothing observed since: no empty segment.
        recorder.observe(quote(5000, "3101", "20261029"), false);
        recorder.observe(quote(9000, "3102", "20261030"), false);
        recorder.close("stopped", 10000);
        EXPECT_EQ(recorder.status().rows, 4u);
        EXPECT_EQ(recorder.status().files, 2u);
        EXPECT_TRUE(recorder.status().error.empty());
    }
    const auto ticks =
        "read_parquet('" + (root / "ticks/*/*.parquet").string() + "', hive_partitioning = true)";
    const auto segments = "read_parquet('" + (root / "segments/*/*.parquet").string()
        + "', hive_partitioning = true)";
    EXPECT_EQ(
        query(
            "SELECT trading_day, segment, last, out_of_order, bid_price_2, bid_quantity_2," " bid_price_3 FROM "
            + ticks + " ORDER BY received_ms"),
        "20261029 1 3100.00000000 false 3099.50000000 4 NULL\n" "20261029 1 3090.12500000 true 3099.50000000 4 NULL\n" "20261029 2 3101.00000000 false 3099.50000000 4 NULL\n" "20261030 2 3102.00000000 false 3099.50000000 4 NULL\n");
    // The segment that crossed into the next trading day ends in both days.
    EXPECT_EQ(
        query(
            "SELECT trading_day, instance, segment, first_received_ms, last_received_ms, ticks," " \"end\" FROM "
            + segments + " ORDER BY trading_day, segment"),
        "20261029 instance 1 1000 1100 2 reconnecting\n" "20261029 instance 2 5000 5000 1 stopped\n" "20261030 instance 2 9000 9000 1 stopped\n");
    fs::remove_all(root);
}
