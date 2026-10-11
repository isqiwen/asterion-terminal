#include "tables.hpp"
#include <asterion/domain/history_tables.hpp>
#include <gtest/gtest.h>
using namespace asterion;
namespace {
const HistoryTable& table(const char* id) {
  return history_table(id);
}
std::vector<std::string> columns(const HistoryTable& table) {
  std::vector<std::string> names;
  for (const auto& column : table.columns)
    names.emplace_back(column.name);
  return names;
}
Json answer(std::vector<std::string> fields, Json rows) {
  return {{"code", 0}, {"data", {{"fields", std::move(fields)}, {"items", std::move(rows)}}}};
}
const std::vector<std::string> contract_fields{
    "ts_code",    "exchange",    "name",       "fut_code",        "multiplier",
    "trade_unit", "per_unit",    "quote_unit", "quote_unit_desc", "d_mode_desc",
    "list_date",  "delist_date", "d_month",    "last_ddate",      "trade_time_desc"};
Json listing(const char* code, const char* exchange, const char* product, const char* month,
             const char* listed, const char* delisted) {
  return {code,       exchange,      std::string(product) + month,
          product,    nullptr,       "吨",
          10,         "人民币元/吨", "1元/吨",
          "实物交割", listed,        delisted,
          month,      nullptr,       "上午9:00-11:30\n下午1:30-3:00"};
}
const std::vector<std::string> daily_fields{
    "ts_code", "trade_date", "pre_close", "pre_settle", "open", "high",   "low",
    "close",   "settle",     "vol",       "amount",     "oi",   "oi_chg", "delv_settle"};
const std::vector<std::string> only_contract{"contract"};
std::string failure(tushare::Tables& tables, const char* venue, const char* day) {
  try {
    (void)tables.read("futures.daily", columns(table("futures.daily")), venue, day, {});
  } catch (const std::exception& error) {
    EXPECT_TRUE(dynamic_cast<const tushare::TableError*>(&error)) << error.what();
    return error.what();
  }
  return {};
}
} // namespace
TEST(TushareTables, ADayOfAnExchangeIsOneRequestAndOnePartition) {
  int catalogues = 0, days = 0;
  tushare::Tables tables("fixture-token", [&](const auto& body, auto) {
    const auto request = Json::parse(body);
    EXPECT_EQ(request.at("token"), "fixture-token");
    if (request.at("api_name") == "fut_basic") {
      ++catalogues;
      EXPECT_EQ(request.at("params"), (Json{{"exchange", "SHFE"}, {"fut_type", "1"}}));
      // The last one is a contract the identity cannot express: it is left out.
      return answer(contract_fields,
                    {listing("RB2510.SHF", "SHFE", "RB", "202510", "20241016", "20251015"),
                     listing("RB2606.SHF", "SHFE", "RB", "202606", "20250617", "20260615"),
                     listing("RB2607.SHF", "SHFE", "RB", "202607", "20250716", "20260715"),
                     listing("RB2606F.SHF", "SHFE", "RB_F", "202606", "20250617", "20260615")})
          .dump();
    }
    ++days;
    EXPECT_EQ(request.at("api_name"), "fut_daily");
    EXPECT_EQ(request.at("params"), (Json{{"trade_date", "20250725"}, {"exchange", "SHFE"}}));
    EXPECT_EQ(request.at("fields"), "ts_code,trade_date,pre_close,pre_settle,open,high,low,close,"
                                    "settle,vol,amount,oi,oi_chg,delv_settle");
    // As the vendor writes numbers: a main contract whose turnover is in the
    // hundreds of billions, a month nobody traded, and its own continuous series.
    return std::string(
        R"({"code":0,"data":{"fields":["ts_code","trade_date","pre_close","pre_settle","open","high","low","close","settle","vol","amount","oi","oi_chg","delv_settle"],"items":[)"
        R"(["RB2510.SHF","20250725",3274.0,3294.0,3290.0,3373.0,3284.0,3356.0,3342.0,3501234.0,1.175014130405e+07,2018654.0,-34567.0,null],)"
        R"(["RB2606.SHF","20250725",3301.0,3301.0,null,null,null,null,3318.0,0.0,0.0,12.0,0.0,null],)"
        R"(["RB2607.SHF","20250725",3305.0,3305.0,0.0,0.0,0.0,3320.0,3320.0,10.0,33.2,3.0,0.0,null],)"
        R"(["RB.SHF","20250725",3274.0,3294.0,3290.0,3373.0,3284.0,3356.0,3342.0,3501234.0,11750141.30405,2018654.0,-34567.0,null],)"
        R"(["RB2606F.SHF","20250725",3301.0,3301.0,3300.0,3300.0,3300.0,3300.0,3318.0,5.0,16.5,12.0,0.0,null],)"
        R"(["RBL.SHF","20250725",3274.0,3294.0,3290.0,3373.0,3284.0,3356.0,3342.0,3501234.0,11750141.30405,2018654.0,-34567.0,null]]}})");
  });
  const auto& daily = table("futures.daily");
  const auto rows = tables.read("futures.daily", columns(daily), "SHFE", "2025-07-25", {});
  ASSERT_EQ(rows.size(), 3U) << "only codes of letters and a delivery month are contracts";
  EXPECT_EQ(tables.read("futures.contracts", only_contract, "SHFE", "all", {}).size(), 3U);
  // What the plugin delivers is what the host stores, without loss.
  const auto partition = table_partition(daily, "SHFE", "2025-07-25", rows);
  const auto cell = [&](std::size_t row, std::string_view name) {
    for (std::size_t c = 0; c < daily.columns.size(); ++c)
      if (daily.columns[c].name == name)
        return table_cell_text(daily.columns[c], partition.rows[row][c]);
    throw std::logic_error("no such column");
  };
  EXPECT_EQ(cell(0, "contract"), "SHFE/rb/2025-10");
  EXPECT_EQ(cell(0, "amount"), "117501413040.5") << "ten thousands of yuan, exactly";
  EXPECT_EQ(cell(0, "volume"), "3501234");
  EXPECT_EQ(cell(0, "settlement"), "3342");
  EXPECT_EQ(cell(0, "open_interest_change"), "-34567");
  EXPECT_EQ(cell(0, "delivery_settlement"), std::nullopt);
  EXPECT_EQ(cell(1, "contract"), "SHFE/rb/2026-06");
  EXPECT_EQ(cell(1, "open"), std::nullopt) << "no trade: no price, not a zero";
  EXPECT_EQ(cell(1, "close"), std::nullopt);
  EXPECT_EQ(cell(1, "settlement"), "3318");
  EXPECT_EQ(cell(1, "volume"), "0");
  // Some exchanges reach us with zeros where others have nulls: the same
  // absence, also when a transfer outside continuous matching gave a volume.
  EXPECT_EQ(cell(2, "contract"), "SHFE/rb/2026-07");
  EXPECT_EQ(cell(2, "open"), std::nullopt);
  EXPECT_EQ(cell(2, "low"), std::nullopt);
  EXPECT_EQ(cell(2, "close"), "3320") << "published without a matched trade, and kept";
  EXPECT_EQ(cell(2, "volume"), "10");
  EXPECT_EQ(cell(2, "amount"), "332000");
  (void)tables.read("futures.daily", columns(daily), "SHFE", "2025-07-25", {});
  EXPECT_EQ(catalogues, 1) << "the contract list is asked for once";
  EXPECT_EQ(days, 2);
}
TEST(TushareTables, ContractsAndCalendarKeepTheVendorsFieldsUnderOurNames) {
  tushare::Tables tables("fixture-token", [](const auto& body, auto) {
    const auto request = Json::parse(body);
    if (request.at("api_name") == "fut_basic") {
      auto index = listing("IF2611.CFX", "CFFEX", "IF", "202611", "20260921", "20261120");
      index[4] = 300;
      index[13] = "20261120";
      return answer(contract_fields, {index}).dump();
    }
    EXPECT_EQ(request.at("api_name"), "trade_cal");
    EXPECT_EQ(request.at("params"),
              (Json{{"exchange", "CFFEX"}, {"start_date", "20260101"}, {"end_date", "20261231"}}));
    return answer({"exchange", "cal_date", "is_open", "pretrade_date"},
                  {{"CFFEX", "20261008", 1, "20260930"}, {"CFFEX", "20261010", 0, nullptr}})
        .dump();
  });
  const auto& contracts = table("futures.contracts");
  const auto listed =
      table_partition(contracts, "CFFEX", "all",
                      tables.read("futures.contracts", columns(contracts), "CFFEX", "all", {}));
  ASSERT_EQ(listed.rows.size(), 1U);
  std::vector<std::optional<std::string>> cells;
  for (std::size_t c = 0; c < contracts.columns.size(); ++c)
    cells.push_back(table_cell_text(contracts.columns[c], listed.rows[0][c]));
  EXPECT_EQ(cells, (std::vector<std::optional<std::string>>{
                       "CFFEX/if/2026-11", "IF2611.CFX", "IF202611", "2026-09-21", "2026-11-20",
                       "2026-11-20", "300", "10", "吨", "人民币元/吨", "1元/吨", "实物交割",
                       "上午9:00-11:30 下午1:30-3:00"}));
  const auto& calendar = table("futures.calendar");
  const auto year =
      table_partition(calendar, "CFFEX", "2026",
                      tables.read("futures.calendar", columns(calendar), "CFFEX", "2026", {}));
  ASSERT_EQ(year.rows.size(), 2U);
  EXPECT_EQ(table_cell_text(calendar.columns[0], year.rows[0][0]), "2026-10-08");
  EXPECT_EQ(table_cell_text(calendar.columns[1], year.rows[0][1]), "1");
  EXPECT_EQ(table_cell_text(calendar.columns[2], year.rows[0][2]), "2026-09-30");
  EXPECT_EQ(table_cell_text(calendar.columns[1], year.rows[1][1]), "0");
  EXPECT_EQ(table_cell_text(calendar.columns[2], year.rows[1][2]), std::nullopt);
}
TEST(TushareTables, ACodeAnExchangeReusesNamesTheContractListedThatDay) {
  Json day;
  tushare::Tables tables("fixture-token", [&](const auto& body, auto) {
    if (Json::parse(body).at("api_name") == "fut_basic")
      return answer(contract_fields,
                    {listing("TA709.ZCE", "CZCE", "TA", "201709", "20160919", "20170914"),
                     listing("TA709.ZCE", "CZCE", "TA", "202709", "20260915", "20270914"),
                     listing("TA701.ZCE", "CZCE", "TA", "202701", "20260115", "20270114")})
          .dump();
    return answer(daily_fields, day).dump();
  });
  const auto contract = [&](const char* code, const char* date, const char* period) {
    day = Json::array({{code, date, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, nullptr}});
    return tables.read("futures.daily", only_contract, "CZCE", period, {}).at(0).at(0);
  };
  EXPECT_EQ(contract("TA709.ZCE", "20261009", "2026-10-09"), "CZCE/ta/2027-09");
  EXPECT_EQ(contract("TA709.ZCE", "20170301", "2017-03-01"), "CZCE/ta/2017-09");
  // One listing under a code is that contract whatever the catalogue's dates say.
  EXPECT_EQ(contract("TA701.ZCE", "20270115", "2027-01-15"), "CZCE/ta/2027-01");
  day = Json::array({{"TA709.ZCE", "20200102", 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, nullptr}});
  EXPECT_EQ(
      failure(tables, "CZCE", "2020-01-02"),
      "futures.daily CZCE 2020-01-02: TA709.ZCE does not name one contract listed on that day");
}
TEST(TushareTables, FailuresSayWhichPartitionAndNeverRepeatTheVendor) {
  std::string reply;
  tushare::Tables tables("fixture-token", [&](const auto& body, auto) {
    if (Json::parse(body).at("api_name") == "fut_basic")
      return answer(contract_fields,
                    {listing("RB2510.SHF", "SHFE", "RB", "202510", "20241016", "20251015")})
          .dump();
    return reply;
  });
  const auto row = [](const char* code, const char* date) {
    return Json::array({code, date, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, nullptr});
  };
  reply = answer(daily_fields, {row("RB2512.SHF", "20250725")}).dump();
  EXPECT_EQ(failure(tables, "SHFE", "2025-07-25"),
            "futures.daily SHFE 2025-07-25: RB2512.SHF is not in the contract list");
  reply = answer(daily_fields, {row("RB2510.SHF", "20250724")}).dump();
  EXPECT_EQ(failure(tables, "SHFE", "2025-07-25"),
            "futures.daily SHFE 2025-07-25: RB2510.SHF belongs to another exchange or day");
  reply = answer(daily_fields, Json(2000, row("RB2510.SHF", "20250725"))).dump();
  EXPECT_EQ(failure(tables, "SHFE", "2025-07-25"),
            "futures.daily SHFE 2025-07-25: fut_daily returned its limit of 2000 rows, so the "
            "partition may be incomplete");
  reply = R"({"code":0,"data":{"fields":["ts_code"],"items":[["RB2510.SHF"]]}})";
  EXPECT_EQ(failure(tables, "SHFE", "2025-07-25"),
            "futures.daily SHFE 2025-07-25: the answer of fut_daily is not the expected table");
  reply = "<html>busy</html>";
  EXPECT_EQ(failure(tables, "SHFE", "2025-07-25"),
            "futures.daily SHFE 2025-07-25: the answer of fut_daily is not the expected table");
  // A refusal is not about this partition, and the vendor's words can carry the token.
  reply = R"({"code":2002,"msg":"抱歉，您没有访问该接口的权限 fixture-token","data":null})";
  try {
    (void)tables.read("futures.daily", only_contract, "SHFE", "2025-07-25", {});
    FAIL() << "a refused request has no rows";
  } catch (const tushare::RequestError& error) {
    EXPECT_EQ(error.reason, tushare::AccessFailure::permission);
    EXPECT_STREQ(error.what(),
                 "the Tushare account has no permission for this interface: fut_daily");
  }
  EXPECT_NO_THROW(reply = answer(daily_fields, Json::array()).dump());
  EXPECT_TRUE(tables.read("futures.daily", only_contract, "SHFE", "2025-07-25", {}).empty())
      << "nothing published yet is not an error";
}
