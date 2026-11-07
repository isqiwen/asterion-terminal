#include "research.hpp"
#include <asterion/protocol/warehouse.hpp>
#include "acquisition/exchanges.hpp"
#include "acquisition/manager.hpp"
#include <asterion/system/durable_file.hpp>
#include <asterion/system/process/child.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <future>

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

std::string reference_fixture()
{
    std::ifstream file(
        fs::path(ASTERION_EXCHANGE_FIXTURES) / "czce_reference_20220104.xml",
        std::ios::binary);
    if (!file)
        throw std::runtime_error("missing CZCE reference fixture");
    return {std::istreambuf_iterator<char>(file), {}};
}

class ExchangeSources : public testing::Test
{
protected:
    fs::path root = fs::temp_directory_path() / ("asterion-exchange-" + unique_process_id());

    void TearDown() override
    {
        std::error_code error;
        fs::remove_all(root, error);
    }
};

TEST_F(ExchangeSources, ExactDecimalsMissingPricesAndFeeBasesReachParquet)
{
    for (const auto* exchange : {"SHFE", "INE"}) {
        ac::Exchange source(exchange, {}, [](const ac::HttpRequest& request, auto) {
            return ac::HttpReply{
                200,
                request.path.find("/js") != std::string::npos ? shfe_settlement : shfe_daily
            };
        });
        auto daily = source.daily(exchange, "2026-10-30");
        ASSERT_EQ(daily.size(), 1);
        EXPECT_EQ(daily[0][7], wh::Cell{});
        EXPECT_EQ(std::get<std::string>(daily[0][8]), "3399.123456");
        EXPECT_EQ(std::get<std::string>(daily[0][11]), "12345678901234567890");
        EXPECT_EQ(daily[0][12], wh::Cell{}); // Preserve unverified turnover units as absent.
        const auto fees = source.settlement(exchange, "2026-10-30");
        EXPECT_EQ(std::get<std::string>(fees[0][6]), "0.00005");
        EXPECT_EQ(std::get<std::string>(fees[0][8]), "0.000025");
        wh::Store store(root / exchange, "exchange");
        auto writer = store.writer();
        writer.put(std::string("daily/") + exchange + "/2026-10-30", daily);
        writer.put(std::string("settlement/") + exchange + "/2026-10-30", fees);
        auto version = writer.commit();
        EXPECT_EQ(
            std::get<std::string>(
                store.read(version, std::string("daily/") + exchange + "/2026-10-30")[0][11]),
            "12345678901234567890.000000");
    }
    ac::Exchange czce("CZCE", {}, [](const auto&, auto) {
        return ac::HttpReply{200, czce_settlement};
    });
    auto rows = czce.settlement("CZCE", "2026-10-30");
    ASSERT_EQ(rows.size(), 2);
    EXPECT_EQ(std::get<std::string>(rows[0][2]), "CZCE/ma/2027-01");
    EXPECT_EQ(std::get<std::string>(rows[0][4]), "0.08");
    EXPECT_EQ(std::get<std::string>(rows[0][6]), "0.00005");
    EXPECT_EQ(std::get<std::string>(rows[0][9]), "0");
    EXPECT_EQ(std::get<std::string>(rows[1][7]), "5");
    EXPECT_EQ(std::get<std::string>(rows[1][9]), "10");
}

TEST_F(ExchangeSources, CffexEncodingAndCzceDailyColumnsExcludeOptions)
{
    std::ifstream file(fs::path(ASTERION_EXCHANGE_FIXTURES) / "cffex_daily.csv", std::ios::binary);
    ASSERT_TRUE(file.good());
    const std::string csv{std::istreambuf_iterator<char>(file), {}};
    ac::Exchange cffex("CFFEX", {}, [&](const ac::HttpRequest& request, auto) {
        EXPECT_EQ(request.path, "/sj/hqsj/rtj/202610/30/20261030_1.csv");
        return ac::HttpReply{200, csv};
    });
    const auto rows = cffex.daily("CFFEX", "2026-10-30");
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(std::get<std::string>(rows[0][2]), "CFFEX/ic/2026-11");
    ac::Exchange czce("CZCE", {}, [](const auto&, auto) {
        return ac::HttpReply{
            200,
            "郑州商品交易所期货每日行情表(2026-10-30)\n" "合约代码|昨结算|今开盘|最高价|最低价|今收盘|今结算|成交量(手)|持仓量\n" "MA701|2,100.50|0|0|0|2,101|2,100|3,000|1,500\n"
        };
    });
    const auto daily = czce.daily("CZCE", "2026-10-30");
    ASSERT_EQ(daily.size(), 1);
    EXPECT_EQ(daily[0][7], wh::Cell{});
    EXPECT_EQ(std::get<std::string>(daily[0][10]), "2100.5");
    EXPECT_EQ(std::get<std::string>(daily[0][11]), "3000");
}

