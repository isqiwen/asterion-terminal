#include "live.hpp"
#include "support/timing.hpp"
#include <asterion/system/process/child.hpp>
#include <gtest/gtest.h>
#include <fstream>
#include <thread>

using namespace asterion;
using namespace std::chrono_literals;
namespace wire = ::market;
using Live = asterion::market::Live;

class MarketRecovery : public ::testing::Test
{
protected:
    void SetUp() override
    {
        settings.directory = std::filesystem::temp_directory_path()
            / ("asterion-market-recovery-" + unique_process_id());
        settings.ctp_library = ASTERION_TEST_CTP;
        settings.ctp_catalog_library = ASTERION_TEST_CTP_TRADER;
        settings.market = {"tcp://127.0.0.1:1", "9999", "steady", "secret"};
        settings.catalog = {"tcp://127.0.0.1:1", "9999", "catalog", "secret", "app", "auth"};
        std::filesystem::create_directories(settings.directory / "ctp-flow");
        std::filesystem::create_directories(settings.directory / "ctp-catalog-flow");
    }

    void TearDown() override
    {
        live.reset();
        std::filesystem::remove_all(settings.directory);
    }

    wire::Status advance(Live::Clock::time_point now = Live::Clock::now())
    {
        live->maintain(now);
        for (const auto& event : live->poll().events)
            live->apply(event);
        wire::Status status;
        live->status(status);
        return status;
    }

    bool wait(auto ready)
    {
        const auto until = Live::Clock::now() + testing_support::bound(10s);
        while (Live::Clock::now() < until) {
            if (ready(advance()))
                return true;
            std::this_thread::sleep_for(1ms);
        }
        return false;
    }

    asterion::market::Settings settings;
    std::unique_ptr<Live> live;
};

TEST_F(MarketRecovery, AStalledConnectionReportsItsTimeoutAndReconnects)
{
    const auto hold = settings.directory / "ctp-flow/hold-front";
    std::ofstream(hold).put('1');
    live = std::make_unique<Live>(settings);
    const auto started = Live::Clock::now();
    EXPECT_EQ(advance().feed().state(), wire::FEED_STATE_CONNECTING);
    const auto failed = advance(started + 31s);
    EXPECT_EQ(failed.feed().state(), wire::FEED_STATE_FAILED);
    EXPECT_EQ(failed.feed().error(), "CTP market front connection timed out");
    std::filesystem::remove(hold);
    advance(started + 37s);
    ASSERT_TRUE(wait([](const auto& status) {
        return status.feed().state() == wire::FEED_STATE_CONNECTED;
    }));
    EXPECT_TRUE(advance().feed().error().empty());
    EXPECT_EQ(advance(started + 2min).feed().state(), wire::FEED_STATE_CONNECTED);
}

TEST_F(MarketRecovery, AStalledLoginReportsItsOwnStageAndReconnects)
{
    const auto hold = settings.directory / "ctp-flow/hold-login";
    std::ofstream(hold).put('1');
    live = std::make_unique<Live>(settings);
    ASSERT_TRUE(wait([](const auto& status) {
        return status.feed().state() == wire::FEED_STATE_LOGGING_IN;
    }));
    const auto started = Live::Clock::now();
    const auto failed = advance(started + 31s);
    EXPECT_EQ(failed.feed().state(), wire::FEED_STATE_FAILED);
    EXPECT_EQ(failed.feed().error(), "CTP market login timed out");
    std::filesystem::remove(hold);
    advance(started + 37s);
    ASSERT_TRUE(wait([](const auto& status) {
        return status.feed().state() == wire::FEED_STATE_CONNECTED;
    }));
}

TEST_F(MarketRecovery, AnInactiveCatalogFrontIsRetriedWithoutDisconnectingTheFeed)
{
    const auto hold = settings.directory / "ctp-catalog-flow/front-inactive";
    std::ofstream(hold).put('1');
    live = std::make_unique<Live>(settings);
    ASSERT_TRUE(wait([](const auto& status) {
        return status.catalog().state() == wire::CATALOG_STATE_FAILED;
    }));
    const auto failed = advance();
    EXPECT_EQ(failed.feed().state(), wire::FEED_STATE_CONNECTED);
    EXPECT_NE(failed.catalog().error().find("front inactive"), std::string::npos);
    std::filesystem::remove(hold);
    const auto retry = advance(Live::Clock::now() + 11s);
    EXPECT_EQ(retry.catalog().state(), wire::CATALOG_STATE_LOADING);
    EXPECT_EQ(retry.feed().state(), wire::FEED_STATE_CONNECTED);
    ASSERT_TRUE(wait([](const auto& status) {
        return status.catalog().state() == wire::CATALOG_STATE_READY;
    }));
    EXPECT_EQ(advance().subscribed(), 1U);
    EXPECT_TRUE(advance().catalog().error().empty());
}
