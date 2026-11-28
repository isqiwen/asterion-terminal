#include "composition.hpp"
#include <asterion/protocol/warehouse.hpp>
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
namespace ac = asterion::warehouse::acquisition;
namespace wh = asterion::warehouse;
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

wh::Row product(const std::vector<wh::Row>& rows, const std::string& name)
{
    for (const auto& row : rows)
        if (row[1] == wh::Cell{name})
            return row;
    throw std::runtime_error("missing product " + name);
}

std::string tick(const std::vector<wh::PriceTick>& rows, const std::string& name)
{
    for (const auto& row : rows)
        if (row.product == name)
            return row.price_tick.str();
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
    void TearDown() override
    {
        std::error_code error;
        fs::remove_all(root, error);
    }

    fs::path root = fs::temp_directory_path() / ("asterion-guotai-" + unique_process_id());
    std::shared_ptr<ac::Budget> budget = std::make_shared<ac::Budget>(500);
};

TEST_F(GuotaiSource, DatedReportsCoverFiveExchangesAndKeepHistoricalChanges)
{
    ac::Guotai source(exchanges, budget, {}, fetch);
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
                EXPECT_EQ(row.exchange, exchange);
                EXPECT_EQ(row.contract, wh::Cell{});
                EXPECT_EQ(row.effective_from, day);
                EXPECT_EQ(
                    row.effective_until,
                    ac::day_text(ac::day_of(day) + std::chrono::days(1)));
                auto date = day;
                std::erase(date, '-');
                EXPECT_EQ(row.source_url, "https://www.gtjaqh.com/pc/calendar?date=" + date);
                EXPECT_EQ(row.product.find('_'), std::string::npos);
            }
            if (exchange == "CFFEX")
                EXPECT_EQ(tick(rows, "ts"), bond_tick);
            if (exchange == "GFEX") {
                EXPECT_EQ(rows.size(), 2);
                EXPECT_EQ(tick(rows, "lc"), day < "2024-12-18" ? "50" : "20");
            }
            if (exchange == "SHFE")
                EXPECT_EQ(tick(rows, "au"), "0.02");
        }
    }
    EXPECT_EQ(source.requests(), dates.size());
    EXPECT_FALSE(source.supports("price_ticks", "CZCE"));
}

TEST_F(GuotaiSource, MissingHistoryStaysUncoveredAndWrongDatedReportsFail)
{
    ac::Guotai old(exchanges, budget, {}, fetch);
    EXPECT_TRUE(old.price_ticks("SHFE", "2020-06-01").empty());
    ac::Guotai missing(exchanges, budget, {}, [](const auto&, auto) {
        return ac::HttpReply{404, {}};
    });
    EXPECT_TRUE(missing.price_ticks("SHFE", "2015-01-05").empty());
    ac::Guotai wrong(exchanges, budget, {}, [](const auto&, auto) {
        return ac::HttpReply{200, fixture("20241218")};
    });
    EXPECT_THROW(wrong.price_ticks("GFEX", "2024-12-17"), ac::SourceError);
    ac::Guotai refused(exchanges, budget, {}, [](const auto&, auto) {
        return ac::HttpReply{503, {}};
    });
    EXPECT_THROW(refused.price_ticks("GFEX", "2024-12-17"), ac::SourceError);

    auto one = std::make_shared<ac::Budget>(1);
    std::stop_source stop;
    ac::Guotai paced(exchanges, one, stop.get_token(), fetch);
    EXPECT_FALSE(paced.price_ticks("SHFE", "2024-12-17").empty());
    EXPECT_FALSE(paced.price_ticks("GFEX", "2024-12-17").empty());
    std::jthread cancel([&] {
        std::this_thread::sleep_for(10ms);
        stop.request_stop();
    });
    EXPECT_THROW(paced.price_ticks("SHFE", "2024-12-18"), ac::SourceError);
}

