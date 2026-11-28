#include "composition.hpp"
#include <asterion/protocol/warehouse.hpp>
#include "acquisition/exchanges.hpp"
#include "acquisition/manager.hpp"
#include "acquisition/values.hpp"
#include <asterion/system/durable_file.hpp>
#include <asterion/system/process/child.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <future>
#include <sstream>
#include <thread>

namespace {

using namespace asterion;
using namespace std::chrono_literals;
namespace ac = asterion::warehouse::acquisition;
namespace wh = asterion::warehouse;
namespace fs = std::filesystem;

const std::string shfe_daily = R"({"report_date":"20261030","o_code":"0000","o_curinstrument":[
 {"PRODUCTGROUPID":"rb", "DELIVERYMONTH":"2701", "OPENPRICE":"0", "HIGHESTPRICE":"", "LOWESTPRICE":0,
 "CLOSEPRICE":3400, "SETTLEMENTPRICE":3399.123456, "PRESETTLEMENTPRICE":3380,
 "VOLUME":12345678901234567890, "OPENINTEREST":2000},
 {"PRODUCTGROUPID":"rb", "DELIVERYMONTH":"小计"},
 {"PRODUCTGROUPID":"rb_tas", "DELIVERYMONTH":"2701"},
 {"PRODUCTGROUPID":"rb_efp", "DELIVERYMONTH":"2701"},
 {"PRODUCTID":"rb_tas", "DELIVERYMONTH":"2701"}]})";
const std::string shfe_settlement = R"({"report_date":"20261030","o_code":"0000","o_cursor":[
 {"INSTRUMENTID":"rb2701", "SPECLONGMARGINRATIO":0.15,"SPECSHORTMARGINRATIO":0.16,
 "TRADEFEERATIO":0.05,"TRADEFEEUNIT":0,"TTRADEFEERATIO":0.025,"TTRADEFEEUNIT":0}]})";
const std::string czce_settlement =
    "郑州商品交易所期货结算参数表(2026-10-30)\n" "合约代码|交易保证金率(%)|手续费收取方式|交易手续费|日内平今仓交易手续费\n" "MA701|8|比例值|0.50|1.25\n" "AP611|20|绝对值|5.00|10.00\n";

std::string reference_fixture(const char* name = "czce_reference_20220104.xml")
{
    std::ifstream file(fs::path(ASTERION_EXCHANGE_FIXTURES) / name, std::ios::binary);
    if (!file)
        throw std::runtime_error("missing exchange reference fixture");
    return {std::istreambuf_iterator<char>(file), {}};
}

class ExchangeSources : public testing::Test
{
protected:
    void TearDown() override
    {
        std::error_code error;
        fs::remove_all(root, error);
    }

    fs::path root = fs::temp_directory_path() / ("asterion-exchange-" + unique_process_id());
    std::shared_ptr<ac::Budget> budget = std::make_shared<ac::Budget>(500);
};

TEST_F(ExchangeSources, ShanghaiInventoryKeepsWarehouseDetailsAndGoldStock)
{
    ac::Exchange source("SHFE", budget, {}, [&](const ac::HttpRequest& request, auto) {
        EXPECT_EQ(request.origin, "https://www.shfe.com.cn");
        EXPECT_EQ(request.path, "/data/tradedata/future/weeklydata/20140523weeklystock.dat");
        return ac::HttpReply{200, reference_fixture("shfe_inventory_20140523.json")};
    });
    const auto before = ac::observation_time();
    const auto rows = source.inventory("SHFE", "2014-05-23");
    ASSERT_EQ(rows.size(), 4);
    const auto& copper = rows[0];
    EXPECT_EQ(copper.product, "cu");
    EXPECT_EQ(copper.area, wh::Cell{std::string("上海")});
    EXPECT_EQ(copper.warehouse, wh::Cell{std::string("期晟公司")});
    EXPECT_EQ(copper.unit, wh::Cell{std::string("吨")});
    EXPECT_EQ(copper.previous_total, wh::Cell{std::string("5563")});
    EXPECT_EQ(copper.previous_receipts, wh::Cell{std::string("1919")});
    EXPECT_EQ(copper.total, wh::Cell{std::string("3959")});
    EXPECT_EQ(copper.receipts, wh::Cell{std::string("199")});
    EXPECT_EQ(copper.total_change, wh::Cell{std::string("-1604")});
    EXPECT_EQ(copper.receipts_change, wh::Cell{std::string("-1720")});
    EXPECT_EQ(copper.previous_capacity, wh::Cell{std::string("58081")});
    EXPECT_EQ(copper.capacity, wh::Cell{std::string("59801")});
    EXPECT_EQ(copper.capacity_change, wh::Cell{std::string("1720")});
    EXPECT_EQ(rows[1].product, "bu");
    EXPECT_EQ(rows[2].product, "bu");
    EXPECT_EQ(rows[1].warehouse, rows[2].warehouse);
    EXPECT_EQ(rows[1].warehouse_type, wh::Cell{std::string("仓库")});
    EXPECT_EQ(rows[2].warehouse_type, wh::Cell{std::string("厂库")});
    EXPECT_EQ(rows[2].total, wh::Cell{});
    const auto& gold = rows[3];
    EXPECT_EQ(gold.product, "au");
    EXPECT_EQ(gold.warehouse, wh::Cell{std::string("总计")});
    EXPECT_EQ(gold.unit, wh::Cell{std::string("千克")});
    EXPECT_EQ(gold.previous_total, wh::Cell{std::string("426")});
    EXPECT_EQ(gold.total, wh::Cell{std::string("420")});
    EXPECT_EQ(gold.total_change, wh::Cell{std::string("-6")});
    EXPECT_EQ(gold.previous_receipts, gold.previous_total);
    EXPECT_EQ(gold.receipts, gold.total);
    EXPECT_EQ(gold.receipts_change, gold.total_change);
    EXPECT_EQ(gold.capacity, wh::Cell{});
    for (const auto& row : rows) {
        EXPECT_EQ(row.exchange, "SHFE");
        EXPECT_EQ(row.day, "2014-05-23");
        EXPECT_EQ(row.published_ms, wh::Cell{});
        EXPECT_GE(row.observed_ms, before);
    }
}

TEST_F(ExchangeSources, ShanghaiInventoryReadsArchivedWeeklySpreadsheetWhenJsonIsMissing)
{
    ac::Exchange source("SHFE", budget, {}, [&](const ac::HttpRequest& request, auto) {
        if (request.path.ends_with(".dat"))
            return ac::HttpReply{404, {}};
        EXPECT_EQ(request.path, "/data/tradedata/future/weeklydata/20140103weeklystock.html");
        return ac::HttpReply{200, reference_fixture("shfe_inventory_20140103.html")};
    });
    const auto rows = source.inventory("SHFE", "2014-01-03");
    ASSERT_EQ(rows.size(), 7);
    EXPECT_EQ(rows[1].area, wh::Cell{std::string("上海")});
    EXPECT_EQ(rows[1].warehouse, wh::Cell{std::string("国储天威")});
    EXPECT_EQ(rows[1].total, wh::Cell{std::string("1609")});
    EXPECT_EQ(rows[3].warehouse, wh::Cell{std::string("海口港")});
    EXPECT_EQ(rows[3].area, wh::Cell{std::string("海南")});
    EXPECT_EQ(rows[5].product, "au");
    EXPECT_EQ(rows[5].receipts, wh::Cell{std::string("1143")});
    EXPECT_EQ(rows[6].warehouse_type, wh::Cell{std::string("厂库")});
    EXPECT_EQ(rows[6].total, wh::Cell{});
    EXPECT_EQ(rows[6].capacity, wh::Cell{std::string("50000")});
}

TEST_F(ExchangeSources, ShanghaiInventoryReadsCurrentWeeklyTablesWithoutCountingSubtotals)
{
    ac::Exchange source("SHFE", budget, {}, [&](const ac::HttpRequest& request, auto) {
        if (request.path.ends_with(".dat"))
            return ac::HttpReply{404, {}};
        EXPECT_EQ(
            request.path,
            "/data/tradedata/future/stockdata/weeklystock_20261120/ZH/all.html");
        return ac::HttpReply{200, reference_fixture("shfe_inventory_20261120.html")};
    });
    const auto rows = source.inventory("SHFE", "2026-11-20");
    ASSERT_EQ(rows.size(), 3);
    EXPECT_EQ(rows[0].product, "ao");
    EXPECT_EQ(rows[0].warehouse, wh::Cell{std::string("中铝山东")});
    EXPECT_EQ(rows[0].area, wh::Cell{std::string("山东")});
    EXPECT_EQ(rows[1].grade, wh::Cell{std::string("RMG380（硫Ⅰ级）")});
    EXPECT_EQ(rows[1].receipts_change, wh::Cell{std::string("20000")});
    EXPECT_EQ(rows[1].capacity, wh::Cell{});
    EXPECT_EQ(rows[2].warehouse, wh::Cell{std::string("总计")});
    EXPECT_EQ(rows[2].total, wh::Cell{std::string("119895")});
    EXPECT_EQ(rows[2].receipts, rows[2].total);
    EXPECT_EQ(rows[2].unit, wh::Cell{std::string("千克")});
}

