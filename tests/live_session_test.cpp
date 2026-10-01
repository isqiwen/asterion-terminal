#include "ctp_support.hpp"
#include "journal_fixture.hpp"
#include "live_session.hpp"
#include <asterion/foundation/error.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/protocol/trading.hpp>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <thread>
using namespace asterion;
using asterion::trading::LiveSession;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
namespace {
// Test control entry point exported by the CTP SDK double. The handle keeps
// the library, and so its exchange state, loaded across sessions.
struct FakeExchange {
  ctp::SharedLibrary library{ASTERION_TEST_CTP_TRADER, "asterion_fake_trader_reset",
                             "asterion_fake_trader_reset"};
  void reset() { library.symbol<void (*)()>()(); }
};
struct Directory {
  fs::path path = fs::temp_directory_path() / ("asterion-live-" + unique_process_id());
  Directory() { fs::create_directory(path); }
  ~Directory() {
    std::error_code ignored;
    fs::remove_all(path, ignored);
  }
};
Json contract(const char* symbol, const char* product = "rb", const char* month = "2026-10") {
  return {{"venue", "SHFE"},        {"symbol", symbol},       {"currency", "CNY"},
          {"price_increment", "1"}, {"multiplier", "10"},     {"quantity_increment", "1"},
          {"product", product},     {"delivery_month", month}};
}
// The fake fills orders of at most 2 lots at once; larger orders rest. Its
// market for rb2610 is 3500 within 3300..3700.
Json manifest(std::uint64_t working_orders = 1, const char* deviation = "0.02") {
  return {{"version", 2},
          {"type", "live_ctp"},
          {"broker",
           {{"front", "tcp://127.0.0.1:41205"},
            {"broker_id", "9999"},
            {"user_id", "000001"},
            {"app_id", "client_app"}}},
          {"risk",
           {{"max_order_quantity", "5"},
            {"max_gross_quantity", "10"},
            {"max_working_orders", working_orders}}},
          {"max_price_deviation", deviation},
          {"contracts", Json::array({contract("rb2610")})}};
}
Json submit(std::string id, const char* quantity, const char* price = "3500",
            const char* symbol = "rb2610") {
  return {{"request_id", "submit." + id},
          {"action", "submit"},
          {"order_id", id},
          {"venue", "SHFE"},
          {"symbol", symbol},
          {"side", "buy"},
          {"offset", "open"},
          {"quantity", quantity},
          {"price", price}};
}
Json authorize(std::string id = "authorize", std::string user = "000001") {
  return {{"request_id", std::move(id)}, {"action", "live_authorize"}, {"user_id", user}};
}
Json find_order(const Json& snapshot, const std::string& id) {
  for (const auto& order : snapshot.at("orders"))
    if (order.at("id") == id)
      return order;
  return nullptr;
}
template <class F> Json wait_for(const LiveSession& session, F done) {
  const auto deadline = std::chrono::steady_clock::now() + 15s;
  auto state = session.snapshot();
  while (!done(state) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(20ms);
    state = session.snapshot();
  }
  return state;
}
Json ready(LiveSession& session) {
  session.connect("secret", "auth-code");
  return wait_for(session, [](const Json& s) { return s.at("phase") == "ready"; });
}
class Live : public ::testing::Test {
protected:
  FakeExchange exchange;
  Directory directory;
  void SetUp() override { exchange.reset(); }
};
} // namespace
TEST_F(Live, OrdersPassAuthorizationAllowlistUnitsAndRiskBeforeReachingTheBroker) {
  LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, manifest());
  EXPECT_EQ(session.snapshot().at("phase"), "disconnected");
  EXPECT_THROW(session.execute(submit("early", "1")), std::invalid_argument);
  EXPECT_THROW(session.execute(authorize()), Error) << "authorizing needs a ready account";
  ASSERT_EQ(ready(session).at("phase"), "ready");
  EXPECT_THROW(session.execute(submit("unauthorized", "1")), std::invalid_argument);
  EXPECT_THROW(session.execute(authorize("other", "000002")), std::invalid_argument);
  session.execute(authorize());
  EXPECT_EQ(session.snapshot().at("authorization").at("trading_day"), "20260928");
  EXPECT_THROW(session.execute(submit("listed", "1", "3500", "rb2611")), std::invalid_argument);
  EXPECT_THROW(session.execute(submit("tick", "1", "3500.5")), std::invalid_argument);
  EXPECT_THROW(session.execute(submit("lot", "0.5")), std::invalid_argument);
  try {
    session.execute(submit("far", "1", "3600"));
    ADD_FAILURE() << "2.9% from the latest price exceeds the 2% bound";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("deviates"), std::string::npos);
  }
  try {
    session.execute(submit("large", "6"));
    ADD_FAILURE() << "risk must reject an oversized order";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("order_quantity"), std::string::npos);
  }
  EXPECT_TRUE(session.snapshot().at("orders").empty()) << "nothing rejected reaches the broker";
  session.query_costs();
  const auto rates = wait_for(session, [](const Json& s) {
    return s.at("costs").size() == 1 && s.at("costs")[0].at("state") == "ready";
  });
  ASSERT_EQ(rates.at("costs")[0].at("state"), "ready");
  EXPECT_EQ(rates.at("costs")[0].at("costs").at("margin_rate"), "0.12");

  session.execute(submit("filled", "2"));
  auto state = wait_for(session, [](const Json& s) {
    return find_order(s, "filled").is_object() &&
           find_order(s, "filled").at("status") == "filled" && !s.at("trades").empty() &&
           !s.at("positions").empty();
  });
  EXPECT_EQ(find_order(state, "filled").at("status"), "filled");
  ASSERT_EQ(state.at("positions").size(), 1U);
  EXPECT_EQ(state.at("positions")[0].at("today"), "2");
  EXPECT_EQ(state.at("trades").size(), 1U);

  session.execute(submit("resting", "3"));
  state = wait_for(session, [](const Json& s) {
    return find_order(s, "resting").is_object() &&
           find_order(s, "resting").at("status") == "accepted";
  });
  EXPECT_EQ(find_order(state, "resting").at("status"), "accepted");
  try {
    session.execute(submit("second", "1"));
    ADD_FAILURE() << "one working order is the limit";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("working_orders"), std::string::npos);
  }
  // A retried request is acknowledged without sending again; a reused ID is refused.
  const auto orders = session.snapshot().at("orders").size();
  session.execute(submit("resting", "3"));
  EXPECT_EQ(session.snapshot().at("orders").size(), orders);
  EXPECT_THROW(session.execute(submit("resting", "4")), Error);

  session.execute({{"request_id", "cancel"}, {"action", "cancel"}, {"order_id", "resting"}});
  state = wait_for(
      session, [](const Json& s) { return find_order(s, "resting").at("status") == "cancelled"; });
  EXPECT_EQ(find_order(state, "resting").at("status"), "cancelled");
  session.execute({{"request_id", "revoke"}, {"action", "live_revoke"}});
  EXPECT_TRUE(session.snapshot().at("authorization").is_null());
  EXPECT_THROW(session.execute(submit("revoked", "1")), std::invalid_argument);
  EXPECT_THROW(session.execute(authorize()), Error) << "authorization IDs are never reused";
  session.execute(authorize("authorize.again"));
  session.disconnect();
  EXPECT_TRUE(session.snapshot().at("authorization").is_null());
}
TEST_F(Live, LimitPricesStayWithinExchangeLimitsAndTheMarketReference) {
  auto wide = manifest(2, "0.1");
  wide["contracts"] = Json::array(
      {contract("rb2610"), contract("rb2611", "rb", "2026-11"), contract("zz2610", "zz")});
  LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, wide);
  ASSERT_EQ(ready(session).at("phase"), "ready");
  session.execute(authorize());
  const auto refused = [&](Json command, const char* reason) {
    try {
      session.execute(command);
      ADD_FAILURE() << command.dump();
    } catch (const std::invalid_argument& error) {
      EXPECT_NE(std::string(error.what()).find(reason), std::string::npos) << error.what();
    }
  };
  // Within 10% of 3500, but above the exchange's upper limit of 3700.
  refused(submit("limit", "1", "3750"), "price limits");
  refused(submit("quiet", "1", "3500", "zz2610"), "no current market price");
  // rb2611 has not traded: the pre-settlement price 3490 is the reference.
  session.execute(submit("settled", "1", "3500", "rb2611"));
  session.execute(submit("near", "1", "3690"));
  EXPECT_EQ(session.snapshot().at("orders").size(), 2U);
  EXPECT_EQ(session.snapshot().at("max_price_deviation"), "0.1");
}
TEST_F(Live, RecoveryAttributesRecordedOrdersAndNeverResendsThem) {
  {
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, manifest(2));
    ASSERT_EQ(ready(session).at("phase"), "ready");
    session.execute(authorize());
    session.execute(submit("resting", "3"));
    ASSERT_EQ(find_order(wait_for(session,
                                  [](const Json& s) {
                                    return find_order(s, "resting").at("status") == "accepted";
                                  }),
                         "resting")
                  .at("status"),
              "accepted");
  }
  LiveSession restored(directory.path, ASTERION_TEST_CTP_TRADER);
  EXPECT_TRUE(restored.snapshot().at("authorization").is_null());
  EXPECT_THROW(restored.execute(submit("after", "1")), std::invalid_argument);
  auto state = ready(restored);
  state = wait_for(restored, [](const Json& s) { return find_order(s, "resting").is_object(); });
  ASSERT_TRUE(find_order(state, "resting").is_object()) << "broker report keeps its order ID";
  EXPECT_TRUE(state.at("unconfirmed").empty());
  const auto orders = state.at("orders").size();
  restored.execute(submit("resting", "3"));
  EXPECT_EQ(restored.snapshot().at("orders").size(), orders) << "never resent after recovery";
  EXPECT_THROW(restored.execute(authorize()), Error);
}
TEST_F(Live, RecordedOrdersTheBrokerDoesNotReportCountAsWorkingExposure) {
  {
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    session.execute(authorize());
    session.execute(submit("lost", "3"));
  }
  // The exchange forgets the order, as if it never arrived.
  exchange.reset();
  LiveSession restored(directory.path, ASTERION_TEST_CTP_TRADER);
  const auto state = ready(restored);
  ASSERT_EQ(state.at("unconfirmed").size(), 1U);
  EXPECT_EQ(state.at("unconfirmed")[0].at("id"), "lost");
  EXPECT_TRUE(state.at("orders").empty()) << "the unconfirmed order is not resent";
  restored.execute(authorize("authorize.restored"));
  EXPECT_THROW(restored.execute(submit("next", "1")), std::invalid_argument)
      << "the unconfirmed order still occupies the working-order limit";
}
TEST_F(Live, CredentialsAreNeverWrittenAndHeadersPinTheEngine) {
  {
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    session.execute(authorize());
  }
  for (const auto& entry : fs::recursive_directory_iterator(directory.path)) {
    if (!entry.is_regular_file())
      continue;
    std::ifstream in(entry.path(), std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), {});
    EXPECT_EQ(bytes.find("secret"), std::string::npos) << entry.path();
    EXPECT_EQ(bytes.find("auth-code"), std::string::npos) << entry.path();
  }
  const auto header_file = test::journal_record(directory.path, 0);
  auto header = test::read_record(header_file);
  EXPECT_EQ(header.at("engine"), "asterion.live-futures.v4");
  EXPECT_EQ(header.at("manifest"), manifest());
  header["engine"] = "asterion.live-futures.v1";
  test::write_record(header_file, header);
  EXPECT_THROW((LiveSession{directory.path, ASTERION_TEST_CTP_TRADER}), std::invalid_argument);
}
TEST_F(Live, InvalidInputsAndMissingSdkWriteNothing) {
  auto bad = manifest();
  bad["broker"]["front"] = "http://127.0.0.1:41205";
  EXPECT_THROW((LiveSession{directory.path, ASTERION_TEST_CTP_TRADER, bad}), std::invalid_argument);
  bad = manifest();
  bad["contracts"].push_back(contract("rb2610"));
  EXPECT_THROW((LiveSession{directory.path, ASTERION_TEST_CTP_TRADER, bad}), std::invalid_argument);
  EXPECT_THROW((LiveSession{directory.path, directory.path / "missing.dylib", manifest()}), Error);
  EXPECT_EQ(test::journal_size(directory.path), 0U);
  EXPECT_FALSE(fs::exists(directory.path / "plugins"));
}
TEST(LiveProtocol, SnapshotAndCommandsRoundTrip) {
  Json snapshot{
      {"broker", manifest().at("broker")},
      {"risk", manifest().at("risk")},
      {"max_price_deviation", "0.02"},
      {"contracts", manifest().at("contracts")},
      {"phase", "ready"},
      {"error_code", 0},
      {"trading_day", "20260928"},
      {"synchronized_ms", 5},
      {"funds",
       {{"balance", "1000000"},
        {"available", "950000"},
        {"margin", "50000"},
        {"commission", "12.34"},
        {"close_profit", "0"},
        {"position_profit", "-10"}}},
      {"positions", Json::array({{{"venue", "SHFE"},
                                  {"symbol", "rb2610"},
                                  {"side", "buy"},
                                  {"today", "2"},
                                  {"yesterday", "0"}}})},
      {"orders", Json::array({{{"venue", "SHFE"},
                               {"symbol", "rb2610"},
                               {"id", "o1"},
                               {"broker_key", "1:2:3"},
                               {"exchange_order_id", "SHFE:1"},
                               {"side", "buy"},
                               {"offset", "open"},
                               {"quantity", "2"},
                               {"filled", "2"},
                               {"limit_price", "3500"},
                               {"status", "filled"},
                               {"error_code", 0}}})},
      {"trades", Json::array({{{"venue", "SHFE"},
                               {"symbol", "rb2610"},
                               {"id", "SHFE:9"},
                               {"order_id", "o1"},
                               {"side", "buy"},
                               {"offset", "open"},
                               {"quantity", "2"},
                               {"price", "3500"},
                               {"trading_day", "20260928"},
                               {"trade_time", "09:01:02"}}})},
      {"authorization", {{"trading_day", "20260928"}, {"authorized_at_ms", 7}}},
      {"unconfirmed",
       Json::array({{{"id", "o2"}, {"broker_key", "1:2:4"}, {"trading_day", "20260928"}}})},
      {"costs", Json::array({{{"venue", "SHFE"},
                              {"symbol", "rb2610"},
                              {"state", "ready"},
                              {"error_code", 0},
                              {"queried_ms", 9},
                              {"costs",
                               {{"margin_per_lot", "0"},
                                {"open_fee", "0"},
                                {"close_today_fee", "1.5"},
                                {"close_yesterday_fee", "0"},
                                {"margin_rate", "0.12"},
                                {"open_fee_rate", "0.0001"},
                                {"close_today_fee_rate", "0.0003"},
                                {"close_yesterday_fee_rate", "0.0001"}}}}})},
      {"storage_state", "ready"}};
  auto expected = snapshot;
  expected["mode"] = "live";
  EXPECT_EQ(protocol::decode_live_snapshot(protocol::encode_live_snapshot(snapshot)), expected);
  for (const auto& command : {authorize(), Json{{"request_id", "r"}, {"action", "live_revoke"}}})
    EXPECT_EQ(protocol::decode_command(protocol::encode_command(command)), command);
  auto wrong = snapshot;
  wrong["orders"][0]["status"] = "lost";
  EXPECT_THROW(protocol::encode_live_snapshot(wrong), std::invalid_argument);
  auto wire = protocol::encode_live_snapshot(snapshot);
  wire.mutable_costs(0)->set_state("querying");
  EXPECT_THROW(protocol::decode_live_snapshot(wire), std::invalid_argument)
      << "rates only accompany a ready state";
  auto spaced = manifest();
  spaced["broker"]["user_id"] = "000 001";
  EXPECT_THROW(protocol::encode_live_input(spaced), std::invalid_argument);
}

