#include "tables.hpp"
#include "response_evidence.hpp"
#include "history_providers.hpp"
#include "table_update.hpp"
#include <asterion/protocol/data.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/domain/history_tables.hpp>
#include <gtest/gtest.h>
using namespace asterion;

namespace {

const HistoryTable& table(const char* id)
{
    return history_table(id);
}

std::vector<std::string> columns(const HistoryTable& table)
{
    std::vector<std::string> names;
    for (const auto& column : table.columns)
        names.emplace_back(column.name);
    return names;
}

Json answer(std::vector<std::string> fields, Json rows)
{
    return {{"code", 0}, {"data", {{"fields", std::move(fields)}, {"items", std::move(rows)}}}};
}

const std::vector<std::string> contract_fields{
    "ts_code",
    "exchange",
    "name",
    "fut_code",
    "multiplier",
    "trade_unit",
    "per_unit",
    "quote_unit",
    "quote_unit_desc",
    "d_mode_desc",
    "list_date",
    "delist_date",
    "d_month",
    "last_ddate",
    "trade_time_desc"
};

Json listing(
    const char* code,
    const char* exchange,
    const char* product,
    const char* month,
    const char* listed,
    const char* delisted)
{
    return {
        code,
        exchange,
        std::string(product) + month,
        product,
        nullptr,
        "吨",
        10,
        "人民币元/吨",
        "1元/吨",
        "实物交割",
        listed,
        delisted,
        month,
        nullptr,
        "上午9:00-11:30\n下午1:30-3:00"
    };
}

const std::vector<std::string> daily_fields{
    "ts_code",
    "trade_date",
    "pre_close",
    "pre_settle",
    "open",
    "high",
    "low",
    "close",
    "settle",
    "vol",
    "amount",
    "oi",
    "oi_chg",
    "delv_settle"
};
const std::vector<std::string> settlement_fields{
    "ts_code",
    "trade_date",
    "trading_fee_rate",
    "trading_fee",
    "delivery_fee",
    "b_hedging_margin_rate",
    "s_hedging_margin_rate",
    "long_margin_rate",
    "short_margin_rate",
    "offset_today_fee"
};
const std::vector<std::string> receipt_fields{
    "trade_date",
    "symbol",
    "fut_name",
    "warehouse",
    "wh_id",
    "pre_vol",
    "vol",
    "vol_chg",
    "area",
    "year",
    "grade",
    "brand",
    "place",
    "pd",
    "is_ct",
    "unit"
};
const std::vector<std::string> ranking_fields{
    "trade_date",
    "symbol",
    "broker",
    "vol",
    "vol_chg",
    "long_hld",
    "long_chg",
    "short_hld",
    "short_chg"
};
const std::vector<std::string> only_contract{"contract"};
// A stored row as readers are shown it.
using Cells = std::vector<std::optional<std::string>>;

Cells cells(const HistoryTable& table, const std::vector<TableCell>& row)
{
    Cells result;
    for (std::size_t c = 0; c < table.columns.size(); ++c)
        result.push_back(table_cell_text(table.columns[c], row[c]));
    return result;
}

std::string failure(tushare::Tables& tables, const char* venue, const char* day)
{
    try {
        (void)tables.read("futures.daily", columns(table("futures.daily")), venue, day, {});
    } catch (const std::exception& error) {
        EXPECT_TRUE(dynamic_cast<const tushare::TableError*>(&error)) << error.what();
        return error.what();
    }
    return {};
}

} // namespace