TEST_F(ExchangeSources, ShanghaiInventoryKeepsFuelGradesAndCountsGoldOnlyOnce)
{
    ac::Exchange source("SHFE", budget, {}, [&](const ac::HttpRequest&, auto) {
        return ac::HttpReply{200, reference_fixture("shfe_inventory_20250103.json")};
    });
    const auto rows = source.inventory("SHFE", "2025-01-03");
    ASSERT_EQ(rows.size(), 4);
    EXPECT_EQ(rows[0].warehouse_type, wh::Cell{std::string("仓库")});
    EXPECT_EQ(rows[1].warehouse_type, wh::Cell{std::string("厂库")});
    EXPECT_EQ(rows[2].product, "fu");
    EXPECT_EQ(rows[2].grade, wh::Cell{std::string("RMG380（硫Ⅰ级）")});
    EXPECT_EQ(rows[2].total, wh::Cell{std::string("7335")});
    EXPECT_EQ(rows[2].capacity, wh::Cell{});
    EXPECT_EQ(rows[3].product, "au");
    EXPECT_EQ(rows[3].total, wh::Cell{std::string("15147")});
    EXPECT_EQ(rows[3].receipts, rows[3].total);
    for (const auto& row : rows)
        EXPECT_EQ(row.published_ms, wh::Cell{std::int64_t{1736236929000}});
}

TEST_F(ExchangeSources, ShanghaiInventoryNormalizesEmptyGradeAndSkipsAbsentGoldCursor)
{
    ac::Exchange source("SHFE", budget, {}, [](const ac::HttpRequest&, auto) {
        return ac::HttpReply{
            200,
            R"({"o_code":"0000","o_tradingday":"20250103","o_cursor":[],"o_cursorfu":[{"VARNAME":"燃料油$$FUEL OIL","VARID":"fu","REGNAME":"","WHABBRNAME":"洋山石油$$Yangshan","GRDNAME":"","WGHTUNIT":"2","PRESPOTWGHTS":100,"PREWRTWGHTS":0,"SPOTWGHTS":100,"WRTWGHTS":0,"SPOTCHANGE":0,"WRTCHANGE":0,"PREWHSTOCKS":"--","WHSTOCKS":"--","WHSTOCKCHANGE":"--","ROWSTATUS":"0"}]})"
        };
    });
    const auto rows = source.inventory("SHFE", "2025-01-03");
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows[0].product, "fu");
    EXPECT_EQ(rows[0].area, wh::Cell{});
    EXPECT_EQ(rows[0].grade, wh::Cell{});
}

TEST_F(ExchangeSources, ShanghaiReceiptsKeepAllWarehousesAndSeparateExchangeTotals)
{
    ac::Exchange shfe("SHFE", budget, {}, [&](const ac::HttpRequest& request, auto) {
        EXPECT_EQ(request.origin, "https://www.shfe.com.cn");
        EXPECT_EQ(request.path, "/data/tradedata/future/dailydata/20181113dailystock.dat");
        return ac::HttpReply{200, reference_fixture("shfe_receipts_20181113.json")};
    });
    const auto before = ac::observation_time();
    const auto rows = shfe.warehouse_receipts("SHFE", "2018-11-13");
    ASSERT_EQ(rows.size(), 6);
    EXPECT_EQ(rows[0].warehouse, wh::Cell{std::string("河南国储巩义")});
    EXPECT_EQ(rows[0].previous_volume, wh::Cell{Decimal::parse("4000")});
    EXPECT_EQ(rows[0].volume, wh::Cell{std::string("4996")});
    EXPECT_EQ(rows[0].volume_change, wh::Cell{std::string("996")});
    std::set<std::string> warehouses;
    for (const auto& row : rows) {
        EXPECT_EQ(row.exchange, "SHFE");
        EXPECT_NE(row.product, "sc");
        EXPECT_EQ(row.day, "2018-11-13");
        EXPECT_EQ(row.unit, wh::Cell{std::string(row.product == "au" ? "千克" : "吨")});
        EXPECT_EQ(row.crop_year, wh::Cell{});
        EXPECT_EQ(row.published_ms, wh::Cell{std::int64_t{1542113748000}});
        EXPECT_GE(row.observed_ms, before);
        warehouses.insert(std::get<std::string>(row.warehouse));
    }
    EXPECT_EQ(
        warehouses,
        (std::set<std::string>{
            "河南国储巩义",
            "中储洛阳",
            "营口港",
            "环宇沥青",
            "中储汉口",
            "上期所交割库"
        }));
    ac::Exchange ine("INE", budget, {}, [&](const ac::HttpRequest& request, auto) {
        EXPECT_EQ(request.origin, "https://www.ine.cn");
        return ac::HttpReply{200, reference_fixture("ine_receipts_20181113.json")};
    });
    const auto energy = ine.warehouse_receipts("INE", "2018-11-13");
    ASSERT_EQ(energy.size(), 8);
    for (const auto& row : energy) {
        EXPECT_EQ(row.exchange, "INE");
        EXPECT_EQ(row.product, "sc");
        EXPECT_EQ(row.unit, wh::Cell{std::string("桶")});
        EXPECT_EQ(row.published_ms, wh::Cell{std::int64_t{1542110400000}});
    }
}

TEST_F(ExchangeSources, ShanghaiReceiptsDistinguishWarehouseAndFactoryAtTheSameLocation)
{
    ac::Exchange source("SHFE", budget, {}, [&](const ac::HttpRequest&, auto) {
        return ac::HttpReply{200, reference_fixture("shfe_receipts_20160104.json")};
    });
    const auto rows = source.warehouse_receipts("SHFE", "2016-01-04");
    ASSERT_EQ(rows.size(), 2);
    for (const auto& row : rows) {
        EXPECT_EQ(row.product, "bu");
        EXPECT_EQ(row.warehouse, wh::Cell{std::string("金海宏业")});
    }
    EXPECT_EQ(rows[0].warehouse_type, wh::Cell{std::string("仓库")});
    EXPECT_EQ(rows[0].volume, wh::Cell{std::string("20000")});
    EXPECT_EQ(rows[1].warehouse_type, wh::Cell{std::string("厂库")});
    EXPECT_EQ(rows[1].volume, wh::Cell{std::string("5440")});
}

TEST_F(ExchangeSources, ShanghaiReceiptsKeepBondedCopperOutOfShfe)
{
    const auto fetch = [&](const ac::HttpRequest&, auto) {
        return ac::HttpReply{200, reference_fixture("shfe_receipts_20250103.json")};
    };
    for (const auto* exchange : {"SHFE", "INE"}) {
        ac::Exchange source(exchange, budget, {}, fetch);
        const auto rows = source.warehouse_receipts(exchange, "2025-01-03");
        ASSERT_EQ(rows.size(), 1);
        EXPECT_EQ(rows[0].product, std::string(exchange) == "SHFE" ? "cu" : "bc");
        EXPECT_EQ(rows[0].volume, wh::Cell{std::string("0")});
    }
}

