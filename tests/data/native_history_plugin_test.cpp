#include "history_providers.hpp"
#include <asterion/kernel/native_plugin.hpp>
#include <asterion/foundation/error.hpp>
#include <gtest/gtest.h>
using namespace asterion;

TEST(NativeHistoryPlugin, IndependentCPluginSuppliesTypedMinutes)
{
    configure_native_plugins(PLUGIN_FIXTURE_DIRECTORY);
    const auto sources = history_providers::sources();
    ASSERT_EQ(sources.size(), 1);
    EXPECT_EQ(sources[0].id, "fixture.minutes");
    EXPECT_EQ(sources[0].plugin_id, "test.independent.c");
    const HistoryIdentity contract{"SHFE", "cu", "2024-03"};
    EXPECT_NO_THROW(history_providers::validate_request("fixture.minutes", contract, 1, 60));
    EXPECT_THROW(
        history_providers::validate_request("fixture.minutes", contract, 1, 61),
        std::invalid_argument);
    EXPECT_THROW(
        history_providers::validate_request("fixture.minutes", contract, 30, 60),
        std::invalid_argument);
    unsigned requests = 0;
    auto budget = [&](std::stop_token) {
        ++requests;
    };
    auto minutes = history_providers::minutes("fixture.minutes", "", budget);
    // The plugin's own code, as its contract list gives it.
    HistoricalBarRange range{
        contract,
        1,
        parse_shanghai_time("2024-03-01 09:01:00"),
        parse_shanghai_time("2024-03-01 09:02:00"),
        "fixture.minutes",
        "vendor/cu2403"
    };
    const auto rows = minutes->read(range, {});
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows[0].close, Decimal::parse("2"));
    EXPECT_EQ(rows[0].timestamp_ns, range.begin_ns);
    EXPECT_EQ(requests, 1);
    std::stop_source cancel;
    cancel.request_stop();
    EXPECT_THROW(minutes->read(range, cancel.get_token()), Error);
    range.interval_minutes = 30;
    EXPECT_THROW(minutes->read(range, {}), std::invalid_argument);
    EXPECT_THROW(history_providers::minutes("missing", ""), std::invalid_argument);
}

TEST(NativeHistoryPlugin, ConnectionCapabilityReportsStructuredChecks)
{
    configure_native_plugins(PLUGIN_FIXTURE_DIRECTORY);
    const auto sources = history_providers::sources();
    ASSERT_TRUE(sources.front().connection);
    EXPECT_EQ(sources.front().connection->requests_per_minute_default, 30);
    const auto valid = history_providers::verify_connection("fixture.minutes", "");
    ASSERT_EQ(valid.size(), 2);
    EXPECT_EQ(valid[0].scope, "catalog");
    EXPECT_EQ(valid[0].state, 0);
    EXPECT_EQ(valid[1].scope, "history");
    EXPECT_EQ(valid[1].state, 0);
    EXPECT_EQ(history_providers::verify_connection("fixture.minutes", "invalid")[0].state, 2);
    EXPECT_EQ(history_providers::verify_connection("fixture.minutes", "limited")[0].state, 4);
    std::stop_source stop;
    stop.request_stop();
    EXPECT_THROW(
        history_providers::verify_connection("fixture.minutes", "", stop.get_token()),
        Error);
}

TEST(NativeHistoryPlugin, BudgetFailureAbortsReadAndPreservesHostDiagnostic)
{
    configure_native_plugins(PLUGIN_FIXTURE_DIRECTORY);
    const HistoricalBarRange range{
        {"SHFE", "cu", "2024-03"},
        1,
        parse_shanghai_time("2024-03-01 09:01:00"),
        parse_shanghai_time("2024-03-01 09:02:00"),
        "fixture.minutes",
        "vendor/cu2403"
    };
    auto missing = history_providers::minutes("fixture.minutes", "");
    EXPECT_THROW(missing->read(range, {}), std::invalid_argument);
    auto rejected = history_providers::minutes("fixture.minutes", "", [](std::stop_token) {
        throw Error(ErrorCode::unavailable, "fixture budget service unavailable");
    });
    try {
        (void)rejected->read(range, {});
        FAIL() << "read must stop when admission fails";
    } catch (const Error& error) {
        EXPECT_STREQ(error.what(), "fixture budget service unavailable");
    }
}

TEST(NativeHistoryPlugin, TablesArriveAsTextAndFailuresKeepThePluginsOwnWords)
{
    configure_native_plugins(PLUGIN_FIXTURE_DIRECTORY);
    const auto offered = history_providers::table_sources();
    ASSERT_EQ(offered.size(), 3U);
    EXPECT_EQ(offered[2].table, "futures.daily");
    EXPECT_EQ(offered[2].source, "fixture.daily.v1");
    EXPECT_EQ(offered[2].plugin_id, "test.independent.c");
    EXPECT_EQ(offered[2].venues, std::vector<std::string>{"SHFE"});
    EXPECT_EQ(history_providers::table_artifact("test.independent.c").size(), 64U);
    unsigned requests = 0;
    auto reader = history_providers::tables("test.independent.c", "fixture", [&](std::stop_token) {
        ++requests;
    });
    const auto& daily = history_table("futures.daily");
    const auto day =
        table_partition(daily, "SHFE", "2024-03-01", reader->read(daily, "SHFE", "2024-03-01", {}));
    ASSERT_EQ(day.rows.size(), 1U);
    EXPECT_EQ(table_cell_text(daily.columns[0], day.rows[0][0]), "SHFE/cu/2024-03");
    EXPECT_EQ(table_cell_text(daily.columns[9], day.rows[0][9]), "432000000000.25");
    EXPECT_EQ(table_cell_text(daily.columns[1], day.rows[0][1]), std::nullopt);
    EXPECT_TRUE(reader->read(daily, "SHFE", "2024-03-07", {}).empty()) << "not published yet";
    EXPECT_EQ(reader->read(history_table("futures.calendar"), "SHFE", "2024", {}).size(), 9U);
    EXPECT_EQ(requests, 3U);
    // The partition is unusable: the plugin's sentence, as one distinct failure.
    try {
        (void)reader->read(daily, "SHFE", "2024-03-06", {});
        FAIL() << "the source reported unusable data";
    } catch (const history_providers::TableRejected& error) {
        EXPECT_STREQ(
            error.what(),
            "futures.daily SHFE 2024-03-06: vendor/cu2499 is not in the contract list");
    }
    // The request did not complete: the status decides the code, the plugin the words.
    auto refused =
        history_providers::tables("test.independent.c", "refused", [](std::stop_token) {});
    try {
        (void)refused->read(daily, "SHFE", "2024-03-01", {});
        FAIL() << "a refused request has no rows";
    } catch (const Error& error) {
        EXPECT_EQ(error.code(), ErrorCode::permission_denied);
        EXPECT_STREQ(error.what(), "fixture refused the request: the account has no permission");
    }
    EXPECT_THROW((void)reader->read(daily, "DCE", "2024-03-01", {}), std::invalid_argument);
    EXPECT_THROW((void)reader->read(daily, "SHFE", "2024", {}), std::invalid_argument);
    EXPECT_THROW(
        (void)history_providers::tables("test.independent.c", "")
            ->read(daily, "SHFE", "2024-03-01", {}),
        std::invalid_argument);
    EXPECT_THROW(history_providers::tables("missing", "fixture"), std::invalid_argument);
    std::stop_source cancelled;
    cancelled.request_stop();
    EXPECT_THROW((void)reader->read(daily, "SHFE", "2024-03-01", cancelled.get_token()), Error);
}