TEST_F(GuotaiSource, Html5VoidTagsEntitiesAndImplicitClosuresParseCleanly)
{
    const std::string html =
        R"(<html><body><script id="__NEXT_DATA__" type="application/json">)" R"({"props":{"pageProps":{"DEFAULT_CONTENT":{"tradingday":"20241218","tradeflag":"T",)" R"("events":[{"event_date":"20241218","content":")" R"(<table><colgroup><col width=90><col width=80></colgroup>)" R"(<thead><tr><th>交易所<th>代码<th>最小变动价位<th>特殊合约参数调整<th>调整备注</thead>)" R"(<tbody><tr><td>&nbsp;上期所&nbsp;<td>AU<td>&nbsp;0.02&nbsp;<td>无<br>常规<td>—)" R"(<tr><td>广期所<td>LC<td>20<td>无<td>—</tbody></table>)" R"("}]}}}}</script></body></html>)";
    ac::Guotai source(exchanges, budget, {}, [&](const auto&, auto) {
        return ac::HttpReply{200, html};
    });
    const auto shfe = source.price_ticks("SHFE", "2024-12-18");
    ASSERT_EQ(shfe.size(), 1);
    EXPECT_EQ(shfe[0].product, "au");
    EXPECT_EQ(shfe[0].price_tick.str(), "0.02");
    const auto gfex = source.price_ticks("GFEX", "2024-12-18");
    ASSERT_EQ(gfex.size(), 1);
    EXPECT_EQ(gfex[0].product, "lc");
    EXPECT_EQ(gfex[0].price_tick.str(), "20");
}

TEST_F(GuotaiSource, MarketPublishesFiveExchangesAndSkipsStoredDays)
{
    ac::Manager manager(
        root,
        {{"gtja", [this](auto stop) {
              return std::make_unique<ac::Guotai>(exchanges, budget, stop, fetch);
          }}});
    ac::wire::StartAcquisition request;
    request.set_id("ticks");
    request.set_table(::warehouse::TABLE_PRICE_TICKS);
    request.set_first_day("2024-12-17");
    request.set_last_day("2024-12-18");
    manager.submit(request);
    auto task = wait(manager, request.id());
    ASSERT_EQ(task.state(), ac::wire::TASK_STATE_SUCCEEDED) << task.error().message();
    EXPECT_EQ(task.requests_sent(), 2);
    EXPECT_EQ(task.items_done(), 10);
    wh::Store research(wh::source_directory(root, "research"), "research");
    wh::compose(root, research, wh::source_versions(root));
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
    EXPECT_EQ(wh::compose(root, research, wh::source_versions(root)).identity, fixed.identity);
}