TEST_F(ExchangeSources, ShanghaiReceiptHtmlUsesEachTablesWarehouseAndAreaHeadings)
{
    ac::Exchange source("SHFE", budget, {}, [&](const ac::HttpRequest& request, auto) {
        EXPECT_EQ(request.origin, "https://www.shfe.com.cn");
        if (request.path == "/data/tradedata/future/dailydata/20261120dailystock.dat")
            return ac::HttpReply{404, {}};
        EXPECT_EQ(request.path, "/data/tradedata/future/stockdata/dailystock_20261120/ZH/all.html");
        return ac::HttpReply{200, reference_fixture("shfe_receipts_20261120.html")};
    });
    const auto rows = source.warehouse_receipts("SHFE", "2026-11-20");
    ASSERT_EQ(rows.size(), 6);
    EXPECT_EQ(rows[0].warehouse, wh::Cell{std::string("弘润石化(仓库)")});
    EXPECT_EQ(rows[0].warehouse_type, wh::Cell{std::string("仓库")});
    EXPECT_EQ(rows[0].area, wh::Cell{std::string("山东")});
    EXPECT_EQ(rows[1].warehouse, wh::Cell{std::string("中基宁波")});
    EXPECT_EQ(rows[2].warehouse, rows[1].warehouse);
    EXPECT_EQ(rows[1].area, wh::Cell{std::string("江苏")});
    EXPECT_EQ(rows[2].area, wh::Cell{std::string("山东")});
    EXPECT_EQ(rows[3].warehouse, wh::Cell{std::string("弘润石化(厂库)")});
    EXPECT_EQ(rows[3].warehouse_type, wh::Cell{std::string("厂库")});
    EXPECT_EQ(rows[3].area, rows[0].area);
    EXPECT_EQ(rows[4].product, "sp");
    EXPECT_EQ(rows[4].warehouse, wh::Cell{std::string("金翌贸易")});
    EXPECT_EQ(rows[4].area, wh::Cell{std::string("江苏")});
    EXPECT_EQ(rows[5].product, "au");
    EXPECT_EQ(rows[5].warehouse, wh::Cell{std::string("上期所指定交割金库")});
    EXPECT_EQ(rows[5].area, wh::Cell{});
    EXPECT_EQ(rows[5].unit, wh::Cell{std::string("千克")});
    EXPECT_EQ(rows[5].previous_volume, wh::Cell{Decimal::parse("119898")});
    EXPECT_EQ(rows[5].published_ms, wh::Cell{});
}

TEST_F(ExchangeSources, ShanghaiHistoricalReceiptsUseTheirPublishedTradingDay)
{
    ac::Exchange source("SHFE", budget, {}, [&](const ac::HttpRequest&, auto) {
        return ac::HttpReply{200, reference_fixture("shfe_receipts_20140519.json")};
    });
    const auto rows = source.warehouse_receipts("SHFE", "2014-05-19");
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows[0].day, "2014-05-19");
    EXPECT_EQ(rows[0].volume, wh::Cell{std::string("1919")});
}

TEST_F(ExchangeSources, IneMemberRanksMergeByContractAndKeepUnrankedSidesMissing)
{
    const std::string raw = R"({"report_date":"20261120","o_code":"0000",
"update_date":"20261120 15:20:45","o_cursor":[
{"INSTRUMENTID":"lu2612","RANK":1,"PARTICIPANTABBR1":"甲会员","CJ1":418,"CJ1_CHG":166,
 "PARTICIPANTABBR2":"乙会员","CJ2":1612,"CJ2_CHG":-58,
 "PARTICIPANTABBR3":"丙会员","CJ3":1000,"CJ3_CHG":0},
{"INSTRUMENTID":"lu2612","RANK":2,"PARTICIPANTABBR1":"乙会员","CJ1":300,"CJ1_CHG":20,
 "PARTICIPANTABBR2":"甲会员","CJ2":500,"CJ2_CHG":10,
 "PARTICIPANTABBR3":"甲会员","CJ3":120,"CJ3_CHG":-5},
{"INSTRUMENTID":"nr2701","RANK":1,"PARTICIPANTABBR1":"甲会员","CJ1":10,"CJ1_CHG":1,
 "PARTICIPANTABBR2":"甲会员","CJ2":20,"CJ2_CHG":2,
 "PARTICIPANTABBR3":"甲会员","CJ3":30,"CJ3_CHG":3},
{"INSTRUMENTID":"lu2612","RANK":999}]})";
    ac::Exchange source("INE", budget, {}, [&](const ac::HttpRequest& request, auto) {
        EXPECT_EQ(request.origin, "https://www.ine.cn");
        EXPECT_EQ(request.path, "/data/tradedata/future/dailydata/pm20261120.dat");
        return ac::HttpReply{200, raw};
    });
    const auto before = ac::observation_time();
    const auto rows = source.member_positions("INE", "2026-11-20");
    ASSERT_EQ(rows.size(), 4);
    for (const auto& row : rows) {
        EXPECT_EQ(row.exchange, "INE");
        EXPECT_EQ(row.published_ms, wh::Cell{std::int64_t{1795159245000}});
        EXPECT_GE(row.observed_ms, before);
        if (row.product == "nr") {
            EXPECT_EQ(row.contract, wh::Cell{std::string("INE/nr/2027-01")});
            EXPECT_EQ(row.volume, wh::Cell{std::string("10")});
        } else if (row.member == "甲会员") {
            EXPECT_EQ(row.contract, wh::Cell{std::string("INE/lu/2026-12")});
            EXPECT_EQ(row.volume, wh::Cell{std::string("418")});
            EXPECT_EQ(row.long_position, wh::Cell{std::string("500")});
            EXPECT_EQ(row.short_position, wh::Cell{std::string("120")});
            EXPECT_EQ(row.short_change, wh::Cell{std::string("-5")});
        } else if (row.member == "乙会员")
            EXPECT_EQ(row.short_position, wh::Cell{});
        else {
            EXPECT_EQ(row.volume, wh::Cell{});
            EXPECT_EQ(row.long_position, wh::Cell{});
        }
    }
}

TEST_F(ExchangeSources, ShfeMemberRanksKeepSummaryScopeAndDistinguishMissingFromZero)
{
    const std::string raw = R"({"report_date":"20261120","o_code":"0000","o_cursor":[
{"INSTRUMENTID":"rb2701","RANK":1,"PARTICIPANTABBR1":"甲会员","CJ1":10,"CJ1_CHG":1,
 "PARTICIPANTABBR2":"乙会员","CJ2":20,"CJ2_CHG":2,
 "PARTICIPANTABBR3":"甲会员","CJ3":30,"CJ3_CHG":3},
{"INSTRUMENTID":"rball","RANK":0,"PARTICIPANTABBR1":"非期货公司会员","CJ1":50,"CJ1_CHG":5,
 "PARTICIPANTABBR2":"非期货公司会员","CJ2":0,"CJ2_CHG":0,
 "PARTICIPANTABBR3":"非期货公司会员","CJ3":40,"CJ3_CHG":4},
{"INSTRUMENTID":"lu2612","RANK":1}]})";
    ac::Exchange source("SHFE", budget, {}, [&](const ac::HttpRequest& request, auto) {
        EXPECT_EQ(request.origin, "https://www.shfe.com.cn");
        return ac::HttpReply{200, raw};
    });
    const auto rows = source.member_positions("SHFE", "2026-11-20");
    ASSERT_EQ(rows.size(), 3);
    for (const auto& row : rows) {
        EXPECT_EQ(row.product, "rb");
        if (row.member == "非期货公司会员") {
            EXPECT_EQ(row.contract, wh::Cell{});
            EXPECT_EQ(row.long_position, wh::Cell{std::string("0")});
        } else if (row.member == "甲会员") {
            EXPECT_EQ(row.contract, wh::Cell{std::string("SHFE/rb/2027-01")});
            EXPECT_EQ(row.volume, wh::Cell{std::string("10")});
            EXPECT_EQ(row.long_position, wh::Cell{});
        } else
            EXPECT_EQ(row.volume, wh::Cell{});
    }
}

TEST_F(ExchangeSources, EarlyShfeRankingKeepsOnlyRealMembersOnContracts)
{
    const std::string raw = R"({"o_code":0,"o_msg":"","o_cursor":[
{"INSTRUMENTID":"cu1501","RANK":-1,"PARTICIPANTABBR1":"期货公司","CJ1":90,"CJ1_CHG":9,
 "PARTICIPANTABBR2":"期货公司","CJ2":80,"CJ2_CHG":8,
 "PARTICIPANTABBR3":"期货公司","CJ3":70,"CJ3_CHG":7},
{"INSTRUMENTID":"cu1501","RANK":0,"PARTICIPANTABBR1":"非期货公司","CJ1":9,"CJ1_CHG":1,
 "PARTICIPANTABBR2":"非期货公司","CJ2":8,"CJ2_CHG":1,
 "PARTICIPANTABBR3":"非期货公司","CJ3":7,"CJ3_CHG":1},
{"INSTRUMENTID":"cu1501","RANK":1,"PARTICIPANTABBR1":"甲会员","CJ1":10,"CJ1_CHG":1,
 "PARTICIPANTABBR2":"甲会员","CJ2":20,"CJ2_CHG":2,
 "PARTICIPANTABBR3":"甲会员","CJ3":30,"CJ3_CHG":3},
{"INSTRUMENTID":"cu1501","RANK":999,"PARTICIPANTABBR1":"","CJ1":10,"CJ1_CHG":1,
 "PARTICIPANTABBR2":"","CJ2":20,"CJ2_CHG":2,
 "PARTICIPANTABBR3":"","CJ3":30,"CJ3_CHG":3},
{"INSTRUMENTID":"cuactv","RANK":1,"PARTICIPANTABBR1":"甲会员","CJ1":10,"CJ1_CHG":1,
 "PARTICIPANTABBR2":"甲会员","CJ2":20,"CJ2_CHG":2,
 "PARTICIPANTABBR3":"甲会员","CJ3":30,"CJ3_CHG":3}]})";
    ac::Exchange source("SHFE", budget, {}, [&](const auto&, auto) {
        return ac::HttpReply{200, raw};
    });
    const auto rows = source.member_positions("SHFE", "2015-01-05");
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows[0].member, "甲会员");
    EXPECT_EQ(rows[0].contract, wh::Cell{std::string("SHFE/cu/2015-01")});
    EXPECT_EQ(rows[0].published_ms, wh::Cell{});
}