TEST_F(Live, AutomaticReconnectRequiresNewAuthorizationEvenOnTheSameTradingDay) {
  LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, manifest());
  ASSERT_EQ(ready(session).at("phase"), "ready");
  session.execute(authorize());
  ctp::SharedLibrary reconnect(ASTERION_TEST_CTP_TRADER, "asterion_fake_trader_reconnect",
                               "asterion_fake_trader_reconnect");
  reconnect.symbol<void (*)()>()();
  ASSERT_TRUE(wait_for(session, [](const Json& s) { return s.at("authorization").is_null(); })
                  .at("authorization")
                  .is_null());
  ASSERT_EQ(wait_for(session, [](const Json& s) { return s.at("phase") == "ready"; }).at("phase"),
            "ready");
  EXPECT_THROW(session.execute(submit("old.authorization", "1")), std::invalid_argument);
  EXPECT_TRUE(session.snapshot().at("orders").empty());
  session.execute(authorize("new.connection"));
  EXPECT_NO_THROW(session.execute(submit("new.authorization", "1")));
}

TEST_F(Live, TradingDayRolloverRebuildsReportsAndRatesWithoutResending) {
  LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, manifest());
  ASSERT_EQ(ready(session).at("phase"), "ready");
  session.execute(authorize());
  session.execute(submit("day0.filled", "1"));
  ASSERT_FALSE(wait_for(session, [](const Json& s) { return !s.at("positions").empty(); })
                   .at("positions")
                   .empty());
  session.execute(submit("day0.resting", "3"));
  session.query_costs();
  ASSERT_EQ(wait_for(session,
                     [](const Json& s) {
                       return !s.at("costs").empty() && s.at("costs")[0].at("state") == "ready";
                     })
                .at("costs")[0]
                .at("state"),
            "ready");
  ctp::SharedLibrary rollover(ASTERION_TEST_CTP_TRADER, "asterion_fake_trader_next_day",
                              "asterion_fake_trader_next_day");
  int index = 0;
  for (const auto* day : {"20260929", "20260930"}) {
    SCOPED_TRACE(day);
    rollover.symbol<void (*)(const char*)>()(day);
    const auto state = wait_for(session, [&](const Json& s) {
      return s.at("phase") == "ready" && s.at("trading_day") == day;
    });
    ASSERT_EQ(state.at("trading_day"), day);
    ASSERT_EQ(state.at("phase"), "ready");
    EXPECT_TRUE(state.at("authorization").is_null());
    EXPECT_TRUE(state.at("orders").empty());
    EXPECT_TRUE(state.at("trades").empty());
    EXPECT_TRUE(state.at("costs").empty());
    EXPECT_TRUE(state.at("unconfirmed").empty());
    ASSERT_EQ(state.at("positions").size(), 1U);
    EXPECT_EQ(state.at("positions")[0].at("today"), "0");
    EXPECT_EQ(state.at("positions")[0].at("yesterday"), std::to_string(index + 1));
    EXPECT_THROW(session.execute(submit("not.authorized", "1")), std::invalid_argument);
    session.execute(submit("day0.filled", "1")); // historical request: acknowledge only
    EXPECT_TRUE(session.snapshot().at("orders").empty());
    session.execute(authorize("authorize." + std::to_string(++index)));
    const auto id = "day" + std::to_string(index) + ".filled";
    session.execute(submit(id, "1"));
    const auto filled = wait_for(session, [&](const Json& s) {
      return !s.at("trades").empty() && s.at("trades").back().at("order_id") == id;
    });
    ASSERT_EQ(filled.at("trades").size(), 1U);
    EXPECT_EQ(filled.at("trades")[0].at("order_id"), id);
    EXPECT_EQ(filled.at("trades")[0].at("trading_day"), day);
  }
}
TEST_F(Live, SameDayReconnectDoesNotMistakeCachedOrdersForBrokerConfirmation) {
  LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, manifest());
  ASSERT_EQ(ready(session).at("phase"), "ready");
  session.execute(authorize());
  session.execute(submit("missing", "3"));
  ASSERT_TRUE(
      wait_for(session, [](const Json& s) { return !s.at("orders").empty(); }).at("orders").size());
  exchange.reset(); // broker no longer reports the order
  ctp::SharedLibrary reconnect(ASTERION_TEST_CTP_TRADER, "asterion_fake_trader_reconnect",
                               "asterion_fake_trader_reconnect");
  reconnect.symbol<void (*)()>()();
  const auto state = wait_for(session, [](const Json& s) {
    return s.at("phase") == "ready" && s.at("authorization").is_null();
  });
  ASSERT_EQ(state.at("phase"), "ready");
  EXPECT_TRUE(state.at("orders").empty());
  ASSERT_EQ(state.at("unconfirmed").size(), 1U);
  EXPECT_EQ(state.at("unconfirmed")[0].at("id"), "missing");
  session.execute(authorize("again"));
  EXPECT_THROW(session.execute(submit("next", "1")), std::invalid_argument);
}
