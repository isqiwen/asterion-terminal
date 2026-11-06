#include "acquisition/manager.hpp"
#include "acquisition/sina.hpp"
#include "warehouse/research.hpp"
#include <asterion/quotes.pb.h>
#include <asterion/system/process/child.hpp>
#include <gtest/gtest.h>
#include <cstdlib>
#include <fstream>
#include <thread>

namespace {

using namespace asterion;
namespace ac = asterion::history::acquisition;
namespace fs = std::filesystem;

class SinaSource : public testing::Test
{
protected:
    fs::path root = fs::temp_directory_path() / ("sina-source-" + unique_process_id());
    std::vector<std::string> asked;
    std::string body = "/*<script>location.href='//sina.com';</script>*/\nvar _x=([])";
    int status = 200;

    void TearDown() override
    {
        std::error_code ignored;
        fs::remove_all(root, ignored);
    }

    void snapshot(const std::string& day, const std::vector<std::string>& contracts)
    {
        ::quotes::Catalog catalog;
        for (const auto& contract : contracts)
            catalog.add_contracts()->set_contract(contract);
        fs::create_directories(root);
        std::ofstream(root / (day + ".pb"), std::ios::binary) << catalog.SerializeAsString();
    }

    ac::Sina source()
    {
        return ac::Sina(
            {"SHFE", "CZCE", "CFFEX"},
            root,
            std::make_shared<ac::Budget>(500),
            {},
            [this](const ac::HttpRequest& request, auto) {
                asked.push_back(request.path);
                return ac::HttpReply{status, body};
            });
    }
};

TEST_F(SinaSource, ADaysDailyBarsCoverThatDaysSnapshotAndOtherwiseStayMissing)
{
    body =
        R"(var _x=([{"d":"2026-11-02","o":"3254.000","h":"3266.000","l":"3234.000",)" R"("c":"3242.000","v":"377","p":"321","s":"3246.000"}]))";
    auto sina = source();
    EXPECT_TRUE(sina.daily("SHFE", "2026-11-02").empty());
    EXPECT_TRUE(asked.empty()) << "without a snapshot the day would look whole";
    snapshot("20261102", {"SHFE/rb/2027-01", "CZCE/ap/2027-01"});
    const auto rows = sina.daily("SHFE", "2026-11-02");
    ASSERT_EQ(rows.size(), 1);
    EXPECT_EQ(std::get<std::string>(rows[0][2]), "SHFE/rb/2027-01");
    EXPECT_EQ(std::get<std::string>(rows[0][7]), "3242.000");
    EXPECT_EQ(std::get<std::string>(rows[0][8]), "3246.000");
    EXPECT_EQ(std::get<std::string>(rows[0][11]), "377");
    EXPECT_EQ(std::get<std::string>(rows[0][13]), "321");
    EXPECT_TRUE(std::holds_alternative<std::monostate>(rows[0][12]));
    ASSERT_EQ(asked.size(), 1);
    EXPECT_NE(asked[0].find("symbol=RB2701"), std::string::npos);
    sina.daily("CZCE", "2026-11-02");
    EXPECT_NE(asked[1].find("symbol=AP2701"), std::string::npos);
}

TEST_F(SinaSource, NightMinutesBelongToTheNextTradingDayAndAreLabelledByTheirStart)
{
    // Friday night through Saturday morning, then Monday's day session.
    body =
        R"(var _x=([)" R"({"d":"2026-11-06 21:01:00","o":"1","h":"2","l":"1","c":"2","v":"3","p":"4"},)" R"({"d":"2026-11-07 00:30:00","o":"1","h":"2","l":"1","c":"2","v":"3","p":"4"},)" R"({"d":"2026-11-09 09:01:00","o":"1","h":"2","l":"1","c":"2","v":"3","p":"4"}]))";
    auto sina = source();
    const auto rows = sina.minutes("SHFE/rb/2027-01", "2026-11-09");
    ASSERT_EQ(rows.size(), 3);
    EXPECT_EQ(std::get<std::int64_t>(rows[0][4]), 1793970000000); // 21:00 Beijing.
    EXPECT_FALSE(std::get<bool>(rows[0][5]));
    EXPECT_TRUE(sina.minutes("SHFE/rb/2027-01", "2026-11-06").empty());
    EXPECT_EQ(asked.size(), 1);
}

TEST_F(SinaSource, AFullAnswersOldestDayIsMarkedPartial)
{
    std::string rows;
    for (int i = 0; i < 1023; ++i)
        rows += std::string(i ? "," : "") + R"({"d":")"
            + (i ? "2026-11-05 09:01:00" : "2026-11-04 14:59:00")
            + R"(","o":"1","h":"2","l":"1","c":"2","v":"3","p":"4"})";
    body = "var _x=([" + rows + "])";
    auto sina = source();
    EXPECT_TRUE(std::get<bool>(sina.minutes("SHFE/rb/2027-01", "2026-11-04")[0][5]));
    EXPECT_FALSE(std::get<bool>(sina.minutes("SHFE/rb/2027-01", "2026-11-05")[0][5]));
}

