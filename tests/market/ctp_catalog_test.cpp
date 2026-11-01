#include "support/market_host.hpp"
#include "ctp_catalog.hpp"
#include "ctp_support.hpp"
#include <asterion/base/error.hpp>
#include <asterion/system/process/child.hpp>
#include <asterion/system/ipc/local_channel.hpp>
#include <asterion/protocol/market.hpp>
#include <gtest/gtest.h>
#include <thread>
using namespace asterion;
using namespace std::chrono_literals;

namespace {

class CtpCatalog : public ::testing::Test
{
protected:
    std::filesystem::path flow =
        std::filesystem::temp_directory_path() / ("asterion-catalog-" + unique_process_id());
    ctp::CatalogConfiguration config{"tcp://127.0.0.1:1", "9999", "catalog", "secret", "", ""};

    void TearDown() override
    {
        std::error_code error;
        std::filesystem::remove_all(flow, error);
    }

    ctp::Catalog read(std::stop_token stop = {}, std::chrono::milliseconds timeout = 2s)
    {
        ctp::CatalogQuery query(ASTERION_TEST_CTP_TRADER, flow, config, timeout);
        for (;;) {
            if (auto result = query.poll(stop))
                return std::move(*result);
            std::this_thread::sleep_for(1ms);
        }
    }
};

} // namespace

TEST_F(CtpCatalog, ReturnsOnlyActiveFuturesWithoutSettlementOrOrderActions)
{
    ctp::SharedLibrary control(ASTERION_TEST_CTP_TRADER, "asterion_fake_catalog_side_effects");
    const auto count = control.symbol<int (*)()>();
    const int before = count();
    const auto result = read();
    EXPECT_EQ(result.trading_day, "20260928");
    ASSERT_EQ(result.contracts.size(), 1U);
    const auto& contract = result.contracts.front();
    EXPECT_EQ(contract.instrument.venue, "SHFE");
    EXPECT_EQ(contract.instrument.symbol, "rb2610");
    EXPECT_EQ(contract.product, "rb");
    EXPECT_EQ(contract.name, "螺纹钢");
    EXPECT_EQ(contract.expiry, "20261015");
    EXPECT_EQ(contract.multiplier, 10);
    EXPECT_EQ(contract.price_tick.str(), "0.5");
    EXPECT_EQ(count(), before);
}

TEST_F(CtpCatalog, AuthAndLoginErrorsRejectRatherThanReturnAnEmptyCatalog)
{
    config.app_id = "app";
    // The refusal names its step: authentication is the AppID, login the password.
    const auto refusal = [&] {
        try {
            (void)read();
        } catch (const Error& error) {
            return std::string(error.what());
        }
        return std::string("accepted");
    };
    config.auth_code = "bad-auth";
    EXPECT_NE(refusal().find("CTP catalog authentication rejected"), std::string::npos);
    config.auth_code = "auth";
    config.password = "bad";
    EXPECT_NE(refusal().find("CTP catalog login rejected"), std::string::npos);
    config.password = "secret";
    EXPECT_EQ(read().contracts.size(), 1U);
}

TEST_F(CtpCatalog, RejectsPartialInvalidAndDuplicateResults)
{
    for (const auto* mode : {"catalog-error", "catalog-invalid", "catalog-dup"}) {
        SCOPED_TRACE(mode);
        config.user = mode;
        EXPECT_THROW(read(), Error);
    }
    config.user = "catalog-empty";
    EXPECT_TRUE(read().contracts.empty());
}

TEST_F(CtpCatalog, TheFirstInstrumentResponseMayTakeLongerThanTheStepTimeout)
{
    config.user = "catalog-slow";
    EXPECT_EQ(read({}, 1s).contracts.size(), 1U);
}

