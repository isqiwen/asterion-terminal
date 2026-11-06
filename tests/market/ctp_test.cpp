#include "support/market_host.hpp"
#include "ctp_feed.hpp"
#include "support/timing.hpp"
#include <asterion/system/durable_file.hpp>
#include <asterion/system/ipc/local_channel.hpp>
#include <asterion/system/process/child.hpp>
#include <asterion/protocol/market.hpp>
#include <gtest/gtest.h>
#include <csignal>
#include <unistd.h>
#include <limits>
#include <thread>

TEST(Ctp, RejectsAliasesAndDuplicates)
{
    EXPECT_NO_THROW(asterion::ctp::validate_instruments({{"SHFE", "rb2610"}, {"CZCE", "SR701"}}));
    EXPECT_THROW(asterion::ctp::validate_instruments({{"SHFE", "rbMAIN"}}), std::invalid_argument);
    EXPECT_THROW(
        asterion::ctp::validate_instruments({{"SHFE", "rb2610"}, {"DCE", "rb2610"}}),
        std::invalid_argument);
}

TEST(Ctp, MissingNumbersAndSourceClockRemainExplicit)
{
    EXPECT_FALSE(asterion::ctp::price(std::numeric_limits<double>::max()));
    EXPECT_FALSE(asterion::ctp::price(std::numeric_limits<double>::quiet_NaN()));
    ASSERT_TRUE(asterion::ctp::price(3510.2));
    EXPECT_EQ(asterion::ctp::price(3510.2)->str(), "3510.2");
    EXPECT_EQ(asterion::ctp::source_time("20260926", "09:00:00", 0), 1790384400000);
    EXPECT_EQ(asterion::ctp::source_time("20260230", "09:00:00", 0), 0);
    EXPECT_EQ(asterion::ctp::source_time("", "09:00:00", 0), 0);
    EXPECT_EQ(asterion::ctp::source_time("20260926", "24:00:00", 0), 0);
}

TEST(Ctp, VendorPricesPreserveDecimalPrecisionAndBounds)
{
    for (const auto& [input, expected] : std::vector<std::pair<double, std::string>>{
             {-0.0, "0"},
             {-12.125, "-12.125"},
             {0.00000001, "0.00000001"},
             {1.234567894, "1.23456789"},
             {1.234567896, "1.2345679"},
             {1e10, "10000000000"}
         }) {
        const auto converted = asterion::ctp::price(input);
        ASSERT_TRUE(converted) << input;
        EXPECT_EQ(converted->str(), expected);
    }
    EXPECT_FALSE(asterion::ctp::price(1e10 + 1));
    EXPECT_FALSE(asterion::ctp::price(-1e10 - 1));
    EXPECT_FALSE(asterion::ctp::price(std::numeric_limits<double>::infinity()));
}

TEST(Ctp, AveragePriceIsNormalizedPerUnit)
{
    using asterion::ctp::average_price;
    EXPECT_EQ(average_price(161550, "SHFE", 10)->str(), asterion::Decimal::parse("16155").str());
    EXPECT_EQ(
        average_price(10234.5, "CZCE", std::nullopt)->str(),
        asterion::Decimal::parse("10234.5").str());
    EXPECT_FALSE(average_price(161550, "SHFE", std::nullopt));
    EXPECT_FALSE(average_price(0, "DCE", 10));
}

TEST(Ctp, ProtocolPreservesMissingPricesAndSourceDates)
{
    asterion::LiveMarketSnapshot state;
    asterion::MarketQuote q;
    q.instrument = {"SHFE", "rb2610"};
    q.action_day = "20260925";
    q.trading_day = "20260928";
    q.source_ms = 0;
    state.subscriptions.push_back({q.instrument, asterion::SubscriptionState::subscribed, 0, q});
    const auto result = asterion::protocol::encode_board(state).rows(0).quote();
    EXPECT_FALSE(result.has_last());
    EXPECT_EQ(result.trading_day(), "2026-09-28");
    EXPECT_FALSE(result.has_exchange_ms());
}

namespace {

struct CtpDirectory
{
    asterion::ThreadPool sdk{1, 2};
    std::filesystem::path path = std::filesystem::temp_directory_path()
        / ("asterion-ctp-events-" + asterion::unique_process_id());

    CtpDirectory()
    {
        std::filesystem::create_directory(path);
    }

