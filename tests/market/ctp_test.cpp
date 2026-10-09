#include "ctp_feed.hpp"
#include "support/timing.hpp"
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/protocol/market.hpp>
#include <gtest/gtest.h>
#include <csignal>
#include <unistd.h>
#include <limits>
#include <thread>
TEST(Ctp, RejectsAliasesAndDuplicates) {
  EXPECT_NO_THROW(asterion::ctp::validate_instruments({{"SHFE", "rb2610"}, {"CZCE", "SR701"}}));
  EXPECT_THROW(asterion::ctp::validate_instruments({{"SHFE", "rbMAIN"}}), std::invalid_argument);
  EXPECT_THROW(asterion::ctp::validate_instruments({{"SHFE", "rb2610"}, {"DCE", "rb2610"}}),
               std::invalid_argument);
}
TEST(Ctp, MissingNumbersAndSourceClockRemainExplicit) {
  EXPECT_FALSE(asterion::ctp::price(std::numeric_limits<double>::max()));
  EXPECT_FALSE(asterion::ctp::price(std::numeric_limits<double>::quiet_NaN()));
  ASSERT_TRUE(asterion::ctp::price(3510.2));
  EXPECT_EQ(asterion::ctp::price(3510.2)->str(), "3510.2");
  EXPECT_EQ(asterion::ctp::source_time("20260926", "09:00:00", 0), 1790384400000);
  EXPECT_EQ(asterion::ctp::source_time("20260230", "09:00:00", 0), 0);
  EXPECT_EQ(asterion::ctp::source_time("", "09:00:00", 0), 0);
  EXPECT_EQ(asterion::ctp::source_time("20260926", "24:00:00", 0), 0);
}
TEST(Ctp, VendorPricesPreserveDecimalPrecisionAndBounds) {
  for (const auto& [input, expected] :
       std::vector<std::pair<double, std::string>>{{-0.0, "0"},
                                                   {-12.125, "-12.125"},
                                                   {0.00000001, "0.00000001"},
                                                   {1.234567894, "1.23456789"},
                                                   {1.234567896, "1.2345679"},
                                                   {1e10, "10000000000"}}) {
    const auto converted = asterion::ctp::price(input);
    ASSERT_TRUE(converted) << input;
    EXPECT_EQ(converted->str(), expected);
  }
  EXPECT_FALSE(asterion::ctp::price(1e10 + 1));
  EXPECT_FALSE(asterion::ctp::price(-1e10 - 1));
  EXPECT_FALSE(asterion::ctp::price(std::numeric_limits<double>::infinity()));
}
TEST(Ctp, AveragePriceIsNormalizedPerUnit) {
  using asterion::ctp::average_price;
  EXPECT_EQ(average_price(161550, "SHFE", 10)->str(), asterion::Decimal::parse("16155").str());
  EXPECT_EQ(average_price(10234.5, "CZCE", std::nullopt)->str(),
            asterion::Decimal::parse("10234.5").str());
  EXPECT_FALSE(average_price(161550, "SHFE", std::nullopt));
  EXPECT_FALSE(average_price(0, "DCE", 10));
}
TEST(Ctp, ProtocolPreservesMissingPricesAndSourceDates) {
  asterion::LiveMarketSnapshot state;
  asterion::MarketQuote q;
  q.instrument = {"SHFE", "rb2610"};
  q.action_day = "20260925";
  q.trading_day = "20260928";
  q.source_ms = 0;
  state.subscriptions.push_back({q.instrument, asterion::SubscriptionState::subscribed, 0, q});
  auto result = asterion::protocol::decode_market(asterion::protocol::encode_market(state, "test"));
  EXPECT_TRUE(result["subscriptions"][0]["quote"]["last"].is_null());
  EXPECT_EQ(result["subscriptions"][0]["quote"]["action_day"], "20260925");
  EXPECT_EQ(result["subscriptions"][0]["quote"]["source_ms"], 0);
}

