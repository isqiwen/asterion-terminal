#include "history_providers.hpp"
#include <asterion/kernel/native_plugin.hpp>
#include <asterion/foundation/error.hpp>
#include <gtest/gtest.h>
using namespace asterion;
TEST(NativeHistoryPlugin, IndependentCPluginSuppliesTypedCatalogMinutesAndDaily) {
  configure_native_plugins(PLUGIN_FIXTURE_DIRECTORY);
  const auto sources = history_providers::sources();
  ASSERT_EQ(sources.size(), 2);
  EXPECT_EQ(sources[0].id, "fixture.minutes");
  EXPECT_EQ(sources[0].plugin_id, "test.independent.c");
  const auto catalog = history_providers::catalog("fixture.minutes", "", "SHFE", "cu");
  ASSERT_EQ(catalog.size(), 1);
  EXPECT_EQ(catalog[0].identity.key(), "SHFE/cu/2024-03");
  EXPECT_EQ(catalog[0].source_instrument, "vendor/cu2403");
  EXPECT_EQ(catalog[0].multiplier, Decimal::parse("5"));
  EXPECT_NO_THROW(
      history_providers::validate_request("fixture.minutes", catalog[0].identity, 1, 60));
  EXPECT_THROW(history_providers::validate_request("fixture.minutes", catalog[0].identity, 1, 61),
               std::invalid_argument);
  EXPECT_THROW(history_providers::validate_request("fixture.minutes", catalog[0].identity, 30, 60),
               std::invalid_argument);
  unsigned requests = 0;
  auto budget = [&](std::stop_token) { ++requests; };
  auto minutes = history_providers::minutes("fixture.minutes", "", budget);
  HistoricalBarRange range{catalog[0].identity,
                           1,
                           parse_shanghai_time("2024-03-01 09:01:00"),
                           parse_shanghai_time("2024-03-01 09:02:00"),
                           "fixture.minutes",
                           catalog[0].source_instrument};
  const auto rows = minutes->read(range, {});
  ASSERT_EQ(rows.size(), 1);
  EXPECT_EQ(rows[0].close, Decimal::parse("2"));
  EXPECT_EQ(rows[0].timestamp_ns, range.begin_ns);
  std::stop_source cancel;
  cancel.request_stop();
  EXPECT_THROW(minutes->read(range, cancel.get_token()), Error);
  range.interval_minutes = 30;
  EXPECT_THROW(minutes->read(range, {}), std::invalid_argument);
  auto daily = history_providers::daily("fixture.daily", "", budget);
  const auto bars =
      daily->read({catalog[0].identity, parse_trading_date("2024-03-01"),
                   parse_trading_date("2024-03-02"), "fixture.daily", catalog[0].source_instrument},
                  {});
  ASSERT_EQ(bars.size(), 1);
  EXPECT_EQ(bars[0].settlement, Decimal::parse("2"));
  EXPECT_EQ(requests, 2);
  EXPECT_THROW(history_providers::minutes("fixture.daily", ""), std::invalid_argument);
  EXPECT_THROW(history_providers::daily("fixture.minutes", ""), std::invalid_argument);
  EXPECT_THROW(history_providers::minutes("missing", ""), std::invalid_argument);
}
TEST(NativeHistoryPlugin, ConnectionCapabilityReportsStructuredChecks) {
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
  EXPECT_THROW(history_providers::verify_connection("fixture.minutes", "", stop.get_token()),
               Error);
}

TEST(NativeHistoryPlugin, BudgetFailureAbortsReadAndPreservesHostDiagnostic) {
  configure_native_plugins(PLUGIN_FIXTURE_DIRECTORY);
  const HistoricalDailyRange range{{"SHFE", "cu", "2024-03"},
                                   parse_trading_date("2024-03-01"),
                                   parse_trading_date("2024-03-02"),
                                   "fixture.daily",
                                   "vendor/cu2403"};
  auto missing = history_providers::daily("fixture.daily", "");
  EXPECT_THROW(missing->read(range, {}), std::invalid_argument);
  auto rejected = history_providers::daily("fixture.daily", "", [](std::stop_token) {
    throw Error(ErrorCode::unavailable, "fixture budget service unavailable");
  });
  try {
    (void)rejected->read(range, {});
    FAIL() << "read must stop when admission fails";
  } catch (const Error& error) {
    EXPECT_STREQ(error.what(), "fixture budget service unavailable");
  }
}