TEST_F(SinaSource, UnknownContractsAreEmptyAndRefusalsFailAndCffexIsNotOffered)
{
    body = "var _x=(null);";
    auto sina = source();
    EXPECT_TRUE(sina.minutes("SHFE/rb/2027-01", "2026-11-05").empty());
    EXPECT_FALSE(sina.supports("daily", "CFFEX"));
    EXPECT_FALSE(sina.supports("settlement", "SHFE"));
    status = 456;
    EXPECT_THROW(sina.minutes("SHFE/rb/2027-02", "2026-11-05"), ac::SourceError);
    status = 200;
    body = "<html>";
    EXPECT_THROW(sina.minutes("SHFE/rb/2027-03", "2026-11-05"), ac::SourceError);
}

TEST_F(SinaSource, RealMinutesAndDailyBarsParse)
{
    const auto* contract = std::getenv("ASTERION_SINA_ACCEPTANCE");
    if (!contract)
        GTEST_SKIP() << "set ASTERION_SINA_ACCEPTANCE=SHFE/rb/2701-style contract to fetch";
    const auto* day = std::getenv("ASTERION_SINA_ACCEPTANCE_DAY");
    ASSERT_NE(day, nullptr);
    ac::Sina sina(
        {"SHFE", "CZCE", "DCE", "INE", "GFEX"},
        root,
        std::make_shared<ac::Budget>(60),
        {});
    const auto minutes = sina.minutes(contract, day);
    ASSERT_FALSE(minutes.empty());
    std::cout << contract << " " << day << ": " << minutes.size() << " minutes, first close "
              << std::get<std::string>(minutes[0][9]) << "\n";
    auto compact = std::string(day);
    std::erase(compact, '-');
    snapshot(compact, {contract});
    const auto exchange = std::string(contract).substr(0, std::string(contract).find('/'));
    const auto bars = sina.daily(exchange, day);
    ASSERT_EQ(bars.size(), 1);
    std::cout << "daily close " << std::get<std::string>(bars[0][7]) << " settle "
              << std::get<std::string>(bars[0][8]) << "\n";
    // The whole way: no Tushare here, so the task falls through to Sina.
    const auto data = root / "market";
    ac::Manager manager(data, {{"sina", [&](std::stop_token stop) {
                                    return std::make_unique<ac::Sina>(
                                        std::vector<std::string>{exchange},
                                        root,
                                        std::make_shared<ac::Budget>(60),
                                        stop);
                                }}});
    ac::wire::StartAcquisition request;
    request.set_id("real");
    request.set_table(::history::TABLE_MINUTES);
    request.add_contracts(contract);
    request.set_first_day(day);
    request.set_last_day(day);
    manager.submit(request);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    auto task = manager.get("real");
    while (task.state() != ac::wire::TASK_STATE_SUCCEEDED
        && task.state() != ac::wire::TASK_STATE_FAILED
        && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        task = manager.get("real");
    }
    ASSERT_EQ(task.state(), ac::wire::TASK_STATE_SUCCEEDED) << task.error().message();
    asterion::history::warehouse::Store research(
        asterion::history::warehouse::source_directory(data, "research"),
        "research");
    const auto version = asterion::history::warehouse::compose_research(data, research);
    ASSERT_EQ(version.partitions.size(), 1);
    EXPECT_EQ(version.partitions.begin()->second.source, "sina");
    std::cout << version.partitions.begin()->first << " from "
              << version.partitions.begin()->second.source << ", "
              << version.partitions.begin()->second.rows << " rows\n";
}

} // namespace