TEST(TushareTables, ADayOfAnExchangeIsOneRequestAndOnePartition)
{
    int catalogues = 0, days = 0;
    tushare::Tables tables("fixture-token", [&](const auto& body, auto) {
        const auto request = Json::parse(body);
        EXPECT_EQ(request.at("token"), "fixture-token");
        if (request.at("api_name") == "fut_basic") {
            ++catalogues;
            EXPECT_EQ(request.at("params"), (Json{{"exchange", "SHFE"}, {"fut_type", "1"}}));
            // The last one is a contract the identity cannot express: it is left out.
            return answer(
                contract_fields,
                {listing("RB2510.SHF", "SHFE", "RB", "202510", "20241016", "20251015"),
                    listing("RB2606.SHF", "SHFE", "RB", "202606", "20250617", "20260615"),
                    listing("RB2607.SHF", "SHFE", "RB", "202607", "20250716", "20260715"),
                    listing("RB2606F.SHF", "SHFE", "RB_F", "202606", "20250617", "20260615")})
                .dump();
        }
        ++days;
        EXPECT_EQ(request.at("api_name"), "fut_daily");
        EXPECT_EQ(request.at("params"), (Json{{"trade_date", "20250725"}, {"exchange", "SHFE"}}));
        EXPECT_EQ(
            request.at("fields"),
            "ts_code,trade_date,pre_close,pre_settle,open,high,low,close," "settle,vol,amount,oi,oi_chg,delv_settle");
        // As the vendor writes numbers: a main contract whose turnover is in the
        // hundreds of billions, a month nobody traded, and its own continuous series.
        return std::string(
            R"({"code":0,"data":{"fields":["ts_code","trade_date","pre_close","pre_settle","open","high","low","close","settle","vol","amount","oi","oi_chg","delv_settle"],"items":[)" R"(["RB2510.SHF","20250725",3274.0,3294.0,3290.0,3373.0,3284.0,3356.0,3342.0,3501234.0,1.175014130405e+07,2018654.0,-34567.0,null],)" R"(["RB2606.SHF","20250725",3301.0,3301.0,null,null,null,null,3318.0,0.0,0.0,12.0,0.0,null],)" R"(["RB2607.SHF","20250725",3305.0,3305.0,0.0,0.0,0.0,3320.0,3320.0,10.0,33.2,3.0,0.0,null],)" R"(["RB.SHF","20250725",3274.0,3294.0,3290.0,3373.0,3284.0,3356.0,3342.0,3501234.0,11750141.30405,2018654.0,-34567.0,null],)" R"(["RB2606F.SHF","20250725",3301.0,3301.0,3300.0,3300.0,3300.0,3300.0,3318.0,5.0,16.5,12.0,0.0,null],)" R"(["RBL.SHF","20250725",3274.0,3294.0,3290.0,3373.0,3284.0,3356.0,3342.0,3501234.0,11750141.30405,2018654.0,-34567.0,null]]}})");
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

TEST(TushareTables, ContractsAndCalendarKeepTheVendorsFieldsUnderOurNames)
{
    tushare::Tables tables("fixture-token", [](const auto& body, auto) {
        const auto request = Json::parse(body);
        if (request.at("api_name") == "fut_basic") {
            auto index = listing("IF2611.CFX", "CFFEX", "IF", "202611", "20260921", "20261120");
            index[4] = 300;
            index[13] = "20261120";
            return answer(contract_fields, {index}).dump();
        }
        EXPECT_EQ(request.at("api_name"), "trade_cal");
        EXPECT_EQ(
            request.at("params"),
            (Json{{"exchange", "CFFEX"}, {"start_date", "20260101"}, {"end_date", "20261231"}}));
        return answer(
            {"exchange", "cal_date", "is_open", "pretrade_date"},
            {{"CFFEX", "20261008", 1, "20260930"}, {"CFFEX", "20261010", 0, nullptr}})
            .dump();
    });
    const auto& contracts = table("futures.contracts");
    const auto listed = table_partition(
        contracts,
        "CFFEX",
        "all",
        tables.read("futures.contracts", columns(contracts), "CFFEX", "all", {}));
    ASSERT_EQ(listed.rows.size(), 1U);
    std::vector<std::optional<std::string>> cells;
    for (std::size_t c = 0; c < contracts.columns.size(); ++c)
        cells.push_back(table_cell_text(contracts.columns[c], listed.rows[0][c]));
    EXPECT_EQ(
        cells,
        (std::vector<std::optional<std::string>>{
            "CFFEX/if/2026-11",
            "IF2611.CFX",
            "IF202611",
            "2026-09-21",
            "2026-11-20",
            "2026-11-20",
            "300",
            "10",
            "吨",
            "人民币元/吨",
            "1元/吨",
            "实物交割",
            "上午9:00-11:30 下午1:30-3:00"
        }));
    const auto& calendar = table("futures.calendar");
    const auto year = table_partition(
        calendar,
        "CFFEX",
        "2026",
        tables.read("futures.calendar", columns(calendar), "CFFEX", "2026", {}));
    ASSERT_EQ(year.rows.size(), 2U);
    EXPECT_EQ(table_cell_text(calendar.columns[0], year.rows[0][0]), "2026-10-08");
    EXPECT_EQ(table_cell_text(calendar.columns[1], year.rows[0][1]), "1");
    EXPECT_EQ(table_cell_text(calendar.columns[2], year.rows[0][2]), "2026-09-30");
    EXPECT_EQ(table_cell_text(calendar.columns[1], year.rows[1][1]), "0");
    EXPECT_EQ(table_cell_text(calendar.columns[2], year.rows[1][2]), std::nullopt);
}

TEST(TushareTables, SettlementParametersAreThoseOfTheContractsListedThatDay)
{
    tushare::Tables tables("fixture-token", [](const auto& body, auto) {
        const auto request = Json::parse(body);
        if (request.at("api_name") == "fut_basic")
            return answer(
                contract_fields,
                {listing("IF2605.CFX", "CFFEX", "IF", "202605", "20250922", "20260515"),
                    listing("IF2612.CFX", "CFFEX", "IF", "202612", "20260420", "20261218")})
                .dump();
        EXPECT_EQ(request.at("api_name"), "fut_settle");
        EXPECT_EQ(request.at("params"), (Json{{"trade_date", "20261009"}, {"exchange", "CFFEX"}}));
        // As the vendor answers for CFFEX: percentages, one number as both rate
        // and amount, and a contract whose last day was in May.
        return answer(
            settlement_fields,
            {{"IF2605.CFX", "20261009", 0.23, 0.23, 0.5, 12.0, 12.0, 12.0, 12.0, 1000.0},
                {"IF2612.CFX", "20261009", 0.23, 0.23, 1.0, 12.0, 12.0, 14.0, 15.0, 1000.0}})
            .dump();
    });
    const auto& settlement = table("futures.settlement");
    const auto partition = table_partition(
        settlement,
        "CFFEX",
        "2026-10-09",
        tables.read("futures.settlement", columns(settlement), "CFFEX", "2026-10-09", {}));
    ASSERT_EQ(partition.rows.size(), 1U);
    EXPECT_EQ(
        cells(settlement, partition.rows[0]),
        (Cells{"CFFEX/if/2026-12", "0.23", "0.23", "1000", "1", "14", "15", "12", "12"}))
        << "in the units published, under the names of the table";
}

TEST(TushareTables, ACzceCodeNamesTheContractListedThatDayWhateverDecadeTheVendorWrote)
{
    Json day;
    tushare::Tables tables("fixture-token", [&](const auto& body, auto) {
        if (Json::parse(body).at("api_name") == "fut_basic")
            return answer(
                contract_fields,
                {listing("AP2003.ZCE", "CZCE", "AP", "202003", "20190315", "20200313"),
                    listing("CF1003.ZCE", "CZCE", "CF", "201003", "20090316", "20100312"),
                    listing("CF2003.ZCE", "CZCE", "CF", "202003", "20190315", "20200313"),
                    listing("CF1907.ZCE", "CZCE", "CF", "201907", "20180716", "20190712")})
                .dump();
        return answer(settlement_fields, day).dump();
    });
    const auto row = [](const char* code) {
        return Json::array({code, "20190617", nullptr, 4.3, 0, 0.07, 0.07, 0.07, 0.07, 0});
    };
    // The exchange calls them AP003 and CF003; in 2019 those are the months of 2020.
    day = {row("AP1003.ZCE"), row("CF1003.ZCE"), row("CF1907.ZCE")};
    const auto rows = tables.read("futures.settlement", only_contract, "CZCE", "2019-06-17", {});
    EXPECT_EQ(
        rows,
        (std::vector<tushare::TableRow>{
            {"CZCE/ap/2020-03"},
            {"CZCE/cf/2020-03"},
            {"CZCE/cf/2019-07"}
        }));
    day = {row("SR1003.ZCE")};
    try {
        (void)tables.read("futures.settlement", only_contract, "CZCE", "2019-06-17", {});
        FAIL() << "no contract of that product was listed";
    } catch (const tushare::TableError& error) {
        EXPECT_STREQ(
            error.what(),
            "futures.settlement CZCE 2019-06-17: SR1003.ZCE is not in the contract list");
    }
}

TEST(TushareTables, ReceiptsAreTheLinesOfTheReportInItsOrder)
{
    std::vector<Json> asked;
    tushare::Tables tables("fixture-token", [&](const auto& body, auto) {
        const auto request = Json::parse(body);
        const auto& exchange = request.at("params").at("exchange");
        if (request.at("api_name") == "fut_basic")
            return answer(
                contract_fields,
                exchange == "SHFE"
                    ? Json{listing("CU2611.SHF", "SHFE", "CU", "202611", "20251118", "20261116")}
                    : exchange == "INE"
                    ? Json{listing("BC2611.INE", "INE", "BC", "202611", "20251118", "20261116"), listing("SC2611.INE", "INE", "SC", "202611", "20231101", "20261030")}
                    : Json{listing("TA2611.ZCE", "CZCE", "TA", "202611", "20251118", "20261113")})
                .dump();
        EXPECT_EQ(request.at("api_name"), "fut_wsr");
        asked.push_back(request.at("params"));
        const auto line = [](const char* symbol, const char* name, const std::string& warehouse) {
            return Json::array(
                {"20261009",
                    symbol,
                    name,
                    warehouse,
                    nullptr,
                    nullptr,
                    300,
                    -5,
                    "上海",
                    "0",
                    nullptr,
                    nullptr,
                    nullptr,
                    nullptr,
                    nullptr,
                    "吨"});
        };
        if (exchange == "CZCE")
            return answer(
                receipt_fields,
                {line("PTA", "PTA", "逸盛大化"), line("WT", "硬麦", "郑州")})
                .dump();
        // The vendor answers for SHFE with the lines of INE among them, a page of
        // a thousand at a time; international copper comes under copper.
        Json lines = Json::array();
        if (request.at("params").at("offset") == 0) {
            for (int i = 0; i < 998; ++i)
                lines.push_back(line("CU", "铜", "仓库" + std::to_string(i)));
            lines.push_back(line("CU", "铜(BC)", "洋山"));
            lines.push_back(line("SC", "中质含硫原油", "中化兴中"));
        } else {
            lines.push_back(line("CU", "铜", "仓库0"));
            lines.push_back(line("CU", "铜(BC)", "外高桥"));
        }
        return answer(receipt_fields, lines).dump();
    });
    const auto& receipts = table("futures.receipts");
    const auto read = [&](const char* venue) {
        return table_partition(
            receipts,
            venue,
            "2026-10-09",
            tables.read("futures.receipts", columns(receipts), venue, "2026-10-09", {}));
    };
    const auto energy = read("INE");
    ASSERT_EQ(energy.rows.size(), 3U);
    EXPECT_EQ(
        cells(receipts, energy.rows[0]),
        (Cells{
            "bc",
            "1",
            "铜(BC)",
            "洋山",
            std::nullopt,
            "上海",
            "0",
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            "300",
            "-5",
            "吨",
            std::nullopt
        }));
    EXPECT_EQ(cells(receipts, energy.rows[1]).at(3), "外高桥");
    EXPECT_EQ(cells(receipts, energy.rows[2]).at(0), "sc");
    const auto shanghai = read("SHFE");
    ASSERT_EQ(shanghai.rows.size(), 999U);
    // Two lines of one warehouse that nothing tells apart are both kept, each at its place.
    EXPECT_EQ(cells(receipts, shanghai.rows[0]).at(3), "仓库0");
    EXPECT_EQ(cells(receipts, shanghai.rows[998]).at(1), "999");
    EXPECT_EQ(cells(receipts, shanghai.rows[998]).at(3), "仓库0");
    EXPECT_EQ(
        asked,
        (std::vector<Json>{
            {{"trade_date", "20261009"}, {"exchange", "SHFE"}, {"limit", 1000}, {"offset", 0}},
            {{"trade_date", "20261009"}, {"exchange", "SHFE"}, {"limit", 1000}, {"offset", 1000}}
        }))
        << "one answer serves both exchanges";
    // CZCE calls TA by its name; a product the exchange never listed is not guessed at.
    try {
        (void)read("CZCE");
        FAIL() << "WT is not in the contract list";
    } catch (const tushare::TableError& error) {
        EXPECT_STREQ(
            error.what(),
            "futures.receipts CZCE 2026-10-09: WT is not a product of this exchange");
    }
}

TEST(TushareTables, RankingsNameAContractOrAProductHoweverTheSourceWritesIt)
{
    Json day;
    tushare::Tables tables("fixture-token", [&](const auto& body, auto) {
        const auto request = Json::parse(body);
        const auto& exchange = request.at("params").at("exchange");
        if (request.at("api_name") == "fut_basic")
            return answer(
                contract_fields,
                exchange == "SHFE"
                    ? Json{listing("CU2611.SHF", "SHFE", "CU", "202611", "20251118", "20261116")}
                    : exchange == "INE"
                    ? Json{listing("BC2611.INE", "INE", "BC", "202611", "20251118", "20261116")}
                    : exchange == "CZCE"
                    ? Json{listing("UR2701.ZCE", "CZCE", "UR", "202701", "20260115", "20270114"), listing("TA2701.ZCE", "CZCE", "TA", "202701", "20260115", "20270114")}
                    : Json{listing("IF2612.CFX", "CFFEX", "IF", "202612", "20260420", "20261218")})
                .dump();
        EXPECT_EQ(request.at("api_name"), "fut_holding");
        EXPECT_EQ(request.at("params").at("limit"), 2000);
        return answer(ranking_fields, day).dump();
    });
    const auto rank = [](const char* symbol, const char* member) {
        return Json::array({"20261009", symbol, member, 1200, -30, 800, 10, nullptr, nullptr});
    };
    const auto& rankings = table("futures.rankings");
    const auto read = [&](const char* venue) {
        std::vector<Cells> rows;
        for (const auto& row : table_partition(
                 rankings,
                 venue,
                 "2026-10-09",
                 tables.read("futures.rankings", columns(rankings), venue, "2026-10-09", {}))
                 .rows)
            rows.push_back(cells(rankings, row));
        return rows;
    };
    // A product by its Chinese name and letters, by its name alone, and a
    // contract by the three digits the exchange itself writes, in lower case.
    day = {rank("尿素UR", "中信期货"), rank("PTA", "中信期货"), rank("ur701", "永安期货（代客）")};
    EXPECT_EQ(
        read("CZCE"),
        (std::vector<Cells>{
            {"CZCE/ta", "中信期货", "1200", "-30", "800", "10", std::nullopt, std::nullopt},
            {"CZCE/ur", "中信期货", "1200", "-30", "800", "10", std::nullopt, std::nullopt},
            {"CZCE/ur/2027-01",
                "永安期货（代客）",
                "1200",
                "-30",
                "800",
                "10",
                std::nullopt,
                std::nullopt}
        }));
    // Shanghai: a product over its active contracts or by a kind of member, and
    // the contracts of INE in the same answer.
    day = {
        rank("CUACTV", "中信期货"),
        rank("CU", "期货公司"),
        rank("CU2611", "中信期货"),
        rank("BC2611", "东证期货")
    };
    const auto shanghai = read("SHFE");
    ASSERT_EQ(shanghai.size(), 3U);
    EXPECT_EQ(shanghai[0].at(0), "SHFE/cu");
    EXPECT_EQ(shanghai[0].at(1), "中信期货");
    EXPECT_EQ(shanghai[1].at(1), "期货公司");
    EXPECT_EQ(shanghai[2].at(0), "SHFE/cu/2026-11");
    const auto energy = read("INE");
    ASSERT_EQ(energy.size(), 1U);
    EXPECT_EQ(energy[0].at(0), "INE/bc/2026-11");
    // Options are ranked with the futures and are not futures.
    day = {rank("IO2610", "中信期货"), rank("IF2612", "中信期货")};
    const auto index = read("CFFEX");
    ASSERT_EQ(index.size(), 1U);
    EXPECT_EQ(index[0].at(0), "CFFEX/if/2026-12");
    // A notation nobody has seen is not skipped: the day waits for a rule.
    day = {rank("IF2612", "中信期货"), rank("合计", "中信期货"), rank("IF26", "中信期货")};
    try {
        (void)read("CFFEX");
        FAIL() << "two symbols name nothing";
    } catch (const tushare::TableError& error) {
        EXPECT_STREQ(
            error.what(),
            "futures.rankings CFFEX 2026-10-09: IF26 合计 name neither a " "contract nor a product");
    }
}

TEST(TushareTables, FailuresSayWhichPartitionAndNeverRepeatTheVendor)
{
    std::string reply;
    tushare::Tables tables("fixture-token", [&](const auto& body, auto) {
        if (Json::parse(body).at("api_name") == "fut_basic")
            return answer(
                contract_fields,
                {listing("RB2510.SHF", "SHFE", "RB", "202510", "20241016", "20251015")})
                .dump();
        return reply;
    });
    const auto row = [](const char* code, const char* date) {
        return Json::array({code, date, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, nullptr});
    };
    reply = answer(daily_fields, {row("RB2512.SHF", "20250725")}).dump();
    EXPECT_EQ(
        failure(tables, "SHFE", "2025-07-25"),
        "futures.daily SHFE 2025-07-25: RB2512.SHF is not in the contract list");
    reply = answer(daily_fields, {row("RB2510.SHF", "20250724")}).dump();
    EXPECT_EQ(
        failure(tables, "SHFE", "2025-07-25"),
        "futures.daily SHFE 2025-07-25: RB2510.SHF belongs to another exchange or day");
    reply = answer(daily_fields, Json(2000, row("RB2510.SHF", "20250725"))).dump();
    EXPECT_EQ(
        failure(tables, "SHFE", "2025-07-25"),
        "futures.daily SHFE 2025-07-25: fut_daily returned its limit of 2000 rows, so the " "partition may be incomplete");
    reply = R"({"code":0,"data":{"fields":["ts_code"],"items":[["RB2510.SHF"]]}})";
    EXPECT_EQ(
        failure(tables, "SHFE", "2025-07-25"),
        "futures.daily SHFE 2025-07-25: the answer of fut_daily is not the expected table");
    reply = "<html>busy</html>";
    EXPECT_EQ(
        failure(tables, "SHFE", "2025-07-25"),
        "futures.daily SHFE 2025-07-25: the answer of fut_daily is not the expected table");
    // A refusal is not about this partition, and the vendor's words can carry the token.
    reply = R"({"code":2002,"msg":"抱歉，您没有访问该接口的权限 fixture-token","data":null})";
    try {
        (void)tables.read("futures.daily", only_contract, "SHFE", "2025-07-25", {});
        FAIL() << "a refused request has no rows";
    } catch (const tushare::RequestError& error) {
        EXPECT_EQ(error.reason, tushare::AccessFailure::permission);
        EXPECT_STREQ(
            error.what(),
            "the Tushare account has no permission for this interface: fut_daily");
    }
    EXPECT_NO_THROW(reply = answer(daily_fields, Json::array()).dump());
    EXPECT_TRUE(tables.read("futures.daily", only_contract, "SHFE", "2025-07-25", {}).empty())
        << "nothing published yet is not an error";
}

TEST(RawResponse, StrictAllowlistRejectsUnknownSensitiveAndDuplicateFieldsWithoutProjection)
{
    const std::string raw =
        R"raw({ "code":0, "msg":null, "data":{"fields":["ts_code","trade_date","pre_close","pre_settle","open","high","low","close","settle","vol","amount","oi","oi_chg","delv_settle"],"items":[["RB2510.SHF","20250725",100,100,100,102,99,101.25,101,1200,12.3456,3400,0,null]]}})raw";
    ASSERT_TRUE(tushare::allow_daily_response(raw, "SHFE", "2025-07-25"));
    auto root = Json::parse(raw);
    for (const auto* key : {"token", "cookie", "Authorization", "password"}) {
        auto bad = root;
        bad[key] = "must-not-retain";
        EXPECT_THROW(
            history_providers::validate_response(
                "futures.daily",
                "SHFE",
                "2025-07-25",
                SourceResponse{tushare::daily_response_format, bad.dump(), 1790000000000000000LL}),
            std::invalid_argument);
        EXPECT_FALSE(tushare::allow_daily_response(bad.dump(), "SHFE", "2025-07-25"));
    }
    auto message = root;
    message["msg"] = "credential echo must-not-retain";
    EXPECT_FALSE(tushare::allow_daily_response(message.dump(), "SHFE", "2025-07-25"));
    auto nested = root;
    nested["data"]["secret"] = "must-not-retain";
    EXPECT_FALSE(tushare::allow_daily_response(nested.dump(), "SHFE", "2025-07-25"));
    auto cell = root;
    cell["data"]["items"][0][7] = "Bearer must-not-retain";
    EXPECT_FALSE(tushare::allow_daily_response(cell.dump(), "SHFE", "2025-07-25"));
    auto field = root;
    field["data"]["fields"][7] = "token";
    EXPECT_FALSE(tushare::allow_daily_response(field.dump(), "SHFE", "2025-07-25"));
    auto duplicate = raw;
    duplicate.insert(duplicate.find("\"code\""), "\"msg\":\"hidden-secret\", ");
    EXPECT_FALSE(tushare::allow_daily_response(duplicate, "SHFE", "2025-07-25"));
    EXPECT_FALSE(tushare::allow_daily_response(raw, "DCE", "2025-07-25"));
    EXPECT_FALSE(tushare::allow_daily_response(raw, "SHFE", "2025-07-28"));
    EXPECT_FALSE(
        tushare::allow_daily_response(
            std::string(max_source_response_bytes + 1, 'x'),
            "SHFE",
            "2025-07-25"));
}

TEST(
    RawResponse,
    SourceCapturesExactSuccessfulBytesWithoutAnotherRequestAndNeverRetainsCredentialEcho)
{
    const auto payload = answer(
                             daily_fields,
                             {{"RB2510.SHF",
                                 "20250725",
                                 100,
                                 100,
                                 100,
                                 102,
                                 99,
                                 101.25,
                                 101,
                                 1200,
                                 12.3456,
                                 3400,
                                 0,
                                 nullptr}})
                             .dump(2)
        + "\n";
    int requests = 0;
    const auto post = [&](const std::string& body, auto) {
        ++requests;
        const auto request = Json::parse(body);
        if (request.at("api_name") == "fut_basic")
            return answer(
                contract_fields,
                {listing("RB2510.SHF", "SHFE", "RB", "202510", "20241016", "20251015")})
                .dump();
        return payload;
    };
    tushare::Tables reader("fixture-token", post);
    const auto rows =
        reader.read("futures.daily", columns(table("futures.daily")), "SHFE", "2025-07-25", {});
    ASSERT_EQ(rows.size(), 1U);
    ASSERT_TRUE(reader.response());
    EXPECT_EQ(reader.response()->body, payload);
    EXPECT_GT(reader.response()->received_at_ns, 0);
    EXPECT_EQ(requests, 2);
    EXPECT_EQ(reader.response()->format, tushare::daily_response_format);
    EXPECT_EQ(requests, 2);
    const std::string credential(32, 'a');
    auto echoed = Json::parse(payload);
    echoed["request_id"] = credential;
    ASSERT_TRUE(tushare::allow_daily_response(echoed.dump(), "SHFE", "2025-07-25"));
    tushare::Tables protected_reader(credential, [&](const std::string& body, auto) {
        const auto request = Json::parse(body);
        if (request.at("api_name") == "fut_basic")
            return answer(
                contract_fields,
                {listing("RB2510.SHF", "SHFE", "RB", "202510", "20241016", "20251015")})
                .dump();
        return echoed.dump();
    });
    EXPECT_EQ(
        protected_reader
            .read("futures.daily", columns(table("futures.daily")), "SHFE", "2025-07-25", {})
            .size(),
        1U);
    EXPECT_FALSE(protected_reader.response());
}

TEST(RawResponse, MalformedIdentifiersNeverRepeatTheCredentialInFailureText)
{
    const std::string credential(32, 'a');
    const auto payload = answer(
        daily_fields,
        {{credential, "20250725", 100, 100, 100, 102, 99, 101, 101, 1200, 12, 3400, 0, nullptr}})
                             .dump();
    tushare::Tables reader(credential, [&](const std::string& body, auto) {
        const auto request = Json::parse(body);
        if (request.at("api_name") == "fut_basic")
            return answer(
                contract_fields,
                {listing("RB2510.SHF", "SHFE", "RB", "202510", "20241016", "20251015")})
                .dump();
        return payload;
    });
    std::string message;
    try {
        reader.read("futures.daily", columns(table("futures.daily")), "SHFE", "2025-07-25", {});
    } catch (const tushare::TableError& error) {
        message = error.what();
    }
    EXPECT_FALSE(message.empty());
    EXPECT_EQ(message.find(credential), std::string::npos);
    EXPECT_FALSE(reader.response());
}

namespace {

Json review_row(const std::string& code = "CU2403.SHF")
{
    return Json::array(
        {code,
            "20240301",
            nullptr,
            nullptr,
            0,
            nullptr,
            nullptr,
            "72000.50000000",
            "71990",
            "1.2e3",
            "43200000.000025",
            "3400",
            "0",
            nullptr});
}

TablePartition review_partition()
{
    return table_partition(
        history_table("futures.daily"),
        "SHFE",
        "2024-03-01",
        {{"SHFE/cu/2024-03",
            {},
            {},
            {},
            "72000.5",
            "71990",
            {},
            {},
            "1200",
            "432000000000.25",
            "3400",
            "0",
            {}}});
}

SourceResponse review_response(Json rows)
{
    return {
        tushare::daily_response_format,
        answer(daily_fields, std::move(rows)).dump(2) + "\n",
        1
    };
}

} // namespace

TEST(ResponseReview, ExactLexicalNumbersUnitsNullsFieldsAndExcludedRowsAreExplainedWithoutWrites)
{
    auto partition = review_partition();
    auto response = review_response(Json::array({review_row(), review_row("CU.SHF")}));
    auto body = Json::parse(response.body);
    std::reverse(body["data"]["fields"].begin(), body["data"]["fields"].end());
    for (auto& row : body["data"]["items"])
        std::reverse(row.begin(), row.end());
    response.body = body.dump(2) + "\n";
    const auto original_body = response.body;
    const auto original_rows = partition.rows;
    const auto result = tushare::review_daily_response(response, partition);
    EXPECT_EQ(result.status, "consistent");
    EXPECT_EQ(result.response_fields, 14);
    EXPECT_EQ(result.response_rows, 2);
    EXPECT_EQ(result.dated_rows, 1);
    EXPECT_EQ(result.normalized_rows, 1);
    EXPECT_EQ(result.matched_rows, 1);
    EXPECT_EQ(result.compared_cells, 12);
    EXPECT_EQ(result.skipped_keys, (std::vector<std::string>{"CU.SHF"}));
    EXPECT_TRUE(result.differences.empty());
    EXPECT_EQ(response.body, original_body);
    EXPECT_EQ(partition.rows, original_rows);
}

TEST(ResponseReview, ReportsConcreteDuplicateMissingPrecisionAndValueDifferences)
{
    auto partition = review_partition();
    auto extra = partition.rows.front();
    extra[0] = std::string("SHFE/rb/2024-03");
    partition.rows.push_back(extra);
    auto row = review_row();
    row[7] = "72000.500000001"; // nine significant fractional digits: never round
    row[9] = "1.5"; // quantities have scale zero
    row[10] = "43200000.000026"; // expected 432000000000.26, stored .25
    row[11] = nullptr; // missing is not 3400
    const auto response = review_response(Json::array({row, row, review_row("ZN2403.SHF")}));
    const auto result = tushare::review_daily_response(response, partition);
    EXPECT_EQ(result.status, "differences");
    std::set<std::pair<std::string, std::string>> kinds;
    for (const auto& issue : result.differences) {
        kinds.emplace(issue.kind, issue.field);
        if (issue.field == "amount") {
            EXPECT_EQ(issue.raw, "43200000.000026");
            EXPECT_EQ(issue.expected, "432000000000.26");
            EXPECT_EQ(issue.actual, "432000000000.25");
        }
    }
    EXPECT_EQ(
        kinds,
        (std::set<std::pair<std::string, std::string>>{
            {"duplicate_response_key", "ts_code"},
            {"response_key_without_partition", "contract"},
            {"partition_key_without_response", "contract"},
            {"not_exactly_representable", "close"},
            {"not_exactly_representable", "volume"},
            {"value_mismatch", "amount"},
            {"value_mismatch", "open_interest"}
        }));
    EXPECT_EQ(result.differences.size(), 7);
}

TEST(ResponseReview, MissingUnsafeAndAmbiguousEvidenceRemainUnknownWithoutEchoOrInference)
{
    auto partition = review_partition();
    EXPECT_EQ(tushare::review_daily_response({}, partition).reason, "raw_missing");
    auto response = review_response(Json::array({review_row()}));
    auto unsafe = Json::parse(response.body);
    unsafe["data"]["fields"][2] = "Authorization";
    response.body = unsafe.dump();
    auto unknown = tushare::review_daily_response(response, partition);
    EXPECT_EQ(unknown.status, "unknown");
    EXPECT_EQ(unknown.reason, "unsupported_response_shape");
    EXPECT_TRUE(unknown.differences.empty());
    response = review_response(Json::array({review_row()}));
    auto century = partition.rows.front();
    century[0] = std::string("SHFE/cu/2124-03");
    partition.rows.push_back(century);
    unknown = tushare::review_daily_response(response, partition);
    EXPECT_EQ(unknown.status, "unknown");
    EXPECT_EQ(unknown.reason, "key_mapping_unverified");
    EXPECT_EQ(unknown.unknown_keys, (std::vector<std::string>{"CU2403.SHF"}));
    EXPECT_EQ(unknown.compared_cells, 0);
    // The retained format admits a short code, but without a catalogue its
    // correspondence cannot be resolved. Never claim the stored key is absent.
    partition = review_partition();
    response = review_response(Json::array({review_row("CU403.SHF")}));
    unknown = tushare::review_daily_response(response, partition);
    EXPECT_EQ(unknown.status, "unknown");
    EXPECT_TRUE(unknown.differences.empty());
    EXPECT_EQ(unknown.unknown_keys, (std::vector<std::string>{"CU403.SHF", "CU2403.SHF"}));
}

TEST(ResponseInventory, ProductionTushareReaderPreservesZeroAndMissingThroughTypedStoreAndQuery)
{
    struct Temporary
    {
        std::filesystem::path path =
            std::filesystem::temp_directory_path() / ("asterion-zero-chain-" + unique_process_id());

        ~Temporary()
        {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    } temporary;

    history_files::TableStore store(temporary.path);
    const auto& daily = history_table("futures.daily");
    const history_files::TableSeries series{"futures.daily", daily.schema, "tushare.fut_daily.v3"};
    std::string payload;
    int requests = 0;
    tushare::Tables reader("synthetic-regression-only", [&](const std::string& body, auto) {
        ++requests;
        const auto request = Json::parse(body);
        if (request.at("api_name") == "fut_basic")
            return answer(
                contract_fields,
                {listing("CU2403.SHF", "SHFE", "CU", "202403", "20230316", "20240315")})
                .dump();
        return payload;
    });
    std::optional<history_files::TableRevision> zero_revision;
    for (const bool missing : {false, true}) {
        auto source_row = review_row();
        source_row[12] = missing ? Json() : Json(0);
        payload = review_response(Json::array({source_row})).body;
        const auto source = reader.read("futures.daily", columns(daily), "SHFE", "2024-03-01", {});
        ASSERT_EQ(source.size(), 1);
        const auto c = table_column(daily, "open_interest_change");
        const auto expected =
            missing ? std::optional<std::string>() : std::optional<std::string>("0");
        EXPECT_EQ(source[0][c], expected);
        const auto partition = table_partition(daily, "SHFE", "2024-03-01", source);
        EXPECT_EQ(table_cell_text(daily.columns[c], partition.rows[0][c]), expected);
        ASSERT_TRUE(reader.response());
        const auto held = store
                              .record(
                                  partition,
                                  series,
                                  store.write(partition),
                                  std::string(64, 'a'),
                                  1,
                                  reader.response())
                              .revision;
        data::proto::TablePageQuery query;
        query.mutable_partition()->set_table("futures.daily");
        query.mutable_partition()->set_exchange("SHFE");
        query.mutable_partition()->set_period("2024-03-01");
        query.set_source(series.source);
        query.set_limit(1);
        const auto page = protocol::decode_table_page(data::table_page(store, query));
        EXPECT_EQ(page.at("rows")[0][c], missing ? Json() : Json("0"));
        EXPECT_EQ(page.at("response_review").at("status"), "consistent");
        EXPECT_TRUE(page.at("response_review").at("differences").empty());
        EXPECT_EQ(store.response(held)->body, payload);
        if (!missing)
            zero_revision = held;
    }
    ASSERT_TRUE(zero_revision);
    EXPECT_EQ(
        table_cell_text(
            daily.columns[table_column(daily, "open_interest_change")],
            store.rows(*zero_revision)[0][table_column(daily, "open_interest_change")]),
        "0");
    EXPECT_EQ(requests, 3); // one catalogue and two mock daily bodies, no network
}