TEST_F(ExchangeSources, PortalsKeepSourceIdentityAndDoNotRequestUnsupportedTables)
{
    for (const auto* exchange : {"DCE", "GFEX"}) {
        ac::Exchange source(exchange, {}, [&](const ac::HttpRequest& request, auto) {
            EXPECT_FALSE(request.body.empty());
            EXPECT_EQ(
                request.origin,
                std::string(
                    exchange == std::string("DCE") ? "http://www.dce.com.cn"
                                                   : "http://www.gfex.com.cn"));
            return ac::HttpReply{200, R"({"code":"0","param":{"trade_date":["20261030"]},"data":[
                {"variety":"玉米","contractId":"c2701","varietyOrder":"si","delivMonth":"2701",
                "open":1200.1,"high":1250,"low":1190,"close":1210,"clearPrice":1208,
                "lastClear":1200,"volumn":100,"openInterest":300}]})"};
        });
        const auto rows = source.daily(exchange, "2026-10-30");
        ASSERT_EQ(rows.size(), 1);
        EXPECT_EQ(std::get<std::string>(rows[0][0]), exchange);
        EXPECT_EQ(rows[0][12], wh::Cell{});
        EXPECT_THROW(source.settlement(exchange, "2026-10-30"), ac::SourceError);
        EXPECT_EQ(source.requests(), 1);
    }
}