namespace {
struct CtpDirectory {
  asterion::ThreadPool sdk{1, 2};
  std::filesystem::path path = std::filesystem::temp_directory_path() /
                               ("asterion-ctp-events-" + asterion::unique_process_id());
  CtpDirectory() { std::filesystem::create_directory(path); }
  ~CtpDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};
bool wait_quotes(asterion::ctp::Feed& feed, std::uint64_t rejected) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    feed.poll();
    if (feed.snapshot().out_of_order >= rejected)
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}
void connect_fixture(asterion::ctp::Feed& feed) {
  feed.connect({"tcp://localhost:12345", "test", "test", "test-only"}, {{"SHFE", "rb2610"}});
}
} // namespace
TEST(CtpEvents, OwnerAppliesCallbacksAndReconnectWaitsOnlyOnTheSdkThread) {
  using namespace asterion;
  using namespace std::chrono_literals;
  CtpDirectory directory;
  ctp::Feed feed(directory.sdk, ASTERION_TEST_CTP, directory.path);
  const auto release = directory.path / "hold-release";
  const auto init = directory.path / "hold-init";
  const auto publication = directory.path / "hold-publication";
  struct Unblock {
    std::filesystem::path release, init, publication;
    ~Unblock() {
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
  feed.connect({"tcp://localhost:12345", "test", "publication", "test-only"}, {{"SHFE", "rb2610"}});
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

TEST(CtpEvents, CallbackOverflowFailsTheStreamInsteadOfPublishingASilentGap) {
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
  feed.poll();
  EXPECT_EQ(feed.phase(), asterion::MarketPhase::error);
  EXPECT_TRUE(feed.events_after("", 0, 1024).failed);
  EXPECT_THROW(feed.connect({"tcp://localhost:12345", "test", "steady", "test-only"}, {}), Error);
  feed.disconnect();
  feed.poll();
  EXPECT_EQ(feed.phase(), asterion::MarketPhase::disconnected);
  EXPECT_TRUE(feed.events_after("", 0, 1024).failed);
}

TEST(CtpEvents, PreservesPreCoalescingQuotesAndReconnectBoundaries) {
  CtpDirectory directory;
  asterion::ctp::Feed feed(directory.sdk, ASTERION_TEST_CTP, directory.path);
  auto initial = feed.events_after("", 0, 100);
  EXPECT_FALSE(initial.stream_id.empty());
  EXPECT_TRUE(initial.events.empty());
  connect_fixture(feed);
  ASSERT_TRUE(wait_quotes(feed, 2));
  feed.disconnect();
  const auto batch = feed.events_after(initial.stream_id, 0, 100);
  ASSERT_FALSE(batch.gap);
  ASSERT_FALSE(batch.failed);
  std::vector<asterion::MarketQuoteObservation> quotes;
  bool reconnecting = false, reconnected = false;
  std::uint64_t sequence = 0;
  for (const auto& event : batch.events) {
    EXPECT_EQ(event.sequence, ++sequence);
    EXPECT_GT(event.received_ms, 0);
    if (const auto* q = std::get_if<asterion::MarketQuoteObservation>(&event.value)) {
      quotes.push_back(*q);
    } else if (const auto* status = std::get_if<asterion::LiveMarketSnapshot>(&event.value)) {
      for (const auto& subscription : status->subscriptions)
        EXPECT_FALSE(subscription.quote);
      if (status->phase == asterion::MarketPhase::reconnecting) {
        reconnecting = true;
        EXPECT_EQ(status->error_code, 4097);
      }
      if (reconnecting && status->phase == asterion::MarketPhase::connected)
        reconnected = true;
    } else {
      const auto& subscription = std::get<asterion::MarketSubscription>(event.value);
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
  const auto decoded_depth = asterion::protocol::decode_market(
      asterion::protocol::encode_market(feed.snapshot(), "depth"));
  EXPECT_EQ(decoded_depth["subscriptions"][0]["quote"]["bid_levels"][0]["price"], "3508.25");
  EXPECT_EQ(quotes[1].quote.last->str(), "1");
  EXPECT_FALSE(quotes[0].out_of_order);
  EXPECT_TRUE(quotes[1].out_of_order);
  EXPECT_FALSE(quotes[1].quote.previous_settlement);
  EXPECT_EQ(quotes[1].quote.volume, 5);
  EXPECT_TRUE(reconnected);
  EXPECT_EQ(feed.snapshot().subscriptions[0].quote->last->str(), "3510");
  EXPECT_EQ(batch.latest_sequence, sequence);
  // Reads never acknowledge or remove records; a failed durable write can
  // retry.
  const auto retry = feed.events_after(batch.stream_id, 0, 100);
  EXPECT_EQ(retry.events.size(), batch.events.size());
  const auto tail = feed.events_after(batch.stream_id, sequence, 100);
  EXPECT_TRUE(tail.events.empty());
  EXPECT_FALSE(tail.gap);
  const auto page = feed.events_after(batch.stream_id, 2, 1);
  ASSERT_EQ(page.events.size(), 1U);
  EXPECT_EQ(page.events[0].sequence, 3U);
}
TEST(CtpEvents, OverflowAndWrongStreamCannotLookComplete) {
  CtpDirectory directory;
  asterion::ctp::Feed feed(directory.sdk, ASTERION_TEST_CTP, directory.path, 2);
  connect_fixture(feed);
  ASSERT_TRUE(wait_quotes(feed, 1));
  feed.disconnect();
  const auto batch = feed.events_after("", 0, 10);
  EXPECT_TRUE(batch.gap);
  EXPECT_FALSE(batch.failed);
  ASSERT_EQ(batch.events.size(), 2U);
  EXPECT_EQ(batch.events.front().sequence, batch.oldest_sequence);
  EXPECT_EQ(batch.events.back().sequence, batch.latest_sequence);
  EXPECT_FALSE(feed.events_after(batch.stream_id, batch.oldest_sequence - 1, 10).gap);
  EXPECT_THROW(feed.events_after("another-feed", 0, 10), std::invalid_argument);
  EXPECT_THROW(feed.events_after("", 1, 10), std::invalid_argument);
  EXPECT_THROW(feed.events_after(batch.stream_id, batch.latest_sequence + 1, 10),
               std::invalid_argument);
  EXPECT_THROW(feed.events_after(batch.stream_id, 0, 0), std::invalid_argument);
  EXPECT_THROW(feed.events_after(batch.stream_id, 0, 1025), std::invalid_argument);
  asterion::ctp::Feed other(directory.sdk, ASTERION_TEST_CTP, directory.path / "other");
  EXPECT_NE(other.events_after("", 0, 1).stream_id, batch.stream_id);
  EXPECT_THROW(other.events_after(batch.stream_id, 0, 10), std::invalid_argument);
  EXPECT_THROW((asterion::ctp::Feed(directory.sdk, ASTERION_TEST_CTP, directory.path, 0)),
               std::invalid_argument);
}

TEST(CtpEvents, SubscriptionReorderingAndVenueReplacementKeepQuotesBoundToExactContracts) {
  CtpDirectory directory;
  asterion::ctp::Feed feed(directory.sdk, ASTERION_TEST_CTP, directory.path);
  feed.connect({"tcp://localhost:12345", "test", "test", "test-only"},
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

TEST(CtpEvents, FullCatalogAcknowledgementsRetainOnlyTheirChangedContract) {
  using namespace asterion;
  CtpDirectory directory;
  ctp::Feed feed(directory.sdk, ASTERION_TEST_CTP, directory.path, 65536);
  constexpr std::size_t count = 20000;
  std::vector<InstrumentId> contracts;
  for (std::size_t i = 0; i < count; ++i)
    contracts.push_back({"SHFE", std::string(1, static_cast<char>('a' + i / 9000)) +
                                     std::to_string(1000 + i % 9000)});
  const auto began = std::chrono::steady_clock::now();
  feed.connect({"tcp://localhost:12345", "test", "steady", "test-only"}, contracts);
  const auto deadline = began + testing_support::bound(std::chrono::seconds(10));
  for (;;) {
    feed.poll();
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
  std::string stream;
  std::uint64_t cursor = 0;
  std::size_t acknowledgements = 0, quotes = 0, acknowledgement_bytes = 0;
  for (;;) {
    const auto batch = feed.events_after(stream, cursor, 1024);
    ASSERT_FALSE(batch.failed);
    const auto wire = protocol::encode_market_events(batch);
    for (const auto& event : wire.events()) {
      if (event.has_subscription()) {
        ++acknowledgements;
        EXPECT_FALSE(event.subscription().has_quote());
        acknowledgement_bytes += event.ByteSizeLong();
      } else if (event.has_quote()) {
        ++quotes;
      }
      cursor = event.sequence();
    }
    stream = batch.stream_id;
    if (cursor == batch.latest_sequence)
      break;
    ASSERT_FALSE(batch.events.empty());
  }
  EXPECT_EQ(acknowledgements, count);
  EXPECT_EQ(quotes, count * 2);
  EXPECT_LT(acknowledgement_bytes, count * 96);
  RecordProperty("contracts", static_cast<int>(count));
  RecordProperty("subscription_and_quote_ms", std::to_string(elapsed));
  RecordProperty("acknowledgement_bytes", std::to_string(acknowledgement_bytes));
  const auto full = feed.snapshot();
  EXPECT_FALSE(full.subscriptions_delta);
  const auto quiet = feed.snapshot(full.sequence);
  EXPECT_TRUE(quiet.subscriptions_delta);
  EXPECT_TRUE(quiet.subscriptions.empty());
  EXPECT_EQ(quiet.sequence, full.sequence);
  const std::vector<InstrumentId> late_intraday{
      contracts.back(), contracts.front(), contracts.front(), {"DCE", contracts.front().symbol}};
  const auto forced = feed.snapshot(full.sequence, late_intraday);
  ASSERT_EQ(forced.subscriptions.size(), 2U);
  EXPECT_EQ(forced.subscriptions[0].instrument, contracts.front());
  EXPECT_EQ(forced.subscriptions[1].instrument, contracts.back());
  EXPECT_THROW(feed.snapshot(full.sequence + 1), std::invalid_argument);
  RecordProperty("full_quote_frame_bytes",
                 std::to_string(protocol::encode_market(full, "sample").ByteSizeLong()));
  RecordProperty("unchanged_quote_frame_bytes",
                 std::to_string(protocol::encode_market(quiet, "sample").ByteSizeLong()));
  feed.subscribe({contracts.front(), contracts.back()});
  const auto reset = feed.snapshot(full.sequence);
  EXPECT_FALSE(reset.subscriptions_delta);
  EXPECT_EQ(reset.subscriptions.size(), 2U);
}

TEST(CtpPublication, ContinuousQuotesChangeOnlyTheirRowAndMatchFullSnapshots) {
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
  auto merged = protocol::decode_market(protocol::encode_market(full, "fixture"));
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
      const auto encoded = protocol::encode_market(delta, "fixture");
      bytes += encoded.ByteSizeLong();
      auto decoded = protocol::decode_market(encoded);
      merged["subscriptions"][0] = decoded["subscriptions"][0];
      merged["sequence"] = decoded["sequence"];
      merged["out_of_order"] = decoded["out_of_order"];
      ++updates;
    }
    cursor = delta.sequence;
    const auto verify = feed.snapshot();
    EXPECT_EQ(verify.sequence, cursor);
    EXPECT_EQ(merged, protocol::decode_market(protocol::encode_market(verify, "fixture")));
    ASSERT_LT(std::chrono::steady_clock::now(), deadline);
    std::this_thread::sleep_for(10ms);
  }
  RecordProperty("changed_frames", std::to_string(updates));
  RecordProperty("changed_frame_bytes", std::to_string(bytes));
  RecordProperty("publication_ms",
                 std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - began)
                                    .count()));
}

TEST(CtpPublication, CatalogRevisionIsOmittedOnlyWithinItsWatchConnection) {
  using namespace asterion;
  using namespace std::chrono_literals;
  namespace wire = asterion::market::v1;
  CtpDirectory directory;
  wire::CatalogState catalog;
  catalog.set_phase("ready");
  catalog.set_trading_day("20261003");
  constexpr int count = 20000;
  for (int i = 0; i < count; ++i) {
    auto* row = catalog.add_contracts();
    row->mutable_instrument()->set_venue("SHFE");
    row->mutable_instrument()->set_symbol(std::string(1, static_cast<char>('a' + i / 9000)) +
                                          std::to_string(1000 + i % 9000));
    row->set_price_tick("1");
    row->set_multiplier(10);
  }
  replace_file_durably(directory.path / "ctp-catalog.pb", catalog.SerializeAsString());
  const auto endpoint = "/tmp/ast-catalog-" + unique_process_id() + ".sock";
  ChildProcess process(ASTERION_MARKET_PATH, {"--session", "catalog.test", "--directory",
                                              directory.path.string(), "--endpoint", endpoint});
  const auto open = [&] {
    const auto deadline = std::chrono::steady_clock::now() + testing_support::bound(5s);
    for (;;) {
      try {
        return ipc::Channel::connect(endpoint, 100ms);
      } catch (const std::exception&) {
        if (process.exited() || std::chrono::steady_clock::now() >= deadline)
          throw;
        std::this_thread::sleep_for(10ms);
      }
    }
  };
  wire::Request watch;
  watch.set_version(1);
  watch.set_service_id("catalog.test");
  watch.set_correlation_id("catalog.watch");
  watch.mutable_watch();
  auto connection = open();
  connection.send(watch.SerializeAsString(), testing_support::bound(2s));
  const auto receive = [&](ipc::Channel& channel) {
    wire::Response response;
    if (!response.ParseFromString(channel.receive(testing_support::bound(2s))) ||
        !response.has_snapshot())
      throw std::runtime_error("expected catalog snapshot");
    return response.snapshot();
  };
  const auto first = receive(connection);
  ASSERT_TRUE(first.has_catalog());
  EXPECT_EQ(first.catalog().contracts_size(), count);
  EXPECT_EQ(first.catalog().phase(), "cached");
  EXPECT_FALSE(first.catalog_omitted());
  const auto second = receive(connection);
  EXPECT_EQ(second.catalog_revision(), first.catalog_revision());
  EXPECT_FALSE(second.has_catalog());
  EXPECT_TRUE(second.catalog_omitted());
  EXPECT_LT(second.ByteSizeLong(), first.ByteSizeLong() / 100);
  RecordProperty("catalog_contracts", count);
  RecordProperty("initial_frame_bytes", std::to_string(first.ByteSizeLong()));
  RecordProperty("unchanged_frame_bytes", std::to_string(second.ByteSizeLong()));
  auto mutation = open();
  auto disconnect = watch;
  disconnect.clear_watch();
  disconnect.mutable_disconnect();
  mutation.send(disconnect.SerializeAsString(), testing_support::bound(2s));
  const auto changed = receive(mutation);
  EXPECT_GT(changed.catalog_revision(), first.catalog_revision());
  EXPECT_TRUE(changed.has_catalog());
  auto update = receive(connection);
  for (int attempt = 0; attempt < 8 && update.catalog_revision() == first.catalog_revision();
       ++attempt)
    update = receive(connection);
  ASSERT_GT(update.catalog_revision(), first.catalog_revision());
  EXPECT_EQ(update.catalog().contracts_size(), count);
  EXPECT_FALSE(update.catalog_omitted());
  auto reconnected = open();
  reconnected.send(watch.SerializeAsString(), testing_support::bound(2s));
  const auto initial = receive(reconnected);
  EXPECT_EQ(initial.catalog_revision(), changed.catalog_revision());
  EXPECT_EQ(initial.catalog().contracts_size(), count);
  EXPECT_FALSE(initial.catalog_omitted());
}

TEST(CtpEvents, ProcessExposesBoundedProtobufReadsAndRejectsStaleIdentity) {
  using namespace asterion;
  using namespace std::chrono_literals;
  namespace wire = asterion::market::v1;
  CtpDirectory directory;
  const auto endpoint = "/tmp/ast-events-" + unique_process_id() + ".sock";
  const auto raw = directory.path.u8string();
  const std::vector<std::string> args{
      "--session",  "events.test", "--directory",   std::string(raw.begin(), raw.end()),
      "--endpoint", endpoint,      "--ctp-library", ASTERION_TEST_CTP};
  auto process = std::make_unique<ChildProcess>(ASTERION_MARKET_PATH, args);
  auto call = [&](wire::Request request) {
    request.set_version(1);
    request.set_service_id("events.test");
    request.set_correlation_id("events.request");
    ipc::Channel channel;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    for (;;) {
      try {
        channel = ipc::Channel::connect(endpoint, 100ms);
        break;
      } catch (const std::exception&) {
        if (process->exited() || std::chrono::steady_clock::now() >= deadline)
          throw;
        std::this_thread::sleep_for(10ms);
      }
    }
    channel.send(request.SerializeAsString(), 2s);
    wire::Response response;
    if (!response.ParseFromString(channel.receive(2s)))
      throw std::runtime_error("invalid market response");
    EXPECT_EQ(response.correlation_id(), request.correlation_id());
    return response;
  };
  wire::Request read;
  read.mutable_events()->set_limit(1);
  EXPECT_EQ(call(read).error().code(), "unavailable");
  wire::Request connect;
  auto* config = connect.mutable_connect();
  config->set_front("tcp://localhost:12345");
  config->set_broker("fixture");
  config->set_user("fixture");
  config->set_password("private-fixture-key");
  auto* id = config->add_instruments();
  id->set_venue("SHFE");
  id->set_symbol("rb2610");
  ASSERT_TRUE(call(connect).has_snapshot());
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  for (;;) {
    wire::Request request;
    request.mutable_snapshot();
    if (call(request).snapshot().out_of_order() >= 1)
      break;
    ASSERT_LT(std::chrono::steady_clock::now(), deadline);
    std::this_thread::sleep_for(10ms);
  }
  wire::Request disconnect;
  disconnect.mutable_disconnect();
  ASSERT_TRUE(call(disconnect).has_snapshot());
  const auto first = call(read);
  ASSERT_TRUE(first.has_events());
  ASSERT_EQ(first.events().events_size(), 1);
  EXPECT_FALSE(first.events().gap());
  EXPECT_FALSE(first.events().failed());
  EXPECT_EQ(first.SerializeAsString(), call(read).SerializeAsString());
  std::uint64_t cursor = 0;
  unsigned quotes = 0, reordered = 0;
  do {
    read.mutable_events()->set_stream_id(first.events().stream_id());
    read.mutable_events()->set_after_sequence(cursor);
    const auto response = call(read);
    ASSERT_TRUE(response.has_events());
    EXPECT_EQ(response.SerializeAsString().find("private-fixture-key"), std::string::npos);
    ASSERT_EQ(response.events().events_size(), 1);
    const auto& event = response.events().events(0);
    EXPECT_EQ(event.sequence(), ++cursor);
    if (event.has_quote()) {
      ++quotes;
      reordered += event.quote().out_of_order();
      EXPECT_FALSE(event.quote().quote().has_previous_settlement());
      EXPECT_EQ(event.quote().quote().open(), "3490.25");
      EXPECT_EQ(event.quote().quote().previous_close(), "3480.125");
      EXPECT_EQ(event.quote().quote().open_interest_change(), "-25.25");
      EXPECT_EQ(event.quote().quote().upper_limit(), "3800.5");
      EXPECT_FALSE(event.quote().quote().has_lower_limit());
      EXPECT_EQ(event.quote().quote().instrument().symbol(), "rb2610");
    } else if (event.has_subscription()) {
      EXPECT_EQ(event.subscription().instrument().symbol(), "rb2610");
      EXPECT_EQ(event.subscription().state(), "subscribed");
      EXPECT_FALSE(event.subscription().has_quote());
    } else {
      ASSERT_TRUE(event.has_status());
    }
  } while (cursor < first.events().latest_sequence());
  EXPECT_EQ(quotes, 2U);
  EXPECT_EQ(reordered, 1U);
  read.mutable_events()->set_after_sequence(cursor);
  EXPECT_EQ(call(read).events().events_size(), 0);
  read.mutable_events()->set_limit(1025);
  EXPECT_EQ(call(read).error().code(), "invalid_request");
  read.mutable_events()->set_limit(1);
  process.reset();
  process = std::make_unique<ChildProcess>(ASTERION_MARKET_PATH, args);
  ASSERT_TRUE(call(connect).has_snapshot());
  EXPECT_EQ(call(read).error().code(), "invalid_request");
}

TEST(Ctp, DepthPreservesMissingLevelsZeroSizeAndExactPrices) {
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
  const auto encoded = protocol::encode_market(state, "depth");
  const auto decoded = protocol::decode_market(encoded)["subscriptions"][0]["quote"];
  ASSERT_EQ(decoded["bid_levels"].size(), 4);
  EXPECT_EQ(decoded["bid_levels"][0]["price"], "100.00000001");
  EXPECT_EQ(decoded["bid_levels"][0]["quantity"], 0);
  EXPECT_TRUE(decoded["bid_levels"][1]["price"].is_null());
  EXPECT_TRUE(decoded["bid_levels"][1]["quantity"].is_null());
  auto bad = encoded;
  bad.mutable_subscriptions(0)->mutable_quote()->mutable_bid_levels()->RemoveLast();
  EXPECT_THROW(protocol::decode_market(bad), std::invalid_argument);
  bad = encoded;
  bad.mutable_subscriptions(0)->mutable_quote()->mutable_bid_levels(0)->set_quantity(-1);
  EXPECT_THROW(protocol::decode_market(bad), std::invalid_argument);
  bad = encoded;
  bad.mutable_subscriptions(0)->mutable_quote()->mutable_bid_levels(0)->clear_price();
  EXPECT_THROW(protocol::decode_market(bad), std::invalid_argument);
  bad = encoded;
  bad.mutable_subscriptions(0)->mutable_quote()->mutable_bid_levels(0)->set_price("NaN");
  EXPECT_THROW(protocol::decode_market(bad), std::exception);
}

TEST(Ctp, SessionPricesPreserveExactValuesAbsenceAndZero) {
  using namespace asterion;
  MarketQuote quote;
  quote.instrument = {"SHFE", "rb2610"};
  quote.open = Decimal::parse("3490.25000001");
  quote.upper_limit = Decimal::parse("0");
  LiveMarketSnapshot state;
  state.subscriptions.push_back(
      {quote.instrument, asterion::SubscriptionState::subscribed, 0, quote});
  auto encoded = protocol::encode_market(state, "session-prices");
  const auto decoded = protocol::decode_market(encoded)["subscriptions"][0]["quote"];
  EXPECT_EQ(decoded["open"], "3490.25000001");
  EXPECT_EQ(decoded["upper_limit"], "0");
  EXPECT_TRUE(decoded["lower_limit"].is_null());
  encoded.mutable_subscriptions(0)->mutable_quote()->set_lower_limit("NaN");
  EXPECT_THROW(protocol::decode_market(encoded), std::exception);
}

TEST(Ctp, OpenInterestChangeUsesProviderReferenceAndPreservesMissing) {
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
  const auto decoded = protocol::decode_market(
      protocol::encode_market(state, "reference"))["subscriptions"][0]["quote"];
  EXPECT_EQ(decoded["open_interest_change"], "-25.25");
  EXPECT_EQ(decoded["previous_close"], "3480.12500001");
  state.subscriptions[0].quote->previous_close.reset();
  state.subscriptions[0].quote->open_interest_change.reset();
  const auto absent = protocol::decode_market(
      protocol::encode_market(state, "reference"))["subscriptions"][0]["quote"];
  EXPECT_TRUE(absent["open_interest_change"].is_null());
  EXPECT_TRUE(absent["previous_close"].is_null());
}

TEST(CtpPublication, WatchStreamsChangedQuotesAndIntradayValuesWithAFullReconnectBaseline) {
  using namespace asterion;
  using namespace std::chrono_literals;
  namespace wire = asterion::market::v1;
  CtpDirectory directory;
  const auto endpoint = "/tmp/ast-quotes-" + unique_process_id() + ".sock";
  ChildProcess process(ASTERION_MARKET_PATH,
                       {"--session", "quotes.test", "--directory", directory.path.string(),
                        "--endpoint", endpoint, "--ctp-library", ASTERION_TEST_CTP});
  auto open = [&] {
    const auto deadline = std::chrono::steady_clock::now() + testing_support::bound(5s);
    for (;;) {
      try {
        return ipc::Channel::connect(endpoint, 100ms);
      } catch (const std::exception&) {
        if (process.exited() || std::chrono::steady_clock::now() >= deadline)
          throw;
        std::this_thread::sleep_for(10ms);
      }
    }
  };
  auto identify = [](wire::Request& request) {
    request.set_version(1);
    request.set_service_id("quotes.test");
    request.set_correlation_id(unique_process_id());
  };
  auto receive = [](ipc::Channel& channel) {
    wire::Response response;
    if (!response.ParseFromString(channel.receive(testing_support::bound(2s))) ||
        !response.has_snapshot())
      throw std::runtime_error("expected market snapshot");
    return response.snapshot();
  };
  auto call = [&](wire::Request request) {
    identify(request);
    auto channel = open();
    channel.send(request.SerializeAsString(), testing_support::bound(2s));
    return receive(channel);
  };
  wire::Request login;
  auto* config = login.mutable_connect();
  config->set_front("tcp://localhost:12345");
  config->set_broker("test");
  config->set_user("publication");
  config->set_password("test-only");
  for (int i = 1000; i < 1050; ++i) {
    auto* row = config->add_instruments();
    row->set_venue("SHFE");
    row->set_symbol("rb" + std::to_string(i));
  }
  EXPECT_FALSE(call(login).subscriptions_delta());
  wire::Request read;
  read.mutable_snapshot();
  const auto deadline = std::chrono::steady_clock::now() + testing_support::bound(5s);
  while (call(read).out_of_order() < 50) {
    ASSERT_LT(std::chrono::steady_clock::now(), deadline);
    std::this_thread::sleep_for(10ms);
  }
  // Feed callbacks and minute aggregation advance independently. The last
  // initial instrument proves the ordered aggregator consumed all 50 rows;
  // only rb1000 receives subsequent fixture ticks. Waiting on feed counters
  // alone can leave any subset of initial rows to appear in the first delta.
  wire::Request minutes;
  identify(minutes);
  minutes.mutable_minutes()->mutable_instrument()->set_venue("SHFE");
  minutes.mutable_minutes()->mutable_instrument()->set_symbol("rb1049");
  for (;;) {
    auto reader = open();
    reader.send(minutes.SerializeAsString(), testing_support::bound(2s));
    wire::Response response;
    ASSERT_TRUE(response.ParseFromString(reader.receive(testing_support::bound(2s))));
    ASSERT_TRUE(response.has_minutes());
    if (response.minutes().bars_size()) {
      ASSERT_EQ(response.minutes().bars_size(), 1);
      ASSERT_EQ(response.minutes().bars(0).close(), "3510");
      break;
    }
    ASSERT_LT(std::chrono::steady_clock::now(), deadline);
    std::this_thread::sleep_for(10ms);
  }
  wire::Request watch;
  identify(watch);
  watch.mutable_watch();
  auto channel = open();
  channel.send(watch.SerializeAsString(), testing_support::bound(2s));
  const auto first = receive(channel);
  EXPECT_FALSE(first.subscriptions_delta());
  ASSERT_EQ(first.subscriptions_size(), 50);
  ASSERT_TRUE(first.has_catalog());
  std::uint64_t sequence = first.sequence();
  bool saw_intraday = false;
  std::size_t bytes = 0;
  int exact_comparisons = 0;
  auto merged = protocol::decode_market(first);
  for (int i = 0; i < 10; ++i) {
    const auto delta = receive(channel);
    ASSERT_TRUE(delta.subscriptions_delta());
    ASSERT_EQ(delta.base_sequence(), sequence);
    EXPECT_GE(delta.sequence(), sequence);
    ASSERT_TRUE(delta.catalog_omitted());
    // An unchanged watch frame is valid if no callback ran in this interval.
    // Every changed row must belong to the sole actively ticking instrument.
    ASSERT_LE(delta.subscriptions_size(), 1);
    if (delta.subscriptions_size()) {
      EXPECT_EQ(delta.subscriptions(0).instrument().symbol(), "rb1000");
      saw_intraday |= delta.subscriptions(0).has_change_1m_percent();
    }
    bytes += delta.ByteSizeLong();
    const auto decoded = protocol::decode_market(delta);
    for (const auto& row : decoded.at("subscriptions")) {
      const auto index = std::stoi(row.at("symbol").get<std::string>().substr(2)) - 1000;
      merged["subscriptions"][index] = row;
    }
    merged["sequence"] = decoded.at("sequence");
    merged["out_of_order"] = decoded.at("out_of_order");
    const auto current = call(read);
    EXPECT_FALSE(current.subscriptions_delta());
    EXPECT_TRUE(current.has_catalog());
    if (current.sequence() == delta.sequence()) {
      EXPECT_EQ(merged, protocol::decode_market(current));
      ++exact_comparisons;
    }
    sequence = delta.sequence();
  }
  EXPECT_TRUE(saw_intraday);
  EXPECT_GT(exact_comparisons, 0);
  EXPECT_LT(bytes, first.ByteSizeLong() * 10 / 5);
  auto reconnect = open();
  reconnect.send(watch.SerializeAsString(), testing_support::bound(2s));
  const auto reset = receive(reconnect);
  EXPECT_FALSE(reset.subscriptions_delta());
  EXPECT_EQ(reset.base_sequence(), 0);
  EXPECT_EQ(reset.subscriptions_size(), 50);
  EXPECT_TRUE(reset.has_catalog());
  // More simultaneous watch connections than the former request worker pool.
  // Each retains its own full baseline; neither queries nor stop wait for a
  // watch connection to release a worker.
  std::vector<ipc::Channel> observers;
  for (int i = 0; i < 30; ++i) {
    auto observer = open();
    observer.send(watch.SerializeAsString(), testing_support::bound(2s));
    EXPECT_FALSE(receive(observer).subscriptions_delta());
    observers.push_back(std::move(observer));
  }
  auto excess = open();
  excess.send(watch.SerializeAsString(), testing_support::bound(2s));
  wire::Response rejected;
  ASSERT_TRUE(rejected.ParseFromString(excess.receive(testing_support::bound(2s))));
  ASSERT_TRUE(rejected.has_error());
  EXPECT_EQ(rejected.error().code(), "resource_exhausted");
  EXPECT_EQ(call(read).subscriptions_size(), 50);
  ASSERT_EQ(::kill(static_cast<pid_t>(process.id()), SIGTERM), 0);
  ASSERT_TRUE(process.wait(testing_support::bound(1s)));
  EXPECT_EQ(process.exit_code(), 0);
  EXPECT_FALSE(std::filesystem::exists(endpoint));
  RecordProperty("initial_frame_bytes", std::to_string(first.ByteSizeLong()));
  RecordProperty("ten_update_frame_bytes", std::to_string(bytes));
  RecordProperty("exact_full_comparisons", exact_comparisons);
}

TEST(CtpEvents, FullMarketSubscriptionsAreBatchedAndRetentionRemainsBounded) {
  CtpDirectory directory;
  asterion::ctp::Feed feed(directory.sdk, ASTERION_TEST_CTP, directory.path, 65536);
  std::vector<asterion::InstrumentId> instruments;
  for (int i = 1000; i < 1300; ++i)
    instruments.push_back({"SHFE", "rb" + std::to_string(i)});
  feed.connect({"tcp://localhost:12345", "test", "test", "test-only"}, instruments);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  bool complete = false;
  while (std::chrono::steady_clock::now() < deadline) {
    feed.poll();
    const auto state = feed.snapshot();
    complete =
        state.subscriptions.size() == instruments.size() &&
        std::all_of(state.subscriptions.begin(), state.subscriptions.end(), [](const auto& row) {
          return row.quote.has_value() && row.state == asterion::SubscriptionState::subscribed;
        });
    if (complete)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(complete);
  const auto events = feed.events_after("", 0, 1024);
  EXPECT_FALSE(events.failed);
  EXPECT_FALSE(events.gap);
  EXPECT_EQ(events.oldest_sequence, 1U);
  std::size_t retained_rows = 0;
  for (const auto& event : events.events) {
    const auto* status = std::get_if<asterion::LiveMarketSnapshot>(&event.value);
    retained_rows += 1 + (status ? status->subscriptions.size() : 0);
  }
  EXPECT_LE(retained_rows, 65536U);
  feed.disconnect();
  const auto retained = feed.events_after(events.stream_id, 0, 1024);
  for (const std::size_t page_size : {1U, 17U, 1024U}) {
    auto cursor = retained.oldest_sequence - 1;
    std::size_t index = 0;
    for (;;) {
      const auto page = feed.events_after(retained.stream_id, cursor, page_size);
      ASSERT_FALSE(page.gap);
      ASSERT_LE(page.events.size(), page_size);
      if (page.events.empty())
        break;
      for (const auto& event : page.events) {
        ASSERT_LT(index, retained.events.size());
        EXPECT_EQ(event.sequence, retained.events[index++].sequence);
        cursor = event.sequence;
      }
    }
    EXPECT_EQ(index, retained.events.size());
    EXPECT_EQ(cursor, retained.latest_sequence);
  }
}