TEST_F(ExchangeSources, IneRankingReadsReportTimeWithoutSeparator)
{
    const std::string raw = R"({"report_date":"20240102","o_code":0,
"update_date":"2024010220:01:00","o_cursor":[
{"INSTRUMENTID":"lu2403","RANK":1,"PARTICIPANTABBR1":"甲会员","CJ1":10,"CJ1_CHG":1,
 "PARTICIPANTABBR2":"甲会员","CJ2":20,"CJ2_CHG":2,
 "PARTICIPANTABBR3":"甲会员","CJ3":30,"CJ3_CHG":3}]})";
    ac::Exchange source("INE", budget, {}, [&](const auto&, auto) {
        return ac::HttpReply{200, raw};
    });
    const auto rows = source.member_positions("INE", "2024-01-02");
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows[0].published_ms, wh::Cell{std::int64_t{1704196860000}});
}

TEST_F(ExchangeSources, GfexMemberRanksIncludeLithiumAndPreserveUnrankedSides)
{
    using Json = ac::values::Json;
    ac::Exchange source("GFEX", budget, {}, [&](const ac::HttpRequest& request, auto) {
        EXPECT_EQ(request.origin, "http://www.gfex.com.cn");
        EXPECT_EQ(request.method, "POST");
        Json params = Json::object();
        std::istringstream form(request.body);
        for (std::string field; std::getline(form, field, '&');) {
            const auto at = field.find('=');
            params[field.substr(0, at)] = Json::array({field.substr(at + 1)});
        }
        Json rows;
        if (request.path == "/u/interfacesWebVariety/loadList")
            rows = Json::array({{{"varietyId", "si"}}, {{"varietyId", "lc"}}});
        else if (request.path.ends_with("loadListContract_id")) {
            EXPECT_EQ(params.at("trade_date").at(0), "20231110");
            rows = Json::array({params.at("variety").at(0) == "si" ? "si2312" : "lc2401"});
        } else {
            EXPECT_EQ(request.path, "/u/interfacesWebTiMemberDealPosiQuotes/loadList");
            EXPECT_EQ(params.at("trade_type").at(0), "0");
            const auto side = params.at("data_type").at(0).get<std::string>();
            rows = Json::array(
                {{{"contractId", "1"},
                     {"abbr", side == "2" ? "乙会员" : "甲会员"},
                     {"todayQty", side == "1" ? 10 : 20},
                     {"qtySub", -1}},
                    {{"contractId", "总计"}, {"abbr", nullptr}, {"todayQty", 20}, {"qtySub", -1}}});
        }
        return ac::HttpReply{200, Json{{"code", "0"}, {"param", params}, {"data", rows}}.dump()};
    });
    const auto before = ac::observation_time();
    const auto rows = source.member_positions("GFEX", "2023-11-10");
    ASSERT_EQ(rows.size(), 4);
    EXPECT_EQ(source.requests(), 9);
    for (const auto& row : rows) {
        EXPECT_EQ(row.exchange, "GFEX");
        EXPECT_EQ(
            row.contract,
            wh::Cell{
                row.product == "si" ? std::string("GFEX/si/2023-12")
                                    : std::string("GFEX/lc/2024-01")
            });
        EXPECT_EQ(row.published_ms, wh::Cell{});
        EXPECT_GE(row.observed_ms, before);
        if (row.member == "甲会员") {
            EXPECT_EQ(row.volume, wh::Cell{std::string("10")});
            EXPECT_EQ(row.long_position, wh::Cell{});
            EXPECT_EQ(row.short_position, wh::Cell{std::string("20")});
            EXPECT_EQ(row.short_change, wh::Cell{std::string("-1")});
        } else {
            EXPECT_EQ(row.volume, wh::Cell{});
            EXPECT_EQ(row.long_position, wh::Cell{std::string("20")});
            EXPECT_EQ(row.short_position, wh::Cell{});
        }
    }
}

TEST_F(ExchangeSources, GfexWithoutPublishedRanksHasNoMemberRows)
{
    ac::Exchange source("GFEX", budget, {}, [](const ac::HttpRequest& request, auto) {
        return ac::HttpReply{
            200,
            request.path == "/u/interfacesWebVariety/loadList"
                ? R"({"code":"0","param":{},"data":[{"varietyId":"si"}]})"
                : R"({"code":"0","param":{"variety":["si"],"trade_date":["20221222"]},"data":[]})"
        };
    });
    EXPECT_TRUE(source.member_positions("GFEX", "2022-12-22").empty());
    EXPECT_EQ(source.requests(), 2);
}

TEST_F(ExchangeSources, RefusedGfexRankingDoesNotReturnPartialMembers)
{
    ac::Exchange source("GFEX", budget, {}, [](const ac::HttpRequest& request, auto) {
        if (request.path == "/u/interfacesWebVariety/loadList")
            return ac::HttpReply{200, R"({"code":"0","param":{},"data":[{"varietyId":"lc"}]})"};
        if (request.path.ends_with("loadListContract_id"))
            return ac::HttpReply{
                200,
                R"({"code":"0","param":{"variety":["lc"],"trade_date":["20231110"]},"data":["lc2401"]})"
            };
        if (request.body.find("data_type=1") != std::string::npos)
            return ac::HttpReply{
                200,
                R"({"code":"0","param":{"variety":["lc"],"trade_date":["20231110"],"trade_type":["0"],"contract_id":["lc2401"],"data_type":["1"]},"data":[{"contractId":"1","abbr":"甲会员","todayQty":20,"qtySub":2}]})"
            };
        return ac::HttpReply{
            200,
            R"({"code":"1","msg":"查询的合约号成交量不能小于20000","data":null})"
        };
    });
    EXPECT_THROW(source.member_positions("GFEX", "2023-11-10"), ac::SourceError);
}