TEST_F(ExchangeSources, MissingPublicationIsUncoveredButRefusalAndWrongDateFail)
{
    for (const auto status : {404, 412, 503, 302}) {
        ac::Exchange source("SHFE", {}, [&](const auto&, auto) {
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
    ac::Exchange wrong("SHFE", {}, [](const auto&, auto) {
        return ac::HttpReply{200, shfe_daily};
    });
    EXPECT_THROW(wrong.daily("SHFE", "2026-10-29"), ac::SourceError);
    ac::Exchange invalid("SHFE", {}, [](const auto&, auto) {
        return ac::HttpReply{200, "<html>challenge</html>"};
    });
    EXPECT_THROW(invalid.daily("SHFE", "2026-10-30"), ac::SourceError);
    std::stop_source cancel;
    cancel.request_stop();
    ac::Exchange stopped("SHFE", cancel.get_token(), [](const auto&, auto) -> ac::HttpReply {
        ADD_FAILURE() << "cancelled request was sent";
        return {200, shfe_daily};
    });
    EXPECT_THROW(stopped.daily("SHFE", "2026-10-30"), ac::SourceError);
}

TEST_F(ExchangeSources, HistoricalPriceTicksKeepExactValuesAndOneDayOfEvidence)
{
    auto raw = reference_fixture();
    raw.insert(raw.find("</Contracts>"), "<Contract><PrdTp>期权</PrdTp></Contract>");
    ac::Exchange source("CZCE", {}, [&](const ac::HttpRequest& request, auto) {
        EXPECT_EQ(request.origin, "https://www.czce.com.cn");
        EXPECT_EQ(
            request.path,
            "/cn/DFSStaticFiles/Future/2022/20220104/FutureDataReferenceData.xml");
        return ac::HttpReply{200, raw};
    });
    const auto rows = source.price_ticks("CZCE", "2022-01-04");
    ASSERT_EQ(rows.size(), 4);
    EXPECT_EQ(rows[0][2], wh::Cell{std::string("CZCE/ap/2022-01")});
    EXPECT_EQ(rows[0][5], wh::Cell{std::string("1")});
    EXPECT_EQ(rows[1][5], wh::Cell{std::string("5")});
    EXPECT_EQ(rows[2][5], wh::Cell{std::string("2")});
    EXPECT_EQ(rows[3][5], wh::Cell{std::string("0.2")});
    for (const auto& row : rows) {
        EXPECT_EQ(row[3], wh::Cell{std::string("2022-01-04")});
        EXPECT_EQ(row[4], wh::Cell{std::string("2022-01-05")});
        EXPECT_EQ(
            row[6],
            wh::Cell{
                std::string(
                    "https://www.czce.com.cn/cn/DFSStaticFiles/Future/2022/20220104/FutureDataReferenceData.xml")
            });
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
        ac::Exchange source("CZCE", {}, [&](const auto&, auto) {
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
    ac::Manager manager(root, {{"czce", [&](auto stop) {
                                    return std::make_unique<ac::Exchange>("CZCE", stop, fetch);
                                }}});
    const auto options = manager.options();
    const auto& tables = options.tables();
    EXPECT_NE(
        std::find(tables.begin(), tables.end(), ::warehouse::TABLE_PRICE_TICKS),
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
    EXPECT_EQ(again.requests_sent(), 2);
    EXPECT_EQ(store.current().identity, original.identity);
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
    ac::Manager manager(root, {{"czce", [&](auto stop) {
                                    return std::make_unique<ac::Exchange>("CZCE", stop, fetch);
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
    wh::compose_research(root, research);
    const auto fixed = research.current();
    ASSERT_TRUE(fixed.partitions.contains("price_ticks/CZCE/2022-01-04"));
    EXPECT_EQ(fixed.partitions.at("price_ticks/CZCE/2022-01-04").source, "czce");
    EXPECT_FALSE(fixed.partitions.contains("price_ticks/CZCE/2022-01-05"));

    available = true;
    request.set_id("fill-gap");
    manager.submit(request);
    task = wait(manager, request.id());
    ASSERT_EQ(task.state(), ac::wire::TASK_STATE_SUCCEEDED) << task.error().message();
    EXPECT_EQ(task.requests_sent(), 1);
    EXPECT_EQ(task.items_empty(), 0);
    wh::compose_research(root, research);
    EXPECT_TRUE(research.current().partitions.contains("price_ticks/CZCE/2022-01-05"));

    raw.replace(raw.find("1.00元/吨"), std::string("1.00元/吨").size(), "0.005元/吨");
    request.set_id("refetch");
    request.set_refetch(true);
    manager.submit(request);
    task = wait(manager, request.id());
    ASSERT_EQ(task.state(), ac::wire::TASK_STATE_SUCCEEDED) << task.error().message();
    EXPECT_EQ(task.requests_sent(), 2);
    wh::compose_research(root, research);
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
    ac::Manager manager(root, {{"shfe", [&](auto stop) {
                                    return std::make_unique<ac::Exchange>("SHFE", stop, fetch);
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
    EXPECT_EQ(
        wait(manager, "again").requests_sent(),
        1); // Only the missing publication is fetched again.
    EXPECT_EQ(store.current().identity, original.identity);
    fail = true;
    request.set_id("partial");
    request.set_first_day("2026-10-28");
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
    ac::Exchange source("CZCE", {}, [&](const ac::HttpRequest& request, auto) {
        if (request.path.ends_with("datahistory2010.zip"))
            return ac::HttpReply{200, file("czce_2010.zip")};
        if (request.path.ends_with("/20150105.txt"))
            return ac::HttpReply{200, file("czce_2015.txt")};
        if (request.path.find("20161227") != std::string::npos)
            return ac::HttpReply{200, file("czce_2016.txt")};
        throw std::runtime_error("unexpected exchange fixture request");
    });
    const auto old = source.daily("CZCE", "2015-01-05");
    ASSERT_EQ(old.size(), 1);
    EXPECT_EQ(std::get<std::string>(old[0][2]), "CZCE/cf/2015-01");
    EXPECT_EQ(std::get<std::string>(old[0][10]), "13185");
    const auto middle = source.daily("CZCE", "2016-12-27");
    ASSERT_EQ(middle.size(), 1);
    EXPECT_EQ(std::get<std::string>(middle[0][13]), "24980");
    const auto first = source.daily("CZCE", "2010-01-04");
    const auto second = source.daily("CZCE", "2010-01-05");
    ASSERT_EQ(first.size(), 1);
    ASSERT_EQ(second.size(), 1);
    EXPECT_EQ(first[0][3], wh::Cell{std::string("2010-01-04")});
    EXPECT_EQ(second[0][3], wh::Cell{std::string("2010-01-05")});
    EXPECT_EQ(std::get<std::string>(first[0][10]), "1100.25");
    EXPECT_EQ(source.requests(), 3);
    for (const bool truncated : {false, true}) {
        auto raw = file("czce_2010.zip");
        if (truncated)
            raw.resize(raw.size() / 2);
        else
            raw[60] ^= 1;
        ac::Exchange broken("CZCE", {}, [&](const auto&, auto) {
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
    ac::Manager manager(root, {{provider, [exchange](auto stop) {
                                    return std::make_unique<ac::Exchange>(exchange, stop);
                                }}});
    for (const auto* table : {"daily", "settlement", "price_ticks"}) {
        const auto* selected_table = std::getenv("ASTERION_EXCHANGE_ACCEPTANCE_TABLE");
        if (selected_table && std::string_view(selected_table) != table)
            continue;
        if (!ac::Exchange(exchange, {}).supports(table, exchange))
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
        std::cout << exchange << " " << table << " " << day << ": " << rows.size() << " rows\n";
        if (std::string_view(table) != "price_ticks"
            && ac::Exchange(exchange, {}).supports("price_ticks", exchange)) {
            const auto ticks = store.read(store.current(), "price_ticks/" + exchange + "/" + day);
            ASSERT_FALSE(ticks.empty());
            std::cout << exchange << " auto price_ticks " << day << ": " << ticks.size()
                      << " rows\n";
        }
    }
}

} // namespace