    ~CtpDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

// Polls until `rejected` quotes arrived out of order; what each poll returned
// is appended to `seen`.
bool wait_quotes(
    asterion::ctp::Feed& feed,
    std::uint64_t rejected,
    std::vector<asterion::ctp::FeedEvent>* seen = nullptr)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        auto update = feed.poll();
        if (seen)
            seen->insert(seen->end(), update.events.begin(), update.events.end());
        if (feed.snapshot().out_of_order >= rejected)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

void connect_fixture(asterion::ctp::Feed& feed)
{
    feed.connect({"tcp://localhost:12345", "test", "test", "test-only"}, {{"SHFE", "rb2610"}});
}

} // namespace

TEST(CtpEvents, OwnerAppliesCallbacksAndReconnectWaitsOnlyOnTheSdkThread)
{
    using namespace asterion;
    using namespace std::chrono_literals;
    CtpDirectory directory;
    ctp::Feed feed(directory.sdk, ASTERION_TEST_CTP, directory.path);
    const auto release = directory.path / "hold-release";
    const auto init = directory.path / "hold-init";
    const auto publication = directory.path / "hold-publication";

    struct Unblock
    {
        std::filesystem::path release, init, publication;

        ~Unblock()
        {
            std::filesystem::remove(release);
            std::filesystem::remove(init);
            std::filesystem::remove(publication);
        }
    } unblock{release, init, publication};

    feed.connect({"tcp://localhost:12345", "test", "steady", "test-only"}, {{"SHFE", "rb2610"}});
    ASSERT_TRUE(wait_quotes(feed, 1));
    const auto wait_gate = [&](const auto& path, bool advance = true) {
        const auto until = std::chrono::steady_clock::now() + testing_support::bound(5s);
        while (!std::filesystem::exists(path.string() + ".entered")) {
            if (advance)
                feed.poll();
            if (std::chrono::steady_clock::now() >= until)
                return false;
            std::this_thread::sleep_for(1ms);
        }
        return true;
    };
    replace_file_durably(release, "hold");
    feed.disconnect();
    ASSERT_TRUE(wait_gate(release));
    replace_file_durably(init, "hold");
    const auto began = std::chrono::steady_clock::now();
    feed.connect(
        {"tcp://localhost:12345", "test", "publication", "test-only"},
        {{"SHFE", "rb2610"}});
    EXPECT_LT(std::chrono::steady_clock::now() - began, testing_support::bound(100ms));
    EXPECT_EQ(feed.phase(), asterion::MarketPhase::connecting);
    EXPECT_FALSE(feed.snapshot().subscriptions.front().quote);
    std::filesystem::remove(release);
    ASSERT_TRUE(wait_gate(init));
    // Release delivered the old generation's delayed login. Its SPI stays alive
    // until release finishes, but the new state must remain unconnected.
    feed.poll();
    EXPECT_EQ(feed.phase(), asterion::MarketPhase::connecting);
    std::filesystem::remove(init);
    ASSERT_TRUE(wait_quotes(feed, 2));
    const auto before = feed.snapshot();
    replace_file_durably(publication, "hold");
    ASSERT_TRUE(wait_gate(publication, false));
    EXPECT_EQ(feed.snapshot().sequence, before.sequence);
    const auto delivered = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
                               .count();
    std::this_thread::sleep_for(20ms);
    feed.poll();
    const auto after = feed.snapshot();
    EXPECT_GT(after.sequence, before.sequence);
    ASSERT_TRUE(after.subscriptions.front().quote);
    EXPECT_LE(after.subscriptions.front().quote->received_ms, delivered);
}

TEST(CtpEvents, CallbackOverflowFailsTheStreamInsteadOfPublishingASilentGap)
{
    using namespace asterion;
    using namespace std::chrono_literals;
    CtpDirectory directory;
    ctp::Feed feed(directory.sdk, ASTERION_TEST_CTP, directory.path);
    feed.connect({"tcp://localhost:12345", "test", "flood", "test-only"}, {{"SHFE", "rb2610"}});
    const auto deadline = std::chrono::steady_clock::now() + testing_support::bound(5s);
    while (feed.phase() != asterion::MarketPhase::connected) {
        ASSERT_LT(std::chrono::steady_clock::now(), deadline);
        feed.poll();
        std::this_thread::sleep_for(1ms);
    }
    // Let a complete provider burst reach the bounded ingress before consuming it.
    while (!std::filesystem::exists(directory.path / "burst.done")) {
        ASSERT_LT(std::chrono::steady_clock::now(), deadline);
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_TRUE(feed.poll().failed);
    EXPECT_EQ(feed.phase(), asterion::MarketPhase::error);
    EXPECT_THROW(feed.connect({"tcp://localhost:12345", "test", "steady", "test-only"}, {}), Error);
    feed.disconnect();
    EXPECT_TRUE(feed.poll().failed);
    EXPECT_EQ(feed.phase(), asterion::MarketPhase::disconnected);
}

TEST(CtpEvents, PreservesPreCoalescingQuotesAndReconnectBoundaries)
{
    CtpDirectory directory;
    asterion::ctp::Feed feed(directory.sdk, ASTERION_TEST_CTP, directory.path);
    EXPECT_TRUE(feed.poll().events.empty());
    connect_fixture(feed);
    std::vector<asterion::ctp::FeedEvent> seen;
    ASSERT_TRUE(wait_quotes(feed, 2, &seen));
    feed.disconnect();
    const auto last = feed.poll();
    ASSERT_FALSE(last.failed);
    seen.insert(seen.end(), last.events.begin(), last.events.end());
    std::vector<asterion::MarketQuoteObservation> quotes;
    bool reconnecting = false, reconnected = false;
    for (const auto& event : seen) {
        if (const auto* q = std::get_if<asterion::MarketQuoteObservation>(&event)) {
            quotes.push_back(*q);
        } else if (const auto* status = std::get_if<asterion::ctp::FeedStatus>(&event)) {
            if (status->phase == asterion::MarketPhase::reconnecting) {
                reconnecting = true;
                EXPECT_EQ(status->error_code, 4097);
            }
            if (reconnecting && status->phase == asterion::MarketPhase::connected)
                reconnected = true;
        } else {
            const auto& subscription = std::get<asterion::MarketSubscription>(event);
            EXPECT_FALSE(subscription.quote);
            EXPECT_EQ(subscription.state, asterion::SubscriptionState::subscribed);
        }
    }
    ASSERT_EQ(quotes.size(), 4U);
    EXPECT_EQ(quotes[0].quote.last->str(), "3510");
    EXPECT_EQ(quotes[0].quote.bid_levels[0].price->str(), "3508.25");
    EXPECT_EQ(quotes[0].quote.bid_levels[0].quantity, 4);
    EXPECT_FALSE(quotes[0].quote.bid_levels[1].price);
    EXPECT_FALSE(quotes[0].quote.bid_levels[1].quantity);
    EXPECT_EQ(quotes[0].quote.ask_levels[1].quantity, 0);
    EXPECT_EQ(quotes[0].quote.bid_levels[2].price->str(), "3506");
    EXPECT_FALSE(quotes[0].quote.bid_levels[2].quantity);
    EXPECT_FALSE(quotes[0].quote.ask_levels[2].price);
    EXPECT_EQ(quotes[0].quote.ask_levels[3].price->str(), "3515");
    const auto encoded_depth = asterion::protocol::encode_board(feed.snapshot());
    EXPECT_EQ(encoded_depth.rows(0).quote().bids(1).price().text(), "3508.25");
    EXPECT_EQ(quotes[1].quote.last->str(), "1");
    EXPECT_FALSE(quotes[0].out_of_order);
    EXPECT_TRUE(quotes[1].out_of_order);
    EXPECT_FALSE(quotes[1].quote.previous_settlement);
    EXPECT_EQ(quotes[1].quote.volume, 5);
    EXPECT_TRUE(reconnected);
    EXPECT_EQ(feed.snapshot().subscriptions[0].quote->last->str(), "3510");
}

TEST(CtpEvents, SubscriptionReorderingAndVenueReplacementKeepQuotesBoundToExactContracts)
{
    CtpDirectory directory;
    asterion::ctp::Feed feed(directory.sdk, ASTERION_TEST_CTP, directory.path);
    feed.connect(
        {"tcp://localhost:12345", "test", "test", "test-only"},
        {{"SHFE", "rb2610"}, {"SHFE", "rb2710"}, {"SHFE", "bad2610"}});
    ASSERT_TRUE(wait_quotes(feed, 2));
    feed.subscribe({{"SHFE", "rb2710"}, {"DCE", "rb2610"}, {"SHFE", "bad2610"}});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    asterion::LiveMarketSnapshot current;
    do {
        feed.poll();
        current = feed.snapshot();
        if (current.subscriptions.at(2).state == asterion::SubscriptionState::error)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);
    ASSERT_EQ(current.subscriptions.size(), 3U);
    ASSERT_TRUE(current.subscriptions[0].quote);
    EXPECT_EQ(current.subscriptions[0].quote->instrument.symbol, "rb2710");
    EXPECT_EQ(current.subscriptions[0].quote->instrument.venue, "SHFE");
    EXPECT_EQ(current.subscriptions[1].state, asterion::SubscriptionState::subscribed);
    EXPECT_EQ(current.subscriptions[1].instrument.venue, "DCE");
    EXPECT_FALSE(current.subscriptions[1].quote) << "SHFE observations cannot become DCE quotes";
    EXPECT_EQ(current.subscriptions[2].state, asterion::SubscriptionState::error);
    EXPECT_EQ(current.subscriptions[2].error_code, 31);
    feed.subscribe({});
    EXPECT_TRUE(feed.snapshot().subscriptions.empty());
    feed.subscribe({{"SHFE", "rb2610"}});
    ASSERT_TRUE(wait_quotes(feed, current.out_of_order + 1));
    const auto replaced = feed.snapshot();
    ASSERT_EQ(replaced.subscriptions.size(), 1U);
    ASSERT_TRUE(replaced.subscriptions[0].quote);
    EXPECT_EQ(replaced.subscriptions[0].quote->instrument.symbol, "rb2610");
}

TEST(CtpEvents, ARefusedSubscriptionIsCountedAndRetriedAlone)
{
    CtpDirectory directory;
    asterion::ctp::Feed feed(directory.sdk, ASTERION_TEST_CTP, directory.path);
    feed.connect({"tcp://localhost:12345", "test", "test", "test-only"}, {{"SHFE", "fla2610"}});
    const auto until = [&](auto predicate) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!predicate() && std::chrono::steady_clock::now() < deadline) {
            feed.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return predicate();
    };
    ASSERT_TRUE(until([&] {
        return feed.failed_subscriptions() == 1;
    }));
    feed.retry_failed_subscriptions();
    EXPECT_TRUE(until([&] {
        return feed.snapshot().subscriptions.at(0).state == asterion::SubscriptionState::subscribed;
    }));
    EXPECT_EQ(feed.failed_subscriptions(), 0U);
    feed.disconnect();
}

TEST(CtpEvents, FullCatalogAcknowledgementsRetainOnlyTheirChangedContract)
{
    using namespace asterion;
    CtpDirectory directory;
    ctp::Feed feed(directory.sdk, ASTERION_TEST_CTP, directory.path);
    constexpr std::size_t count = 20000;
    std::vector<InstrumentId> contracts;
    for (std::size_t i = 0; i < count; ++i)
        contracts.push_back(
            {"SHFE",
                std::string(1, static_cast<char>('a' + i / 9000))
                    + std::to_string(1000 + i % 9000)});
    const auto began = std::chrono::steady_clock::now();
    feed.connect({"tcp://localhost:12345", "test", "steady", "test-only"}, contracts);
    const auto deadline = began + testing_support::bound(std::chrono::seconds(30));
    std::size_t acknowledgements = 0, quotes = 0;
    const auto count_events = [&] {
        const auto update = feed.poll();
        EXPECT_FALSE(update.failed);
        for (const auto& event : update.events) {
            if (const auto* subscription = std::get_if<MarketSubscription>(&event)) {
                ++acknowledgements;
                EXPECT_FALSE(subscription->quote);
            } else if (std::holds_alternative<MarketQuoteObservation>(event)) {
                ++quotes;
            }
        }
        return update.events.size();
    };
    for (;;) {
        count_events();
        const auto state = feed.snapshot();
        if (state.out_of_order == count) {
            ASSERT_EQ(state.subscriptions.size(), count);
            for (const auto& row : state.subscriptions) {
                ASSERT_EQ(row.state, asterion::SubscriptionState::subscribed);
                ASSERT_TRUE(row.quote);
            }
            break;
        }
        ASSERT_LT(std::chrono::steady_clock::now(), deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - began)
                             .count();
    while (count_events())
        ;
    EXPECT_EQ(acknowledgements, count);
    EXPECT_EQ(quotes, count * 2);
    RecordProperty("contracts", static_cast<int>(count));
    RecordProperty("subscription_and_quote_ms", std::to_string(elapsed));
    const auto full = feed.snapshot();
    EXPECT_FALSE(full.subscriptions_delta);
    const auto quiet = feed.snapshot(full.sequence);
    EXPECT_TRUE(quiet.subscriptions_delta);
    EXPECT_TRUE(quiet.subscriptions.empty());
    EXPECT_EQ(quiet.sequence, full.sequence);
    const std::vector<InstrumentId> late_intraday{
        contracts.back(),
        contracts.front(),
        contracts.front(),
        {"DCE", contracts.front().symbol}
    };
    const auto forced = feed.snapshot(full.sequence, late_intraday);
    ASSERT_EQ(forced.subscriptions.size(), 2U);
    EXPECT_EQ(forced.subscriptions[0].instrument, contracts.front());
    EXPECT_EQ(forced.subscriptions[1].instrument, contracts.back());
    EXPECT_THROW(feed.snapshot(full.sequence + 1), std::invalid_argument);
    RecordProperty(
        "full_quote_frame_bytes",
        std::to_string(protocol::encode_board(full).ByteSizeLong()));
    RecordProperty(
        "unchanged_quote_frame_bytes",
        std::to_string(protocol::encode_board(quiet).ByteSizeLong()));
    feed.subscribe({contracts.front(), contracts.back()});
    const auto reset = feed.snapshot(full.sequence);
    EXPECT_FALSE(reset.subscriptions_delta);
    EXPECT_EQ(reset.subscriptions.size(), 2U);
}

TEST(CtpPublication, ContinuousQuotesChangeOnlyTheirRowAndMatchFullSnapshots)
{
    using namespace asterion;
    using namespace std::chrono_literals;
    CtpDirectory directory;
    ctp::Feed feed(directory.sdk, ASTERION_TEST_CTP, directory.path);
    std::vector<InstrumentId> contracts;
    for (int i = 1000; i < 1200; ++i)
        contracts.push_back({"SHFE", "rb" + std::to_string(i)});
    feed.connect({"tcp://localhost:12345", "test", "publication", "test-only"}, contracts);
    auto deadline = std::chrono::steady_clock::now() + testing_support::bound(5s);
    LiveMarketSnapshot full;
    for (;;) {
        feed.poll();
        full = feed.snapshot();
        if (full.out_of_order == contracts.size())
            break;
        ASSERT_LT(std::chrono::steady_clock::now(), deadline);
        std::this_thread::sleep_for(10ms);
    }
    // The page's table: a full frame, then each changed row replaced in place.
    auto merged = protocol::encode_board(full);
    const auto same_rows = [](const auto& a, const auto& b) {
        if (a.rows_size() != b.rows_size())
            return false;
        for (int i = 0; i < a.rows_size(); ++i)
            if (a.rows(i).SerializeAsString() != b.rows(i).SerializeAsString())
                return false;
        return true;
    };
    auto cursor = full.sequence;
    std::size_t updates = 0, bytes = 0;
    const auto began = std::chrono::steady_clock::now();
    deadline = began + testing_support::bound(5s);
    while (updates < 20) {
        feed.poll();
        const auto delta = feed.snapshot(cursor);
        ASSERT_TRUE(delta.subscriptions_delta);
        if (delta.sequence != cursor) {
            ASSERT_EQ(delta.subscriptions.size(), 1U);
            EXPECT_EQ(delta.subscriptions.front().instrument, contracts.front());
            const auto encoded = protocol::encode_board(delta);
            bytes += encoded.ByteSizeLong();
            *merged.mutable_rows(0) = encoded.rows(0);
            ++updates;
        }
        cursor = delta.sequence;
        const auto verify = feed.snapshot();
        EXPECT_EQ(verify.sequence, cursor);
        EXPECT_TRUE(same_rows(merged, protocol::encode_board(verify)));
        ASSERT_LT(std::chrono::steady_clock::now(), deadline);
        std::this_thread::sleep_for(10ms);
    }
    RecordProperty("changed_frames", std::to_string(updates));
    RecordProperty("changed_frame_bytes", std::to_string(bytes));
    RecordProperty(
        "publication_ms",
        std::to_string(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - began)
                .count()));
}

TEST(CtpPublication, CatalogIsSentOnlyInTheFirstFrameOfAStream)
{
    using namespace asterion;
    using namespace std::chrono_literals;
    namespace wire = ::market;
    CtpDirectory directory;
    testing_support::MarketHost host(directory.path, "catalog-large");
    host.start();
    constexpr int count = 20000;
    const auto deadline = std::chrono::steady_clock::now() + testing_support::bound(40s);
    for (;;) {
        auto channel = host.open();
        wire::Request request;
        request.set_id(1);
        request.mutable_get_status();
        channel.send(request.SerializeAsString(), testing_support::bound(10s));
        wire::Response response;
        ASSERT_TRUE(response.ParseFromString(channel.receive(testing_support::bound(10s))));
        if (response.get_status().catalog().state() == wire::CATALOG_STATE_READY)
            break;
        ASSERT_LT(std::chrono::steady_clock::now(), deadline);
        std::this_thread::sleep_for(20ms);
    }
    wire::Request watch;
    watch.set_id(2);
    // This test measures catalog publication, independently of initial quote churn.
    auto* filter = watch.mutable_stream_quotes()->add_instruments();
    filter->set_exchange("SHFE");
    filter->set_symbol("rb9999");
    const auto receive = [&](testing_support::MarketChannel& channel) {
        wire::Response response;
        if (!response.ParseFromString(channel.receive(testing_support::bound(10s)))
            || !response.has_stream_quotes())
            throw std::runtime_error("expected a quote frame");
        return response.stream_quotes();
    };
    auto connection = host.open();
    connection.send(watch.SerializeAsString(), testing_support::bound(10s));
    const auto first_frame = receive(connection);
    ASSERT_TRUE(first_frame.has_full());
    const auto& first = first_frame.full();
    EXPECT_EQ(first.catalog().contracts_size(), count);
    EXPECT_EQ(first.catalog().status().state(), wire::CATALOG_STATE_READY);
    const auto second_frame = receive(connection);
    ASSERT_TRUE(second_frame.has_changed());
    const auto& second = second_frame.changed();
    EXPECT_FALSE(second.has_catalog());
    EXPECT_LT(second.ByteSizeLong(), first.ByteSizeLong() / 100);
    RecordProperty("catalog_contracts", count);
    RecordProperty("initial_frame_bytes", std::to_string(first.ByteSizeLong()));
    RecordProperty("unchanged_frame_bytes", std::to_string(second.ByteSizeLong()));
    auto reconnected = host.open();
    reconnected.send(watch.SerializeAsString(), testing_support::bound(10s));
    const auto initial_frame = receive(reconnected);
    ASSERT_TRUE(initial_frame.has_full());
    EXPECT_EQ(initial_frame.full().catalog().contracts_size(), count);
}

TEST(Ctp, DepthPreservesMissingLevelsZeroSizeAndExactPrices)
{
    using namespace asterion;
    EXPECT_FALSE(ctp::depth_level(0, 0).price);
    EXPECT_FALSE(ctp::depth_level(std::numeric_limits<double>::max(), 4).quantity);
    EXPECT_FALSE(ctp::depth_level(std::numeric_limits<double>::quiet_NaN(), 4).price);
    EXPECT_EQ(ctp::depth_level(100.00000001, 0).price->str(), "100.00000001");
    EXPECT_EQ(ctp::depth_level(100.00000001, 0).quantity, 0);
    EXPECT_EQ(ctp::depth_level(0, 2).price->str(), "0");
    EXPECT_FALSE(ctp::depth_level(100, -1).quantity);
    MarketQuote quote;
    quote.instrument = {"SHFE", "rb2610"};
    quote.bid_levels[0] = ctp::depth_level(100.00000001, 0);
    quote.ask_levels[3] = ctp::depth_level(105, 9);
    LiveMarketSnapshot state;
    state.subscriptions.push_back(
        {quote.instrument, asterion::SubscriptionState::subscribed, 0, quote});
    quote.bid = Decimal::parse("100.5");
    quote.bid_quantity = 3;
    state.subscriptions[0].quote = quote;
    const auto encoded = protocol::encode_board(state).rows(0).quote();
    ASSERT_EQ(encoded.bids_size(), 5);
    EXPECT_EQ(encoded.bids(0).price().text(), "100.5");
    EXPECT_EQ(encoded.bids(0).lots(), 3);
    EXPECT_EQ(encoded.bids(1).price().text(), "100.00000001");
    EXPECT_EQ(encoded.bids(1).lots(), 0);
    EXPECT_FALSE(encoded.bids(2).has_price());
    EXPECT_FALSE(encoded.bids(2).has_lots());
    ASSERT_EQ(encoded.asks_size(), 5);
    EXPECT_FALSE(encoded.asks(0).has_price());
    EXPECT_EQ(encoded.asks(4).price().text(), "105");
}

TEST(Ctp, SessionPricesPreserveExactValuesAbsenceAndZero)
{
    using namespace asterion;
    MarketQuote quote;
    quote.instrument = {"SHFE", "rb2610"};
    quote.open = Decimal::parse("3490.25000001");
    quote.upper_limit = Decimal::parse("0");
    LiveMarketSnapshot state;
    state.subscriptions.push_back(
        {quote.instrument, asterion::SubscriptionState::subscribed, 0, quote});
    const auto encoded = protocol::encode_board(state).rows(0).quote();
    EXPECT_EQ(encoded.open().text(), "3490.25000001");
    EXPECT_EQ(encoded.upper_limit().text(), "0");
    EXPECT_FALSE(encoded.has_lower_limit());
}

TEST(Ctp, OpenInterestChangeUsesProviderReferenceAndPreservesMissing)
{
    using namespace asterion;
    EXPECT_EQ(ctp::open_interest_change(100, 125.25)->str(), "-25.25");
    EXPECT_EQ(ctp::open_interest_change(125.25, 100)->str(), "25.25");
    EXPECT_EQ(ctp::open_interest_change(100, 100)->str(), "0");
    EXPECT_EQ(ctp::open_interest_change(100, 0)->str(), "100");
    EXPECT_EQ(ctp::open_interest_change(100.00000001, 100)->str(), "0.00000001");
    EXPECT_FALSE(ctp::open_interest_change(-1, 100));
    EXPECT_FALSE(ctp::open_interest_change(100, -1));
    EXPECT_FALSE(ctp::open_interest_change(100, std::numeric_limits<double>::max()));
    EXPECT_FALSE(ctp::open_interest_change(std::numeric_limits<double>::quiet_NaN(), 100));
    MarketQuote quote;
    quote.instrument = {"SHFE", "rb2610"};
    quote.open_interest_change = ctp::open_interest_change(100, 125.25);
    quote.previous_close = Decimal::parse("3480.12500001");
    LiveMarketSnapshot state;
    state.subscriptions.push_back(
        {quote.instrument, asterion::SubscriptionState::subscribed, 0, quote});
    const auto encoded = protocol::encode_board(state).rows(0).quote();
    EXPECT_EQ(encoded.open_interest_change().text(), "-25.25");
    EXPECT_EQ(encoded.previous_close().text(), "3480.12500001");
    state.subscriptions[0].quote->previous_close.reset();
    state.subscriptions[0].quote->open_interest_change.reset();
    const auto absent = protocol::encode_board(state).rows(0).quote();
    EXPECT_FALSE(absent.has_open_interest_change());
    EXPECT_FALSE(absent.has_previous_close());
}

TEST(CtpPublication, StreamSendsChangedRowsAfterAFullFrameAndStartsOverOnReconnect)
{
    using namespace asterion;
    using namespace std::chrono_literals;
    namespace wire = ::market;
    CtpDirectory directory;
    testing_support::MarketHost host(directory.path, "publication");
    host.start();
    std::uint64_t ids = 0;
    auto receive = [](testing_support::MarketChannel& channel) {
        wire::Response response;
        if (!response.ParseFromString(channel.receive(testing_support::bound(2s))))
            throw std::runtime_error("invalid market response");
        return response;
    };
    auto call = [&](wire::Request request) {
        request.set_id(++ids);
        auto channel = host.open();
        channel.send(request.SerializeAsString(), testing_support::bound(2s));
        return receive(channel);
    };
    wire::Request status;
    status.mutable_get_status();
    const auto deadline = std::chrono::steady_clock::now() + testing_support::bound(5s);
    while (call(status).get_status().out_of_order_quotes() < 50) {
        ASSERT_LT(std::chrono::steady_clock::now(), deadline);
        std::this_thread::sleep_for(10ms);
    }
    // Feed callbacks and minute aggregation advance independently. The last
    // initial instrument proves the ordered aggregator consumed all 50 rows;
    // only rb1000 receives subsequent fixture ticks.
    wire::Request minutes;
    minutes.mutable_get_minutes()->set_exchange("SHFE");
    minutes.mutable_get_minutes()->set_symbol("rb1049");
    for (;;) {
        const auto response = call(minutes);
        ASSERT_TRUE(response.has_get_minutes());
        if (response.get_minutes().bars_size()) {
            ASSERT_EQ(response.get_minutes().bars_size(), 1);
            ASSERT_EQ(response.get_minutes().bars(0).close().text(), "3510");
            break;
        }
        ASSERT_LT(std::chrono::steady_clock::now(), deadline);
        std::this_thread::sleep_for(10ms);
    }
    wire::Request watch;
    watch.set_id(++ids);
    watch.mutable_stream_quotes();
    auto channel = host.open();
    channel.send(watch.SerializeAsString(), testing_support::bound(2s));
    const auto first_frame = receive(channel).stream_quotes();
    ASSERT_TRUE(first_frame.has_full());
    const auto& first = first_frame.full();
    ASSERT_EQ(first.rows_size(), 50);
    ASSERT_TRUE(first.has_catalog());
    bool saw_intraday = false;
    std::size_t bytes = 0;
    // The page's table: the first frame, then each changed row replaced in place.
    auto merged = first;
    for (int i = 0; i < 10; ++i) {
        const auto update = receive(channel).stream_quotes();
        ASSERT_TRUE(update.has_changed());
        const auto& changed = update.changed();
        ASSERT_FALSE(changed.has_catalog());
        // An unchanged frame is valid if no callback ran in this interval.
        // Every changed row must belong to the sole actively ticking instrument.
        ASSERT_LE(changed.rows_size(), 1);
        if (changed.rows_size()) {
            EXPECT_EQ(changed.rows(0).instrument().symbol(), "rb1000");
            saw_intraday |= changed.rows(0).has_change_1m_percent();
        }
        bytes += changed.ByteSizeLong();
        for (const auto& row : changed.rows()) {
            const auto index = std::stoi(row.instrument().symbol().substr(2)) - 1000;
            *merged.mutable_rows(index) = row;
        }
    }
    EXPECT_TRUE(saw_intraday);
    EXPECT_LT(bytes, first.ByteSizeLong() * 10 / 5);
    auto reconnect = host.open();
    reconnect.send(watch.SerializeAsString(), testing_support::bound(2s));
    const auto reset_frame = receive(reconnect).stream_quotes();
    ASSERT_TRUE(reset_frame.has_full());
    const auto& reset = reset_frame.full();
    EXPECT_EQ(reset.rows_size(), 50);
    EXPECT_TRUE(reset.has_catalog());
    // Rows that stopped ticking are what the merged table holds.
    for (int row = 1; row < merged.rows_size(); ++row)
        EXPECT_EQ(merged.rows(row).SerializeAsString(), reset.rows(row).SerializeAsString());
    // More simultaneous streams than the former request worker pool. Each
    // retains its own full baseline; neither queries nor stop wait for a
    // stream to release a worker.
    std::vector<testing_support::MarketChannel> observers;
    for (int i = 0; i < 30; ++i) {
        auto observer = host.open();
        observer.send(watch.SerializeAsString(), testing_support::bound(2s));
        EXPECT_TRUE(receive(observer).stream_quotes().has_full());
        observers.push_back(std::move(observer));
    }
    auto excess = host.open();
    excess.send(watch.SerializeAsString(), testing_support::bound(2s));
    const auto rejected = receive(excess);
    ASSERT_TRUE(rejected.has_error());
    EXPECT_EQ(rejected.error().code(), ::common::ERROR_CODE_RESOURCE_EXHAUSTED);
    EXPECT_TRUE(call(status).has_get_status());
    ASSERT_EQ(::kill(static_cast<pid_t>(host.process->id()), SIGTERM), 0);
    ASSERT_TRUE(host.process->wait(testing_support::bound(5s)));
    EXPECT_EQ(host.process->exit_code(), 0);
    RecordProperty("initial_frame_bytes", std::to_string(first.ByteSizeLong()));
    RecordProperty("ten_update_frame_bytes", std::to_string(bytes));
}

TEST(CtpEvents, FullMarketSubscriptionsAreBatched)
{
    CtpDirectory directory;
    asterion::ctp::Feed feed(directory.sdk, ASTERION_TEST_CTP, directory.path);
    std::vector<asterion::InstrumentId> instruments;
    for (int i = 1000; i < 1300; ++i)
        instruments.push_back({"SHFE", "rb" + std::to_string(i)});
    feed.connect({"tcp://localhost:12345", "test", "test", "test-only"}, instruments);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool complete = false;
    while (std::chrono::steady_clock::now() < deadline) {
        feed.poll();
        const auto state = feed.snapshot();
        complete = state.subscriptions.size() == instruments.size()
            && std::all_of(
                state.subscriptions.begin(),
                state.subscriptions.end(),
                [](const auto& row) {
                    return row.quote.has_value()
                        && row.state == asterion::SubscriptionState::subscribed;
                });
        if (complete)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(complete);
}
