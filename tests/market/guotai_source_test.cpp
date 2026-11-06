#include "warehouse/research.hpp"
#include <asterion/protocol/market.hpp>
#include "acquisition/guotai.hpp"
#include "acquisition/manager.hpp"
#include <asterion/system/process/child.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <thread>

namespace {

using namespace asterion;
using namespace std::chrono_literals;
namespace ac = market_data::acquisition;
namespace wh = market_data::warehouse;
namespace fs = std::filesystem;
const std::vector<std::string> exchanges{"SHFE", "INE", "DCE", "CFFEX", "GFEX"};

std::string fixture(const std::string& date)
{
    std::ifstream input(fs::path(ASTERION_EXCHANGE_FIXTURES) / ("gtja_" + date + ".html"));
    if (!input)
        throw std::runtime_error("missing Guotai fixture");
    return {std::istreambuf_iterator<char>(input), {}};
}

ac::HttpReply fetch(const ac::HttpRequest& request, std::stop_token)
{
    EXPECT_EQ(request.origin, "https://www.gtjaqh.com");
    return {200, fixture(request.path.substr(request.path.find('=') + 1))};
}

wh::Row product(const ac::Rows& rows, const std::string& name)
{
    for (const auto& row : rows)
        if (row[1] == wh::Cell{name})
            return row;
    throw std::runtime_error("missing product " + name);
}

ac::wire::AcquisitionTask wait(ac::Manager& manager, const std::string& id)
{
    const auto until = std::chrono::steady_clock::now() + 120s;
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

class GuotaiSource : public testing::Test
{
protected:
    fs::path root = fs::temp_directory_path() / ("asterion-guotai-" + unique_process_id());

    void TearDown() override
    {
        std::error_code error;
        fs::remove_all(root, error);
    }
};

TEST_F(GuotaiSource, DatedReportsCoverFiveExchangesAndKeepHistoricalChanges)
{
    ac::Guotai source(exchanges, {}, fetch);
    const std::vector<std::pair<std::string, std::string>> dates{
        {"2023-11-06", "0.005"},
        {"2023-11-07", "0.002"},
        {"2024-12-17", "0.002"},
        {"2024-12-18", "0.002"}
    };
    for (const auto& [day, bond_tick] : dates) {
        SCOPED_TRACE(day);
        for (const auto& exchange : exchanges) {
            const auto rows = source.price_ticks(exchange, day);
            ASSERT_FALSE(rows.empty());
            for (const auto& row : rows) {
                EXPECT_EQ(row[0], wh::Cell{exchange});
                EXPECT_EQ(row[2], wh::Cell{});
                EXPECT_EQ(row[3], wh::Cell{day});
                EXPECT_EQ(row[4], wh::Cell{ac::day_text(ac::day_of(day) + std::chrono::days(1))});
                auto date = day;
                std::erase(date, '-');
                EXPECT_EQ(row[6], wh::Cell{"https://www.gtjaqh.com/pc/calendar?date=" + date});
                EXPECT_EQ(std::get<std::string>(row[1]).find('_'), std::string::npos);
            }
            if (exchange == "CFFEX")
                EXPECT_EQ(product(rows, "ts")[5], wh::Cell{bond_tick});
            if (exchange == "GFEX") {
                EXPECT_EQ(rows.size(), 2);
                EXPECT_EQ(
                    product(rows, "lc")[5],
                    wh::Cell{std::string(day < "2024-12-18" ? "50" : "20")});
            }
            if (exchange == "SHFE")
                EXPECT_EQ(product(rows, "au")[5], wh::Cell{std::string("0.02")});
        }
    }
    EXPECT_EQ(source.requests(), dates.size());
    EXPECT_FALSE(source.supports("price_ticks", "CZCE"));
}

TEST_F(GuotaiSource, MissingHistoryStaysUncoveredAndWrongDatedReportsFail)
{
    ac::Guotai old(exchanges, {}, fetch);
    EXPECT_TRUE(old.price_ticks("SHFE", "2020-06-01").empty());
    ac::Guotai missing(exchanges, {}, [](const auto&, auto) {
        return ac::HttpReply{404, {}};
    });
    EXPECT_TRUE(missing.price_ticks("SHFE", "2015-01-05").empty());
    ac::Guotai wrong(exchanges, {}, [](const auto&, auto) {
        return ac::HttpReply{200, fixture("20241218")};
    });
    EXPECT_THROW(wrong.price_ticks("GFEX", "2024-12-17"), ac::SourceError);
    ac::Guotai refused(exchanges, {}, [](const auto&, auto) {
        return ac::HttpReply{503, {}};
    });
    EXPECT_THROW(refused.price_ticks("GFEX", "2024-12-17"), ac::SourceError);
}

TEST_F(GuotaiSource, MarketPublishesFiveExchangesAndSkipsStoredDays)
{
    ac::Manager manager(root, {{"gtja", [](auto stop) {
                                    return std::make_unique<ac::Guotai>(exchanges, stop, fetch);
                                }}});
    ac::wire::StartAcquisition request;
    request.set_id("ticks");
    request.set_table(::market::TABLE_PRICE_TICKS);
    request.set_first_day("2024-12-17");
    request.set_last_day("2024-12-18");
    manager.submit(request);
    auto task = wait(manager, request.id());
    ASSERT_EQ(task.state(), ac::wire::TASK_STATE_SUCCEEDED) << task.error().message();
    EXPECT_EQ(task.requests_sent(), 2);
    EXPECT_EQ(task.items_done(), 10);
    wh::Store research(wh::source_directory(root, "research"), "research");
    wh::compose_research(root, research);
    const auto fixed = research.current();
    ASSERT_EQ(fixed.partitions.size(), 10);
    for (const auto& [key, part] : fixed.partitions)
        EXPECT_EQ(part.source, "gtja");
    EXPECT_EQ(
        product(research.read(fixed, "price_ticks/GFEX/2024-12-17"), "lc")[5],
        wh::Cell{std::string("50.00000000")});
    EXPECT_EQ(
        product(research.read(fixed, "price_ticks/GFEX/2024-12-18"), "lc")[5],
        wh::Cell{std::string("20.00000000")});
    request.set_id("again");
    manager.submit(request);
    task = wait(manager, request.id());
    EXPECT_EQ(task.state(), ac::wire::TASK_STATE_SUCCEEDED) << task.error().message();
    EXPECT_EQ(task.requests_sent(), 0);
    EXPECT_EQ(wh::compose_research(root, research).identity, fixed.identity);
}

TEST_F(GuotaiSource, PublicReportsPublishHistoricalChangesThroughMarket)
{
    if (!std::getenv("ASTERION_GUOTAI_ACCEPTANCE"))
        GTEST_SKIP() << "set ASTERION_GUOTAI_ACCEPTANCE=1 to fetch public historical reports";
    ac::Manager manager(root, {{"gtja", [](auto stop) {
                                    return std::make_unique<ac::Guotai>(exchanges, stop);
                                }}});
    wh::Store research(wh::source_directory(root, "research"), "research");
    wh::compose_research(root, research);
    for (const auto* first : {"2023-11-06", "2024-12-17"}) {
        ac::wire::StartAcquisition request;
        request.set_id(first);
        request.set_table(::market::TABLE_PRICE_TICKS);
        request.set_first_day(first);
        request.set_last_day(ac::day_text(ac::day_of(first) + std::chrono::days(1)));
        manager.submit(request);
        const auto task = wait(manager, request.id());
        ASSERT_EQ(task.state(), ac::wire::TASK_STATE_SUCCEEDED) << task.error().message();
        EXPECT_EQ(task.requests_sent(), 2);
        EXPECT_EQ(task.items_empty(), 0);
    }
    const auto fixed = research.current();
    ASSERT_EQ(fixed.partitions.size(), 20);
    const auto tick =
        [&](const std::string& exchange, const std::string& name, const std::string& day) {
            return product(research.read(fixed, "price_ticks/" + exchange + "/" + day), name)[5];
        };
    EXPECT_EQ(tick("CFFEX", "ts", "2023-11-06"), wh::Cell{std::string("0.00500000")});
    EXPECT_EQ(tick("CFFEX", "ts", "2023-11-07"), wh::Cell{std::string("0.00200000")});
    EXPECT_EQ(tick("GFEX", "lc", "2024-12-17"), wh::Cell{std::string("50.00000000")});
    EXPECT_EQ(tick("GFEX", "lc", "2024-12-18"), wh::Cell{std::string("20.00000000")});
    for (const auto& [key, part] : fixed.partitions)
        EXPECT_EQ(part.source, "gtja");
    std::cout << "Published " << fixed.partitions.size() << " real price tick partitions\n";
}

} // namespace