TEST_F(GuotaiSource, VerifiedRulesCoverSixExchangesOfflineAndDetectPublicChanges)
{
    const auto rules = wh::load_exchange_rules(ASTERION_WAREHOUSE_RULES_PATH);
    const auto first = wh::publish_verified_price_ticks(root, rules.price_ticks);
    ASSERT_FALSE(first.empty());
    EXPECT_EQ(wh::publish_verified_price_ticks(root, rules.price_ticks), first);
    wh::Store research(wh::source_directory(root, "research"), "research");
    const auto composed = wh::compose(root, research, wh::source_versions(root));
    ASSERT_EQ(composed.partitions.size(), 6U);
    for (const auto* exchange : {"SHFE", "INE", "DCE", "CZCE", "CFFEX", "GFEX"})
        EXPECT_EQ(
            composed.partitions.at(std::string("price_ticks/") + exchange).source,
            "verified");

    // Public reports match the shipped rules across historical transitions without writing duplicates.
    std::string override_body;
    ac::Manager manager(
        root,
        {{"gtja",
            [&](auto stop) {
                return std::make_unique<ac::Guotai>(
                    exchanges,
                    budget,
                    stop,
                    [&](const ac::HttpRequest& request, auto token) {
                        if (!override_body.empty())
                            return ac::HttpReply{200, override_body};
                        return fetch(request, token);
                    });
            }}},
        {},
        rules.price_ticks);
    for (const auto* begin : {"2023-11-06", "2024-12-17"}) {
        ac::wire::StartAcquisition check;
        check.set_id(std::string("verify-") + begin);
        check.set_table(::warehouse::TABLE_PRICE_TICKS);
        check.set_first_day(begin);
        check.set_last_day(ac::day_text(ac::day_of(begin) + std::chrono::days(1)));
        manager.submit(check);
        const auto done = wait(manager, check.id());
        ASSERT_EQ(done.state(), ac::wire::TASK_STATE_SUCCEEDED) << done.error().message();
    }
    EXPECT_TRUE(wh::Store(wh::source_directory(root, "gtja"), "gtja").current_identity().empty());

    // A public change diverges from exchange_rules.json and fails verification explicitly.
    override_body = fixture("20241218");
    const auto pos = override_body.find(">20<");
    ASSERT_NE(pos, std::string::npos);
    override_body.replace(pos, 4, ">10<");
    ac::wire::StartAcquisition changed;
    changed.set_id("changed");
    changed.set_table(::warehouse::TABLE_PRICE_TICKS);
    changed.add_exchanges("GFEX");
    changed.set_first_day("2024-12-18");
    changed.set_last_day("2024-12-18");
    manager.submit(changed);
    const auto failed = wait(manager, "changed");
    EXPECT_EQ(failed.state(), ac::wire::TASK_STATE_FAILED);
    EXPECT_NE(failed.error().message().find("GFEX/lc"), std::string::npos);
}

TEST_F(GuotaiSource, MonthlyPriceTicksUseProductRulesUnlessAContractRuleApplies)
{
    const wh::PriceTick
        product{"CZCE", "ap", {}, "2026-01-01", "2027-01-01", Decimal::parse("1"), ""};
    auto observed = product;
    observed.contract = std::string("CZCE/ap/2026-12");
    auto specific = observed;
    specific.price_tick = Decimal::parse("2");
    std::vector verified{product};
    std::vector observations{observed};
    EXPECT_NO_THROW(wh::verify_price_ticks(verified, observations, "2026-11-26"));
    verified.push_back(specific);
    EXPECT_THROW(
        wh::verify_price_ticks(verified, observations, "2026-11-26"),
        std::invalid_argument);
    observations[0].price_tick = Decimal::parse("2");
    EXPECT_NO_THROW(wh::verify_price_ticks(verified, observations, "2026-11-26"));
    verified[1].effective_from = "2026-12-01";
    observations[0].price_tick = Decimal::parse("1");
    EXPECT_NO_THROW(wh::verify_price_ticks(verified, observations, "2026-11-26"));
}

TEST_F(GuotaiSource, PublicReportsPublishHistoricalChangesThroughMarket)
{
    if (!std::getenv("ASTERION_GUOTAI_ACCEPTANCE"))
        GTEST_SKIP() << "set ASTERION_GUOTAI_ACCEPTANCE=1 to fetch public historical reports";
    ac::Manager manager(root, {{"gtja", [this](auto stop) {
                                    return std::make_unique<ac::Guotai>(exchanges, budget, stop);
                                }}});
    for (const auto* first : {"2023-11-06", "2024-12-17"}) {
        ac::wire::StartAcquisition request;
        request.set_id(first);
        request.set_table(::warehouse::TABLE_PRICE_TICKS);
        request.set_first_day(first);
        request.set_last_day(ac::day_text(ac::day_of(first) + std::chrono::days(1)));
        manager.submit(request);
        const auto task = wait(manager, request.id());
        ASSERT_EQ(task.state(), ac::wire::TASK_STATE_SUCCEEDED) << task.error().message();
        EXPECT_EQ(task.requests_sent(), 2);
        EXPECT_EQ(task.items_empty(), 0);
    }
    wh::Store research(wh::source_directory(root, "research"), "research");
    const auto fixed = wh::compose(root, research, wh::source_versions(root));
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