TEST_F(ExchangeSources, ExactDecimalsMissingPricesAndFeeBasesReachParquet)
{
    for (const auto* exchange : {"SHFE", "INE"}) {
        ac::Exchange source(exchange, budget, {}, [](const ac::HttpRequest& request, auto) {
            return ac::HttpReply{
                200,
                request.path.find("/js") != std::string::npos ? shfe_settlement : shfe_daily
            };
        });
        auto daily = source.daily(exchange, "2026-10-30");
        ASSERT_EQ(daily.size(), 1);
        EXPECT_EQ(daily[0].close, wh::Cell{});
        EXPECT_EQ(std::get<std::string>(daily[0].settlement), "3399.123456");
        EXPECT_EQ(std::get<std::string>(daily[0].volume), "12345678901234567890");
        EXPECT_EQ(daily[0].amount, wh::Cell{}); // Preserve unverified turnover units as absent.
        const auto fees = source.settlement(exchange, "2026-10-30");
        EXPECT_EQ(std::get<std::string>(fees[0].fee_rate), "0.00005");
        EXPECT_EQ(std::get<std::string>(fees[0].close_today_fee_rate), "0.000025");
        wh::Store store(root / exchange, "exchange");
        auto writer = store.writer();
        writer.put(std::string("daily/") + exchange + "/2026-10-30", wh::rows(daily));
        writer.put(std::string("settlement/") + exchange + "/2026-10-30", wh::rows(fees));
        auto version = writer.commit();
        EXPECT_EQ(
            std::get<std::string>(
                store.read(version, std::string("daily/") + exchange + "/2026-10-30")[0][11]),
            "12345678901234567890.000000");
    }
    // Until 2024-03-20 the status is the number 0; until 2024-07-19 there is no close-today fee.
    auto older = shfe_daily;
    older.replace(older.find(R"("0000")"), 6, "0");
    auto older_fees = shfe_settlement;
    const std::string today_fees = R"(,"TTRADEFEERATIO":0.025,"TTRADEFEEUNIT":0)";
    older_fees.erase(older_fees.find(today_fees), today_fees.size());
    ac::Exchange shfe("SHFE", budget, {}, [&](const ac::HttpRequest& request, auto) {
        return ac::HttpReply{
            200,
            request.path.find("/js") != std::string::npos ? older_fees : older
        };
    });
    EXPECT_EQ(shfe.daily("SHFE", "2026-10-30").size(), 1);
    const auto fees = shfe.settlement("SHFE", "2026-10-30");
    ASSERT_EQ(fees.size(), 1);
    EXPECT_EQ(std::get<std::string>(fees[0].fee_rate), "0.00005");
    EXPECT_EQ(fees[0].close_today_fee_rate, wh::Cell{});
    EXPECT_EQ(fees[0].close_today_fee_per_lot, wh::Cell{});
    wh::Store store(root / "older", "exchange");
    auto writer = store.writer();
    writer.put("settlement/SHFE/2026-10-30", wh::rows(fees));
    const auto stored = store.read(writer.commit(), "settlement/SHFE/2026-10-30");
    EXPECT_EQ(stored[0][8], wh::Cell{});
    ac::Exchange czce("CZCE", budget, {}, [](const auto&, auto) {
        return ac::HttpReply{200, czce_settlement};
    });
    auto rows = czce.settlement("CZCE", "2026-10-30");
    ASSERT_EQ(rows.size(), 2);
    EXPECT_EQ(rows[0].contract, "CZCE/ma/2027-01");
    EXPECT_EQ(std::get<std::string>(rows[0].long_margin_rate), "0.08");
    EXPECT_EQ(std::get<std::string>(rows[0].fee_rate), "0.00005");
    EXPECT_EQ(std::get<std::string>(rows[0].close_today_fee_per_lot), "0");
    EXPECT_EQ(std::get<std::string>(rows[1].fee_per_lot), "5");
    EXPECT_EQ(std::get<std::string>(rows[1].close_today_fee_per_lot), "10");
    // Before 2022-11-07 the table has no fee basis; every fee is per lot.
    ac::Exchange before("CZCE", budget, {}, [](const auto&, auto) {
        return ac::HttpReply{
            200,
            "郑州商品交易所期货结算参数表(2022-01-04)\n" "合约代码|当日结算价|是否单边市|连续单边市天数|交易保证金率(%)|涨跌停板(%)|" "交易手续费|交割手续费|日内平今仓交易手续费|日持仓限额\n" "AP201 |8,565.00 |N |0 |20 |±9 |5.00 |0.00 |20.00 |20 \n"
        };
    });
    rows = before.settlement("CZCE", "2022-01-04");
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows[0].contract, "CZCE/ap/2022-01");
    EXPECT_EQ(std::get<std::string>(rows[0].fee_rate), "0");
    EXPECT_EQ(std::get<std::string>(rows[0].fee_per_lot), "5");
    EXPECT_EQ(std::get<std::string>(rows[0].close_today_fee_per_lot), "20");
    // Older tables give buy and sell margins and name the close-today fee differently;
    // in 2016 only a flag says whether it is half the fee.
    ac::Exchange oldest("CZCE", budget, {}, [](const ac::HttpRequest& request, auto) {
        if (request.path.find("20160105") != std::string::npos) {
            std::ifstream input(
                fs::path(ASTERION_EXCHANGE_FIXTURES) / "czce_settlement_2016.txt",
                std::ios::binary);
            return ac::HttpReply{200, {std::istreambuf_iterator<char>(input), {}}};
        }
        return ac::HttpReply{
            200,
            "郑州商品交易所期货结算参数表(2018-08-24)\n" "合约代码|当日结算价|是否单边市|连续单边市天数|买交易保证金率(%)|卖交易保证金率(%)|" "交易手续费|交割手续费|平今仓手续费\n" "AP810 |10,886.00 |N |0 |11.00 |11.00 |20.00 |10.00 |20.00 \n"
        };
    });
    rows = oldest.settlement("CZCE", "2016-01-05");
    ASSERT_EQ(rows.size(), 2);
    EXPECT_EQ(std::get<std::string>(rows[0].long_margin_rate), "0.2");
    EXPECT_EQ(std::get<std::string>(rows[0].short_margin_rate), "0.2");
    EXPECT_EQ(std::get<std::string>(rows[0].fee_per_lot), "4.3");
    EXPECT_EQ(std::get<std::string>(rows[0].close_today_fee_per_lot), "2.15");
    EXPECT_EQ(std::get<std::string>(rows[1].close_today_fee_per_lot), "3");
    rows = oldest.settlement("CZCE", "2018-08-24");
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(std::get<std::string>(rows[0].long_margin_rate), "0.11");
    EXPECT_EQ(std::get<std::string>(rows[0].close_today_fee_per_lot), "20");
}

TEST_F(ExchangeSources, CffexEncodingAndCzceDailyColumnsExcludeOptions)
{
    std::ifstream file(fs::path(ASTERION_EXCHANGE_FIXTURES) / "cffex_daily.csv", std::ios::binary);
    ASSERT_TRUE(file.good());
    const std::string csv{std::istreambuf_iterator<char>(file), {}};
    ac::Exchange cffex("CFFEX", budget, {}, [&](const ac::HttpRequest& request, auto) {
        EXPECT_EQ(request.path, "/sj/hqsj/rtj/202610/30/20261030_1.csv");
        return ac::HttpReply{200, csv};
    });
    const auto rows = cffex.daily("CFFEX", "2026-10-30");
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(rows[0].contract, "CFFEX/ic/2026-11");
    ac::Exchange czce("CZCE", budget, {}, [](const auto&, auto) {
        return ac::HttpReply{
            200,
            "郑州商品交易所期货每日行情表(2026-10-30)\n" "合约代码|昨结算|今开盘|最高价|最低价|今收盘|今结算|成交量(手)|持仓量\n" "MA701|2,100.50|0|0|0|2,101|2,100|3,000|1,500\n"
        };
    });
    const auto daily = czce.daily("CZCE", "2026-10-30");
    ASSERT_EQ(daily.size(), 1);
    EXPECT_EQ(daily[0].close, wh::Cell{});
    EXPECT_EQ(std::get<std::string>(daily[0].previous_settlement), "2100.5");
    EXPECT_EQ(std::get<std::string>(daily[0].volume), "3000");
}

TEST_F(ExchangeSources, GfexTasksShareRequestSpacing)
{
    auto paced = std::make_shared<ac::Budget>(1, 20ms);
    std::vector<std::chrono::steady_clock::time_point> sent;
    const auto fetch = [&](const auto&, auto) {
        sent.push_back(std::chrono::steady_clock::now());
        return ac::HttpReply{200, R"({"code":"0","param":{"trade_date":["20261030"]},"data":[
            {"variety":"工业硅","contractId":"si2701","openFee":3,"offsetFee":3,"shortOpenFee":3,
            "shortOffsetFee":1.5,"specBuyRate":0.1,"specSellRate":0.1,"style":"绝对值"}]})"};
    };
    ac::Exchange first("GFEX", paced, {}, fetch), second("GFEX", paced, {}, fetch);
    EXPECT_EQ(first.settlement("GFEX", "2026-10-30").size(), 1);
    EXPECT_EQ(second.settlement("GFEX", "2026-10-30").size(), 1);
    EXPECT_EQ(first.settlement("GFEX", "2026-10-30").size(), 1);
    ASSERT_EQ(sent.size(), 3);
    EXPECT_GE(sent[1] - sent[0], 10ms);
    EXPECT_GE(sent[2] - sent[1], 10ms);
}