TEST_F(CtpCatalog, TimeoutAndCancellationDoNotReturnPartialResults)
{
    config.user = "catalog-partial";
    try {
        // Long enough to connect and log in on a loaded machine; the query itself
        // never completes, so the deadline always expires inside it.
        read({}, 1s);
        FAIL() << "expected query timeout";
    } catch (const Error& error) {
        EXPECT_EQ(error.code(), ErrorCode::unavailable);
        EXPECT_STREQ(error.what(), "CTP catalog instrument query timed out after 3 responses");
    }
    config.user = "catalog-stall";
    std::stop_source stop;
    std::jthread cancel([&] {
        std::this_thread::sleep_for(120ms);
        stop.request_stop();
    });
    try {
        read(stop.get_token());
        FAIL() << "expected cancellation";
    } catch (const Error& error) {
        EXPECT_EQ(error.code(), ErrorCode::cancelled);
    }
}

TEST_F(CtpCatalog, FilteredResponsesKeepQueryAliveUntilFinalResponse)
{
    config.user = "catalog-stream";
    const auto result = read({}, 300ms);
    ASSERT_EQ(result.contracts.size(), 1U);
    EXPECT_EQ(result.contracts.front().instrument.symbol, "rb2610");
}

TEST(CtpCatalogService, EmptyWatchlistLoadsMarketAndQueriesRemainResponsive)
{
    const auto directory =
        std::filesystem::path("/tmp") / ("ast-cat-" + unique_process_id().substr(0, 12));
    std::filesystem::create_directory(directory);
    std::filesystem::permissions(directory, std::filesystem::perms::owner_all);

    struct Cleanup
    {
        std::filesystem::path path;

        ~Cleanup()
        {
            std::error_code e;
            std::filesystem::remove_all(path, e);
        }
    } cleanup{directory};

    testing_support::MarketHost host(directory, "catalog-slow");
    host.start();
    namespace wire = asterion::market::proto;
    auto call = [&](wire::Request request) {
        request.set_version(1);
        request.set_service_id("market-data");
        request.set_correlation_id(unique_process_id());
        auto channel = host.open();
        channel.send(request.SerializeAsString(), 1s);
        wire::Response response;
        if (!response.ParseFromString(channel.receive(1s)))
            throw std::runtime_error("invalid market response");
        if (response.has_error())
            throw std::runtime_error(response.error().message());
        return response;
    };
    auto snapshot = [&] {
        wire::Request request;
        request.mutable_snapshot();
        return call(request).snapshot();
    };
    auto wait = [&](auto condition) {
        const auto deadline = std::chrono::steady_clock::now() + 8s;
        for (;;) {
            try {
                auto state = snapshot();
                if (condition(state))
                    return state;
            } catch (const std::exception&) {
                if (host.process->exited())
                    throw;
            }
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("market test deadline");
            std::this_thread::sleep_for(20ms);
        }
    };
    wait([](const auto& state) {
        return state.catalog().phase() == "loading";
    });
    const auto begin = std::chrono::steady_clock::now();
    EXPECT_EQ(snapshot().catalog().phase(), "loading");
    EXPECT_LT(std::chrono::steady_clock::now() - begin, 1s);
    auto ready = wait([](const auto& state) {
        return state.catalog().phase() == "ready" && state.subscriptions_size() == 1
            && state.subscriptions(0).has_quote();
    });
    EXPECT_EQ(ready.watchlist_size(), 0);
    EXPECT_EQ(ready.subscriptions(0).instrument().symbol(), "rb2610");
    EXPECT_EQ(ready.subscriptions(0).quote().last(), "3510");
    const auto json = protocol::decode_market(ready);
    EXPECT_EQ(json.at("catalog").at("contracts").size(), 1U);
    EXPECT_TRUE(json.at("watchlist").empty());
    wire::Request watch;
    auto* selected = watch.mutable_watch()->add_instruments();
    selected->set_venue("SHFE");
    selected->set_symbol("rb2610");
    EXPECT_EQ(call(watch).snapshot().watchlist_size(), 1);
    // Closing this stream does not change the shared catalog or collection.
    EXPECT_EQ(snapshot().watchlist_size(), 0);
    EXPECT_EQ(snapshot().subscriptions_size(), 1);
}
