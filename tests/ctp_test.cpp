#include "ctp_feed.hpp"
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/protocol/market.hpp>
#include <gtest/gtest.h>
#include <limits>
#include <thread>
TEST(Ctp, RejectsAliasesAndDuplicates) {
  EXPECT_NO_THROW(asterion::ctp::validate_instruments(
      {{"SHFE", "rb2610"}, {"CZCE", "SR701"}}));
  EXPECT_THROW(asterion::ctp::validate_instruments({{"SHFE", "rbMAIN"}}),
               std::invalid_argument);
  EXPECT_THROW(asterion::ctp::validate_instruments(
                   {{"SHFE", "rb2610"}, {"DCE", "rb2610"}}),
               std::invalid_argument);
}
TEST(Ctp, MissingNumbersAndSourceClockRemainExplicit) {
  EXPECT_FALSE(asterion::ctp::price(std::numeric_limits<double>::max()));
  EXPECT_FALSE(asterion::ctp::price(std::numeric_limits<double>::quiet_NaN()));
  ASSERT_TRUE(asterion::ctp::price(3510.2));
  EXPECT_EQ(asterion::ctp::price(3510.2)->str(), "3510.2");
  EXPECT_EQ(asterion::ctp::source_time("20260926", "09:00:00", 0),
            1790384400000);
  EXPECT_EQ(asterion::ctp::source_time("20260230", "09:00:00", 0), 0);
  EXPECT_EQ(asterion::ctp::source_time("", "09:00:00", 0), 0);
  EXPECT_EQ(asterion::ctp::source_time("20260926", "24:00:00", 0), 0);
}
TEST(Ctp, ProtocolPreservesMissingPricesAndSourceDates) {
  asterion::LiveMarketSnapshot state;
  asterion::MarketQuote q;
  q.instrument = {"SHFE", "rb2610"};
  q.action_day = "20260925";
  q.trading_day = "20260928";
  q.source_ms = 0;
  state.subscriptions.push_back({q.instrument, "subscribed", 0, q});
  auto result = asterion::protocol::decode_market(
      asterion::protocol::encode_market(state, "test"));
  EXPECT_TRUE(result["subscriptions"][0]["quote"]["last"].is_null());
  EXPECT_EQ(result["subscriptions"][0]["quote"]["action_day"], "20260925");
  EXPECT_EQ(result["subscriptions"][0]["quote"]["source_ms"], 0);
}

namespace {
struct CtpDirectory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("asterion-ctp-events-" + asterion::unique_process_id());
  CtpDirectory() { std::filesystem::create_directory(path); }
  ~CtpDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};
bool wait_quotes(asterion::ctp::Feed &feed, std::uint64_t rejected) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    if (feed.snapshot().out_of_order >= rejected)
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}
void connect_fixture(asterion::ctp::Feed &feed) {
  feed.connect({"tcp://localhost:12345", "test", "test", "test-only"},
               {{"SHFE", "rb2610"}});
}
} // namespace
TEST(CtpEvents, PreservesPreCoalescingQuotesAndReconnectBoundaries) {
  CtpDirectory directory;
  asterion::ctp::Feed feed(ASTERION_TEST_CTP, directory.path);
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
  for (const auto &event : batch.events) {
    EXPECT_EQ(event.sequence, ++sequence);
    EXPECT_GT(event.received_ms, 0);
    if (const auto *q =
            std::get_if<asterion::MarketQuoteObservation>(&event.value)) {
      quotes.push_back(*q);
    } else {
      const auto &status = std::get<asterion::LiveMarketSnapshot>(event.value);
      for (const auto &subscription : status.subscriptions)
        EXPECT_FALSE(subscription.quote);
      if (status.phase == "reconnecting") {
        reconnecting = true;
        EXPECT_EQ(status.error_code, 4097);
      }
      if (reconnecting && status.phase == "connected")
        reconnected = true;
    }
  }
  ASSERT_EQ(quotes.size(), 4U);
  EXPECT_EQ(quotes[0].quote.last->str(), "3510");
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
  asterion::ctp::Feed feed(ASTERION_TEST_CTP, directory.path, 2);
  connect_fixture(feed);
  ASSERT_TRUE(wait_quotes(feed, 1));
  feed.disconnect();
  const auto batch = feed.events_after("", 0, 10);
  EXPECT_TRUE(batch.gap);
  EXPECT_FALSE(batch.failed);
  ASSERT_EQ(batch.events.size(), 2U);
  EXPECT_EQ(batch.events.front().sequence, batch.oldest_sequence);
  EXPECT_EQ(batch.events.back().sequence, batch.latest_sequence);
  EXPECT_FALSE(
      feed.events_after(batch.stream_id, batch.oldest_sequence - 1, 10).gap);
  EXPECT_THROW(feed.events_after("another-feed", 0, 10), std::invalid_argument);
  EXPECT_THROW(feed.events_after("", 1, 10), std::invalid_argument);
  EXPECT_THROW(
      feed.events_after(batch.stream_id, batch.latest_sequence + 1, 10),
      std::invalid_argument);
  EXPECT_THROW(feed.events_after(batch.stream_id, 0, 0), std::invalid_argument);
  EXPECT_THROW(feed.events_after(batch.stream_id, 0, 1025),
               std::invalid_argument);
  asterion::ctp::Feed other(ASTERION_TEST_CTP, directory.path / "other");
  EXPECT_NE(other.events_after("", 0, 1).stream_id, batch.stream_id);
  EXPECT_THROW(other.events_after(batch.stream_id, 0, 10),
               std::invalid_argument);
  EXPECT_THROW((asterion::ctp::Feed(ASTERION_TEST_CTP, directory.path, 0)),
               std::invalid_argument);
}

TEST(CtpEvents, ProcessExposesBoundedProtobufReadsAndRejectsStaleIdentity) {
  using namespace asterion;
  using namespace std::chrono_literals;
  namespace wire = asterion::market::v1;
  CtpDirectory directory;
#ifdef _WIN32
  const auto endpoint = "asterion.market-events." + unique_process_id();
#else
  const auto endpoint = "/tmp/ast-events-" + unique_process_id() + ".sock";
#endif
  const auto raw = directory.path.u8string();
  const std::vector<std::string> args{
      "--session",     "events.test",
      "--directory",   std::string(raw.begin(), raw.end()),
      "--endpoint",    endpoint,
      "--ctp-library", ASTERION_TEST_CTP};
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
      } catch (const std::exception &) {
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
  auto *config = connect.mutable_connect();
  config->set_front("tcp://localhost:12345");
  config->set_broker("fixture");
  config->set_user("fixture");
  config->set_password("private-fixture-key");
  auto *id = config->add_instruments();
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
    EXPECT_EQ(response.SerializeAsString().find("private-fixture-key"),
              std::string::npos);
    ASSERT_EQ(response.events().events_size(), 1);
    const auto &event = response.events().events(0);
    EXPECT_EQ(event.sequence(), ++cursor);
    if (event.has_quote()) {
      ++quotes;
      reordered += event.quote().out_of_order();
      EXPECT_FALSE(event.quote().quote().has_previous_settlement());
      EXPECT_EQ(event.quote().quote().instrument().symbol(), "rb2610");
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