TEST_F(ExchangeSources, PortalsKeepSourceIdentityAndDoNotRequestUnsupportedTables)
{
    for (const auto* exchange : {"DCE", "GFEX"}) {
        ac::Exchange source(exchange, budget, {}, [&](const ac::HttpRequest& request, auto) {
            EXPECT_FALSE(request.body.empty());
            EXPECT_EQ(
                request.origin,
                std::string(
                    exchange == std::string("DCE") ? "http://www.dce.com.cn"
                                                   : "http://www.gfex.com.cn"));
            if (request.path == "/u/interfacesWebTiFutAndOptSettle/loadList")
                return ac::HttpReply{
                    200,
                    R"({"code":"0","param":{"trade_date":["20261030"]},"data":[
                    {"variety":"碳酸锂","contractId":"lc2701","openFee":0.8,"offsetFee":0.8,
                    "shortOpenFee":0.8,"shortOffsetFee":0,"specBuyRate":0.12,"specSellRate":0.13,
                    "style":"比例值"},
                    {"variety":"工业硅","contractId":"si2701","openFee":3,"offsetFee":3,
                    "shortOpenFee":3,"shortOffsetFee":1.5,"specBuyRate":0.1,"specSellRate":0.1,
                    "style":"绝对值"},
                    {"variety":"上市辅助指数","contractId":"ssfz2212"}]})"
                };
            return ac::HttpReply{200, R"({"code":"0","param":{"trade_date":["20261030"]},"data":[
                {"variety":"玉米","contractId":"c2701","varietyOrder":"si","delivMonth":"2701",
                "open":1200.1,"high":1250,"low":1190,"close":1210,"clearPrice":1208,
                "lastClear":1200,"volumn":100,"openInterest":300}]})"};
        });
        const auto rows = source.daily(exchange, "2026-10-30");
        ASSERT_EQ(rows.size(), 1);
        EXPECT_EQ(rows[0].exchange, exchange);
        EXPECT_EQ(rows[0].amount, wh::Cell{});
        if (exchange == std::string("GFEX")) {
            const auto fees = source.settlement(exchange, "2026-10-30");
            ASSERT_EQ(fees.size(), 2);
            EXPECT_EQ(fees[0].contract, "GFEX/lc/2027-01");
            EXPECT_EQ(std::get<std::string>(fees[0].long_margin_rate), "0.12");
            EXPECT_EQ(std::get<std::string>(fees[0].short_margin_rate), "0.13");
            EXPECT_EQ(std::get<std::string>(fees[0].fee_rate), "0.00008");
            EXPECT_EQ(std::get<std::string>(fees[0].fee_per_lot), "0");
            EXPECT_EQ(std::get<std::string>(fees[0].close_today_fee_rate), "0");
            EXPECT_EQ(std::get<std::string>(fees[0].close_today_fee_per_lot), "0");
            EXPECT_EQ(fees[1].contract, "GFEX/si/2027-01");
            EXPECT_EQ(std::get<std::string>(fees[1].fee_rate), "0");
            EXPECT_EQ(std::get<std::string>(fees[1].fee_per_lot), "3");
            EXPECT_EQ(std::get<std::string>(fees[1].close_today_fee_rate), "0");
            EXPECT_EQ(std::get<std::string>(fees[1].close_today_fee_per_lot), "1.5");
            EXPECT_THROW(source.price_ticks(exchange, "2026-10-30"), ac::SourceError);
            EXPECT_EQ(source.requests(), 2);
        } else {
            EXPECT_THROW(source.settlement(exchange, "2026-10-30"), ac::SourceError);
            EXPECT_EQ(source.requests(), 1);
        }
    }
    ac::Exchange intraday_surcharge("GFEX", budget, {}, [](const auto&, auto) {
        return ac::HttpReply{200, R"({"code":"0","param":{"trade_date":["20261030"]},"data":[
            {"variety":"碳酸锂","contractId":"lc2701","openFee":0.8,"offsetFee":0.8,
            "shortOpenFee":3.2,"shortOffsetFee":3.2,"specBuyRate":0.12,"specSellRate":0.12,
            "style":"比例值"}]})"};
    });
    const auto surcharged = intraday_surcharge.settlement("GFEX", "2026-10-30");
    ASSERT_EQ(surcharged.size(), 1);
    EXPECT_EQ(std::get<std::string>(surcharged[0].fee_rate), "0.00008");
    EXPECT_EQ(std::get<std::string>(surcharged[0].close_today_fee_rate), "0.00032");
    ac::Exchange mismatch("GFEX", budget, {}, [](const auto&, auto) {
        return ac::HttpReply{200, R"({"code":"0","param":{"trade_date":["20261030"]},"data":[
            {"variety":"碳酸锂","contractId":"lc2701","openFee":0.8,"offsetFee":1.6,
            "shortOpenFee":0.8,"shortOffsetFee":0,"specBuyRate":0.12,"specSellRate":0.12,
            "style":"比例值"}]})"};
    });
    EXPECT_THROW(mismatch.settlement("GFEX", "2026-10-30"), ac::SourceError);
}

TEST_F(ExchangeSources, MissingPublicationIsUncoveredButRefusalAndWrongDateFail)
{
    for (const auto status : {404, 412, 503, 302}) {
        ac::Exchange source("SHFE", budget, {}, [&](const auto&, auto) {
            return ac::HttpReply{status, "private diagnostic"};
        });
        if (status == 404)
            EXPECT_TRUE(source.daily("SHFE", "2026-10-30").empty());
        else {
            try {
                source.daily("SHFE", "2026-10-30");
                FAIL();
            } catch (const ac::SourceError& error) {
                EXPECT_EQ(error.code, "unavailable");
                EXPECT_EQ(std::string(error.what()).find("private"), std::string::npos);
            }
        }
    }
    ac::Exchange wrong("SHFE", budget, {}, [](const auto&, auto) {
        return ac::HttpReply{200, shfe_daily};
    });
    EXPECT_THROW(wrong.daily("SHFE", "2026-10-29"), ac::SourceError);
    ac::Exchange receipts("SHFE", budget, {}, [](const ac::HttpRequest& request, auto) {
        return request.path.ends_with(".dat")
            ? ac::HttpReply{404, {}}
            : ac::HttpReply{200, reference_fixture("shfe_receipts_20261120.html")};
    });
    EXPECT_THROW(receipts.warehouse_receipts("SHFE", "2026-11-26"), ac::SourceError);
    for (const auto* day : {"2014-01-10", "2026-11-27"}) {
        ac::Exchange weekly("SHFE", budget, {}, [&](const ac::HttpRequest& request, auto) {
            return request.path.ends_with(".dat")
                ? ac::HttpReply{404, {}}
                : ac::HttpReply{
                      200,
                      reference_fixture(
                          std::string_view(day).starts_with("2014")
                              ? "shfe_inventory_20140103.html"
                              : "shfe_inventory_20261120.html")
                  };
        });
        EXPECT_THROW(weekly.inventory("SHFE", day), ac::SourceError);
    }
    ac::Exchange invalid("SHFE", budget, {}, [](const auto&, auto) {
        return ac::HttpReply{200, "<html>challenge</html>"};
    });
    EXPECT_THROW(invalid.daily("SHFE", "2026-10-30"), ac::SourceError);
    std::stop_source cancel;
    cancel.request_stop();
    ac::Exchange stopped(
        "SHFE",
        budget,
        cancel.get_token(),
        [](const auto&, auto) -> ac::HttpReply {
            ADD_FAILURE() << "cancelled request was sent";
            return {200, shfe_daily};
        });
    EXPECT_THROW(stopped.daily("SHFE", "2026-10-30"), ac::SourceError);
}

TEST_F(ExchangeSources, HistoricalPriceTicksKeepExactValuesAndOneDayOfEvidence)
{
    auto raw = reference_fixture();
    raw.insert(raw.find("</Contracts>"), "<Contract><PrdTp>期权</PrdTp></Contract>");
    ac::Exchange source("CZCE", budget, {}, [&](const ac::HttpRequest& request, auto) {
        EXPECT_EQ(request.origin, "https://www.czce.com.cn");
        EXPECT_EQ(
            request.path,
            "/cn/DFSStaticFiles/Future/2022/20220104/FutureDataReferenceData.xml");
        return ac::HttpReply{200, raw};
    });
    const auto rows = source.price_ticks("CZCE", "2022-01-04");
    ASSERT_EQ(rows.size(), 4);
    EXPECT_EQ(rows[0].contract, wh::Cell{std::string("CZCE/ap/2022-01")});
    EXPECT_EQ(rows[0].price_tick.str(), "1");
    EXPECT_EQ(rows[1].price_tick.str(), "5");
    EXPECT_EQ(rows[2].price_tick.str(), "2");
    EXPECT_EQ(rows[3].price_tick.str(), "0.2");
    for (const auto& row : rows) {
        EXPECT_EQ(row.effective_from, "2022-01-04");
        EXPECT_EQ(row.effective_until, "2022-01-05");
        EXPECT_EQ(
            row.source_url,
            "https://www.czce.com.cn/cn/DFSStaticFiles/Future/2022/20220104/FutureDataReferenceData.xml");
    }
}

