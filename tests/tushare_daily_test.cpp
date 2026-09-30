#include "tushare.hpp"
#include <gtest/gtest.h>
using namespace asterion;
namespace {
HistoricalDailyRange daily_range() {
  using namespace std::chrono;
  return {{"SHFE", "cu", "2024-03"},
          year(2024) / February / 28,
          year(2024) / February / 29,
          "tushare.fut_daily",
          "CU2403.SHF"};
}
std::string daily_response() {
  return R"({"code":0,"data":{"fields":["ts_code","trade_date","pre_close","pre_settle","open","high","low","close","settle","vol","amount","oi"],"items":[["CU2403.SHF","20240229",100,99.5,100.00000001,102,99,101,100.5,20,123.456789012345,2000],["CU2403.SHF","20240228",null,null,100,102,99,100,null,10,1.25e-4,1900]]}})";
}
} // namespace
TEST(TushareDaily, PreservesTradingDatesOptionalSettlementAndExactCurrencyUnits) {
  tushare::Daily provider("fixture-token", [](const auto& body, auto) {
    const auto request = Json::parse(body);
    EXPECT_EQ(request.at("api_name"), "fut_daily");
    EXPECT_EQ(
        request.at("params"),
        (Json{{"ts_code", "CU2403.SHF"}, {"start_date", "20240228"}, {"end_date", "20240229"}}));
    EXPECT_EQ(request.at("token"), "fixture-token");
    EXPECT_EQ(request.at("fields"),
              "ts_code,trade_date,pre_close,pre_settle,open,high,low,close,settle,vol,amount,oi");
    return daily_response();
  });
  provider.start();
  const auto bars = provider.read(daily_range(), {});
  ASSERT_EQ(bars.size(), 2);
  EXPECT_TRUE(bars[0].trading_day == daily_range().begin);
  EXPECT_TRUE(bars[1].trading_day == daily_range().end);
  EXPECT_FALSE(bars[0].previous_close);
  EXPECT_FALSE(bars[0].previous_settlement);
  EXPECT_FALSE(bars[0].settlement);
  EXPECT_EQ(bars[0].amount.str(), "1.25");
  EXPECT_EQ(bars[1].open.str(), "100.00000001");
  EXPECT_EQ(bars[1].amount.str(), "1234567.89012345");
  EXPECT_EQ(bars[1].settlement, Decimal::parse("100.5"));
  EXPECT_EQ(bars[1].previous_settlement, Decimal::parse("99.5"));
  EXPECT_EQ(provider.descriptor().kind, PluginKind::data);
}
TEST(TushareDaily, RejectsMalformedDatesContractsAmountsQuantitiesAndAmbiguousRows) {
  auto valid = Json::parse(daily_response());
  // Use strings for mutations so the test does not round original numeric text.
  valid["data"]["items"][0][10] = "123.456789012345";
  std::vector<Json> invalid;
  for (const auto& [column, value] : std::vector<std::pair<int, Json>>{{0, "CU2404.SHF"},
                                                                       {1, "20240230"},
                                                                       {1, "2024-02-29"},
                                                                       {1, "20240301"},
                                                                       {4, "100.000000001"},
                                                                       {4, nullptr},
                                                                       {5, "98"},
                                                                       {9, "1.5"},
                                                                       {11, "2.5"},
                                                                       {10, "123.1234567890123"},
                                                                       {10, "92233720368"},
                                                                       {10, "-1"}}) {
    auto value_case = valid;
    value_case["data"]["items"][0][column] = value;
    invalid.push_back(std::move(value_case));
  }
  auto duplicate_day = valid;
  duplicate_day["data"]["items"].push_back(duplicate_day["data"]["items"][0]);
  invalid.push_back(duplicate_day);
  auto duplicate_field = valid;
  duplicate_field["data"]["fields"][2] = "ts_code";
  invalid.push_back(duplicate_field);
  auto missing_field = valid;
  missing_field["data"]["fields"][2] = "unsupported";
  invalid.push_back(missing_field);
  for (const auto& response : invalid) {
    tushare::Daily provider("fixture-token", [&](const auto&, auto) { return response.dump(); });
    provider.start();
    EXPECT_THROW(provider.read(daily_range(), {}), std::exception) << response.dump();
  }
}
TEST(TushareDaily, BoundsQueriesResponsesAndLifecycleWithoutLeakingProviderText) {
  int calls = 0;
  tushare::Daily provider("private-fixture-token", [&](const auto&, auto) {
    ++calls;
    return R"({"code":2002,"msg":"echo private-fixture-token"})";
  });
  EXPECT_THROW(provider.read(daily_range(), {}), std::logic_error);
  provider.start();
  auto invalid = daily_range();
  invalid.begin = std::chrono::year(2023) / std::chrono::January / 1;
  EXPECT_THROW(provider.read(invalid, {}), std::invalid_argument);
  invalid = daily_range();
  std::swap(invalid.begin, invalid.end);
  EXPECT_THROW(provider.read(invalid, {}), std::invalid_argument);
  std::stop_source cancelled;
  cancelled.request_stop();
  EXPECT_THROW(provider.read(daily_range(), cancelled.get_token()), std::runtime_error);
  EXPECT_EQ(calls, 0);
  try {
    provider.read(daily_range(), {});
    FAIL() << "expected rejection";
  } catch (const std::runtime_error& error) {
    EXPECT_EQ(std::string(error.what()).find("private-fixture-token"), std::string::npos);
    EXPECT_NE(std::string(error.what()).find("daily-data entitlement"), std::string::npos);
  }
  provider.stop();
  EXPECT_THROW(provider.read(daily_range(), {}), std::logic_error);
  auto truncated = Json::parse(daily_response());
  const auto row = truncated["data"]["items"][0];
  truncated["data"]["items"] = Json::array();
  for (int i = 0; i < 2000; ++i)
    truncated["data"]["items"].push_back(row);
  tushare::Daily limited("fixture", [&](const auto&, auto) { return truncated.dump(); });
  limited.start();
  EXPECT_THROW(limited.read(daily_range(), {}), std::runtime_error);
  tushare::Daily oversized("fixture",
                           [](const auto&, auto) { return std::string(8 * 1024 * 1024 + 1, ' '); });
  oversized.start();
  EXPECT_THROW(oversized.read(daily_range(), {}), std::runtime_error);
  std::stop_source after;
  tushare::Daily interrupted("fixture", [&](const auto&, auto) {
    after.request_stop();
    return daily_response();
  });
  interrupted.start();
  EXPECT_THROW(interrupted.read(daily_range(), after.get_token()), std::runtime_error);
}

TEST(TerminalDailyQueries, CatalogLifetimeUsesShanghaiDateAndStopsAtDelisting) {
  const tushare::FuturesListing item{
      "CU2403.SHF", "copper", "SHFE", "CU", "20230101", "20240315", {"SHFE", "cu", "2024-03"}};
  auto range = tushare::daily_contract_range(item, tushare::parse_time("2023-06-01 00:00:00"));
  EXPECT_EQ(format_trading_date(range.begin), "2023-01-01");
  EXPECT_EQ(format_trading_date(range.end), "2023-06-01");
  range = tushare::daily_contract_range(item, tushare::parse_time("2024-04-01 08:00:00"));
  EXPECT_EQ(format_trading_date(range.end), "2024-03-15");
  EXPECT_THROW(tushare::daily_contract_range(item, tushare::parse_time("2022-12-31 23:59:59")),
               std::invalid_argument);
}