TEST_F(ExchangeSources, InvalidHistoricalTickEvidenceFailsInsteadOfGuessing)
{
    const std::vector<std::pair<std::string, std::string>> changes{
        {"<TckSz>1.00元/吨</TckSz>", "<TckSz>1.00元/手</TckSz>"},
        {"<TckSz>1.00元/吨</TckSz>", "<TckSz>0.00元/吨</TckSz>"},
        {"<TckSz>1.00元/吨</TckSz>", "<TckSz>1.00至2.00元/吨</TckSz>"},
        {"<TrdCcyCd>CNY</TrdCcyCd>", "<TrdCcyCd>USD</TrdCcyCd>"},
        {"<YrCd>2022</YrCd>", "<YrCd>2032</YrCd>"},
        {"<FrstTrdDt>2021-01-18</FrstTrdDt>", "<FrstTrdDt>2022-01-05</FrstTrdDt>"},
        {"<LstTrdDt>2022-01-17</LstTrdDt>", "<LstTrdDt>2022-01-03</LstTrdDt>"},
        {"</Contracts>", ""}
    };
    for (const auto& [before, after] : changes) {
        SCOPED_TRACE(after);
        auto raw = reference_fixture();
        raw.replace(raw.find(before), before.size(), after);
        ac::Exchange source("CZCE", budget, {}, [&](const auto&, auto) {
            return ac::HttpReply{200, raw};
        });
        try {
            source.price_ticks("CZCE", "2022-01-04");
            FAIL() << "invalid historical reference accepted";
        } catch (const ac::SourceError& error) {
            EXPECT_EQ(error.code, "invalid_data");
        }
    }
}

ac::wire::AcquisitionTask wait(ac::Manager& manager, const std::string& id)
{
    const auto until = std::chrono::steady_clock::now() + 10s;
    for (;;) {
        auto task = manager.get(id);
        if (task.state() != ac::wire::TASK_STATE_QUEUED
            && task.state() != ac::wire::TASK_STATE_RUNNING)
            return task;
        if (std::chrono::steady_clock::now() > until)
            throw std::runtime_error("acquisition did not finish");
        std::this_thread::sleep_for(5ms);
    }
}

TEST_F(ExchangeSources, PriceTickTasksDiscoverAndPublishOnlyObservedTradingDays)
{
    const auto raw = reference_fixture();
    auto fetch = [&](const ac::HttpRequest& request, auto) {
        return request.path.find("20220104") != std::string::npos ? ac::HttpReply{200, raw}
                                                                  : ac::HttpReply{404, {}};
    };
    ac::Manager manager(
        root,
        {{"czce", [&](auto stop) {
              return std::make_unique<ac::Exchange>("CZCE", budget, stop, fetch);
          }}});
    const auto options = manager.options();
    const auto& tables = options.tables();
    EXPECT_NE(
        std::find_if(
            tables.begin(),
            tables.end(),
            [](const auto& table) {
                return table.table() == ::warehouse::TABLE_PRICE_TICKS;
            }),
        tables.end());
    ac::wire::StartAcquisition request;
    request.set_id("ticks");
    request.set_table(::warehouse::TABLE_PRICE_TICKS);
    request.set_first_day("2022-01-03");
    request.set_last_day("2022-01-05");
    manager.submit(request);
    const auto task = wait(manager, "ticks");
    ASSERT_EQ(task.state(), ac::wire::TASK_STATE_SUCCEEDED) << task.error().message();
    EXPECT_EQ(task.items_done(), 3);
    EXPECT_EQ(task.items_empty(), 2);
    wh::Store store(wh::source_directory(root, "czce"), "czce");
    const auto original = store.current();
    ASSERT_EQ(original.partitions.size(), 1);
    const auto rows = store.read(original, "price_ticks/CZCE/2022-01-04");
    ASSERT_EQ(rows.size(), 4);
    EXPECT_EQ(rows[3][5], wh::Cell{std::string("0.20000000")});
    EXPECT_EQ(rows[0][4], wh::Cell{std::string("2022-01-05")});
    request.set_id("again");
    manager.submit(request);
    const auto again = wait(manager, "again");
    EXPECT_EQ(again.state(), ac::wire::TASK_STATE_SUCCEEDED) << again.error().message();
    // Without a calendar an empty answer is never taken as final: asked again.
    EXPECT_EQ(again.requests_sent(), 2);
    EXPECT_EQ(store.current().identity, original.identity);
}

TEST_F(ExchangeSources, ReceiptRecollectionReplacesAnIncompleteProviderPartitionAndKeepsItsVersion)
{
    const std::string key = "warehouse_receipts/SHFE/2018-11-13";
    wh::Store tushare(wh::source_directory(root, "tushare"), "tushare");
    auto writer = tushare.writer();
    writer.put(
        key,
        wh::rows(
            std::vector<wh::WarehouseReceipt>{
                {.exchange = "SHFE",
                    .product = "al",
                    .day = "2018-11-13",
                    .warehouse = std::string("中储洛阳"),
                    .volume = std::string("7275")}
            }),
        {"2018-11-13"});
    writer.commit();
    wh::Store research(wh::source_directory(root, "research"), "research");
    const auto fixed = wh::compose(root, research, wh::source_versions(root));
    ac::Manager manager(
        root,
        {{"shfe", [&](auto stop) {
              return std::make_unique<ac::Exchange>("SHFE", budget, stop, [](const auto&, auto) {
                  return ac::HttpReply{200, reference_fixture("shfe_receipts_20181113.json")};
              });
          }}});
    ac::wire::StartAcquisition request;
    request.set_id("receipts");
    request.set_table(::warehouse::TABLE_WAREHOUSE_RECEIPTS);
    request.add_exchanges("SHFE");
    request.set_first_day("2018-11-13");
    request.set_last_day("2018-11-13");
    request.set_refetch(true);
    manager.submit(request);
    const auto task = wait(manager, request.id());
    ASSERT_EQ(task.state(), ac::wire::TASK_STATE_SUCCEEDED) << task.error().message();
    const auto updated = wh::compose(root, research, wh::source_versions(root));
    EXPECT_EQ(updated.partitions.at(key).source, "shfe");
    EXPECT_EQ(research.read(updated, key).size(), 6);
    EXPECT_EQ(research.read(fixed, key).size(), 1);
    EXPECT_EQ(fixed.partitions.at(key).source, "tushare");
}

TEST_F(ExchangeSources, PriceTickTasksFillGapsAndKeepFixedVersions)
{
    auto raw = reference_fixture();
    bool available = false;
    auto fetch = [&](const ac::HttpRequest& request, auto) {
        return available || request.path.find("20220104") != std::string::npos
            ? ac::HttpReply{200, raw}
            : ac::HttpReply{404, {}};
    };
    ac::Manager manager(
        root,
        {{"czce", [&](auto stop) {
              return std::make_unique<ac::Exchange>("CZCE", budget, stop, fetch);
          }}});
    ac::wire::StartAcquisition request;
    request.set_id("ticks");
    request.set_table(::warehouse::TABLE_PRICE_TICKS);
    request.add_exchanges("CZCE");
    request.set_first_day("2022-01-04");
    request.set_last_day("2022-01-05");
    manager.submit(request);
    auto task = wait(manager, request.id());
    ASSERT_EQ(task.state(), ac::wire::TASK_STATE_SUCCEEDED) << task.error().message();
    EXPECT_EQ(task.items(), 2);
    EXPECT_EQ(task.items_empty(), 1);
    wh::Store research(wh::source_directory(root, "research"), "research");
    wh::compose(root, research, wh::source_versions(root));
    const auto fixed = research.current();
    ASSERT_TRUE(fixed.partitions.contains("price_ticks/CZCE/2022-01-04"));
    EXPECT_EQ(fixed.partitions.at("price_ticks/CZCE/2022-01-04").source, "czce");
    EXPECT_FALSE(fixed.partitions.contains("price_ticks/CZCE/2022-01-05"));

    // A day answered empty is filled by acquiring it again.
    available = true;
    request.set_id("fill-gap");
    request.set_refetch(true);
    manager.submit(request);
    task = wait(manager, request.id());
    ASSERT_EQ(task.state(), ac::wire::TASK_STATE_SUCCEEDED) << task.error().message();
    EXPECT_EQ(task.requests_sent(), 2);
    EXPECT_EQ(task.items_empty(), 0);
    wh::compose(root, research, wh::source_versions(root));
    EXPECT_TRUE(research.current().partitions.contains("price_ticks/CZCE/2022-01-05"));

    raw.replace(raw.find("1.00元/吨"), std::string("1.00元/吨").size(), "0.005元/吨");
    request.set_id("refetch");
    manager.submit(request);
    task = wait(manager, request.id());
    ASSERT_EQ(task.state(), ac::wire::TASK_STATE_SUCCEEDED) << task.error().message();
    EXPECT_EQ(task.requests_sent(), 2);
    wh::compose(root, research, wh::source_versions(root));
    EXPECT_EQ(
        research.read(fixed, "price_ticks/CZCE/2022-01-04")[0][5],
        wh::Cell{std::string("1.00000000")});
    EXPECT_EQ(
        research.read(research.current(), "price_ticks/CZCE/2022-01-04")[0][5],
        wh::Cell{std::string("0.00500000")});
}

TEST_F(ExchangeSources, TaskOwnsDatedPartitionsAndKeepsPublishedDataAfterFailure)
{
    std::atomic<bool> fail = false;
    auto fetch = [&](const ac::HttpRequest& request, auto) {
        if (request.path.find("20261029") != std::string::npos)
            return ac::HttpReply{fail ? 503 : 404, {}};
        if (request.path.find("20261028") != std::string::npos) {
            auto data = shfe_settlement;
            data.replace(data.find("20261030"), 8, "20261028");
            return ac::HttpReply{200, std::move(data)};
        }
        return ac::HttpReply{200, shfe_settlement};
    };
    ac::Manager manager(
        root,
        {{"shfe", [&](auto stop) {
              return std::make_unique<ac::Exchange>("SHFE", budget, stop, fetch);
          }}});
    ac::wire::StartAcquisition request;
    request.set_id("fees");
    request.set_table(::warehouse::TABLE_SETTLEMENT);
    request.set_first_day("2026-10-29");
    request.set_last_day("2026-10-30");
    manager.submit(request);
    auto task = wait(manager, "fees");
    ASSERT_EQ(task.state(), ac::wire::TASK_STATE_SUCCEEDED) << task.error().message();
    EXPECT_EQ(task.items_empty(), 1);
    wh::Store store(wh::source_directory(root, "shfe"), "shfe");
    const auto original = store.current();
    ASSERT_EQ(original.partitions.size(), 1);
    EXPECT_TRUE(original.partitions.contains("settlement/SHFE/2026-10-30"));
    request.set_id("again");
    manager.submit(request);
    // Only the missing publication is fetched again: without a calendar an
    // empty answer is never taken as final.
    EXPECT_EQ(wait(manager, "again").requests_sent(), 1);
    EXPECT_EQ(store.current().identity, original.identity);
    fail = true;
    request.set_id("partial");
    request.set_first_day("2026-10-28");
    request.set_refetch(true);
    manager.submit(request);
    const auto failed = wait(manager, "partial");
    EXPECT_EQ(failed.state(), ac::wire::TASK_STATE_FAILED);
    EXPECT_EQ(failed.error().code(), ::common::ERROR_CODE_UNAVAILABLE);
    EXPECT_EQ(failed.items_done(), 1);
    EXPECT_EQ(store.current().partitions.size(), 2);
    EXPECT_EQ(store.version(original.identity).partitions.size(), 1);
    request.set_id("wrong");
    request.add_exchanges("DCE");
    manager.submit(request);
    EXPECT_EQ(wait(manager, "wrong").error().code(), ::common::ERROR_CODE_INVALID_REQUEST);
}

// Dated text fixtures are excerpts of the official 2015/2016 publications.
// The small yearly ZIP is a fixture of the provider's GB18030 archive format.
TEST_F(ExchangeSources, HistoricalCzceFormatsKeepDatesAndReuseTheYearlyDownload)
{
    const auto file = [](const std::string& name) {
        std::ifstream input(fs::path(ASTERION_EXCHANGE_FIXTURES) / name, std::ios::binary);
        if (!input)
            throw std::runtime_error("missing exchange fixture");
        return std::string{std::istreambuf_iterator<char>(input), {}};
    };
    auto fetch = [&](const ac::HttpRequest& request, auto) {
        if (request.path.ends_with("datahistory2010.zip"))
            return ac::HttpReply{200, file("czce_2010.zip")};
        if (request.path.ends_with("/20150105.txt"))
            return ac::HttpReply{200, file("czce_2015.txt")};
        if (request.path.find("20161227") != std::string::npos)
            return ac::HttpReply{200, file("czce_2016.txt")};
        throw std::runtime_error("unexpected exchange fixture request");
    };
    ac::Exchange source("CZCE", budget, {}, fetch);
    const auto old = source.daily("CZCE", "2015-01-05");
    ASSERT_EQ(old.size(), 1);
    EXPECT_EQ(old[0].contract, "CZCE/cf/2015-01");
    EXPECT_EQ(std::get<std::string>(old[0].previous_settlement), "13185");
    const auto middle = source.daily("CZCE", "2016-12-27");
    ASSERT_EQ(middle.size(), 1);
    EXPECT_EQ(std::get<std::string>(middle[0].open_interest), "24980");
    const auto first = source.daily("CZCE", "2010-01-04");
    const auto second = source.daily("CZCE", "2010-01-05");
    ASSERT_EQ(first.size(), 1);
    ASSERT_EQ(second.size(), 1);
    EXPECT_EQ(first[0].day, "2010-01-04");
    EXPECT_EQ(second[0].day, "2010-01-05");
    EXPECT_EQ(std::get<std::string>(first[0].previous_settlement), "1100.25");
    EXPECT_EQ(source.requests(), 3);

    auto one = std::make_shared<ac::Budget>(1);
    std::stop_source stop;
    ac::Exchange paced("CZCE", one, stop.get_token(), fetch);
    EXPECT_FALSE(paced.daily("CZCE", "2010-01-04").empty());
    EXPECT_FALSE(paced.daily("CZCE", "2010-01-05").empty());
    std::jthread cancel([&] {
        std::this_thread::sleep_for(10ms);
        stop.request_stop();
    });
    EXPECT_THROW(paced.daily("CZCE", "2015-01-05"), ac::SourceError);

    for (const bool truncated : {false, true}) {
        auto raw = file("czce_2010.zip");
        if (truncated)
            raw.resize(raw.size() / 2);
        else
            raw[60] ^= 1;
        ac::Exchange broken("CZCE", budget, {}, [&](const auto&, auto) {
            return ac::HttpReply{200, raw};
        });
        EXPECT_THROW(broken.daily("CZCE", "2010-01-04"), ac::SourceError);
    }
}

// Explicit acceptance only: each selected provider makes real public requests
// and publishes to a temporary warehouse. Never runs in the offline suite.
TEST_F(ExchangeSources, PublicFilesPublishThroughNativeTaskManager)
{
    const auto* selected = std::getenv("ASTERION_EXCHANGE_ACCEPTANCE_SOURCE");
    const auto* date = std::getenv("ASTERION_EXCHANGE_ACCEPTANCE_DAY");
    if (!selected || !date)
        GTEST_SKIP() << "set a public exchange and date for online acceptance";
    const std::string exchange(selected), day(date);
    std::string provider = exchange;
    std::transform(provider.begin(), provider.end(), provider.begin(), [](unsigned char c) {
        return std::tolower(c);
    });
    auto online_budget = std::make_shared<ac::Budget>(45);
    ac::Manager manager(
        root,
        {{provider, [exchange, online_budget](auto stop) {
              return std::make_unique<ac::Exchange>(exchange, online_budget, stop);
          }}});
    const auto* selected_table = std::getenv("ASTERION_EXCHANGE_ACCEPTANCE_TABLE");
    bool checked = false;
    for (const auto* table :
        {"daily",
            "settlement",
            "price_ticks",
            "member_positions",
            "warehouse_receipts",
            "inventory"}) {
        if (selected_table && std::string_view(selected_table) != table)
            continue;
        if (!ac::Exchange(exchange, online_budget, {}).supports(table, exchange))
            continue;
        ac::wire::StartAcquisition request;
        request.set_id(table);
        request.set_table(asterion::protocol::table_of(table));
        request.set_first_day(day);
        request.set_last_day(day);
        manager.submit(request);
        // The production transport has its own bounded deadline.
        ac::wire::AcquisitionTask task;
        do {
            std::this_thread::sleep_for(50ms);
            task = manager.get(table);
        } while (task.state() == ac::wire::TASK_STATE_QUEUED
            || task.state() == ac::wire::TASK_STATE_RUNNING);
        ASSERT_EQ(task.state(), ac::wire::TASK_STATE_SUCCEEDED) << task.error().message();
        ASSERT_EQ(task.items_empty(), 0) << "select a published trading day";
        wh::Store store(wh::source_directory(root, provider), provider);
        const auto rows =
            store.read(store.current(), std::string(table) + "/" + exchange + "/" + day);
        ASSERT_FALSE(rows.empty());
        checked = true;
        std::cout << exchange << " " << table << " " << day << ": " << rows.size() << " rows\n";
    }
    EXPECT_TRUE(checked) << exchange << " has no public files for "
                         << (selected_table ? selected_table : "the requested tables");
}

} // namespace
