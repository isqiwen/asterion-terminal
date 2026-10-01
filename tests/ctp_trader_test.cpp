#include "ctp_support.hpp"
#include "ctp_trader.hpp"
#include <asterion/foundation/error.hpp>
#include <asterion/kernel/process/child.hpp>
#include <gtest/gtest.h>
#include <thread>
#include <future>
using namespace asterion;
using namespace std::chrono_literals;
namespace fs = std::filesystem;
namespace {
Decimal d(const char* text) {
  return Decimal::parse(text);
}
// Test control entry points exported by the SDK double.
struct FakeControl {
  ctp::SharedLibrary reset_library{ASTERION_TEST_CTP_TRADER, "asterion_fake_trader_reset",
                                   "asterion_fake_trader_reset"};
  ctp::SharedLibrary reconnect_library{ASTERION_TEST_CTP_TRADER, "asterion_fake_trader_reconnect",
                                       "asterion_fake_trader_reconnect"};
  ctp::SharedLibrary rejections_library{ASTERION_TEST_CTP_TRADER,
                                        "asterion_fake_trader_query_rejections",
                                        "asterion_fake_trader_query_rejections"};
  void reset() { reset_library.symbol<void (*)()>()(); }
  void reconnect() { reconnect_library.symbol<void (*)()>()(); }
  int query_rejections() { return rejections_library.symbol<int (*)()>()(); }
};
ctp::TraderConfiguration configuration(std::string password = "secret",
                                       std::string auth = "auth-code") {
  return {"tcp://127.0.0.1:41205", "9999", "000001", std::move(password), "client_app", auth};
}
LimitOrder order(std::string id, std::string venue, std::string symbol, Side side,
                 const char* quantity, const char* price) {
  return {std::move(id), {std::move(venue), std::move(symbol)}, side, d(quantity), d(price)};
}
template <class F> BrokerSnapshot wait_for(const ctp::Trader& trader, F done, auto timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  auto state = trader.snapshot();
  while (!done(state) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(20ms);
    state = trader.snapshot();
  }
  return state;
}
bool ready(const BrokerSnapshot& s) {
  return s.phase == "ready";
}
const BrokerOrder* find(const BrokerSnapshot& s, const std::string& id) {
  for (const auto& o : s.orders)
    if (o.order_id == id)
      return &o;
  return nullptr;
}
class CtpTrader : public ::testing::Test {
protected:
  FakeControl fake;
  fs::path flow = fs::temp_directory_path() / ("asterion-ctp-trader-" + unique_process_id());
  void SetUp() override { fake.reset(); }
  void TearDown() override {
    std::error_code e;
    fs::remove_all(flow, e);
  }
};
auto no_journal = [](const BrokerOrder&) {};
} // namespace
TEST_F(CtpTrader, LoginSynchronizesJournalsBeforeSendingAndTracksFills) {
  ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow);
  trader.start();
  EXPECT_THROW(trader.submit(order("o0", "SHFE", "rb2610", Side::buy, "1", "3500"), Offset::open,
                             trader.snapshot().connection_generation, no_journal),
               Error)
      << "not connected";
  trader.connect(configuration());
  auto state = wait_for(trader, ready, 15s);
  ASSERT_EQ(state.phase, "ready");
  EXPECT_EQ(state.trading_day, "20260928");
  ASSERT_TRUE(state.funds);
  EXPECT_EQ(state.funds->available, d("950000"));
  EXPECT_EQ(state.funds->commission, d("12.34"));
  EXPECT_TRUE(state.positions.empty());

  std::vector<BrokerOrder> journal;
  const auto first =
      trader.submit(order("o1", "SHFE", "rb2610", Side::buy, "2", "3500.125"), Offset::open,
                    trader.snapshot().connection_generation, [&](const BrokerOrder& o) {
                      EXPECT_TRUE(trader.snapshot().orders.empty())
                          << "journal runs before the order is tracked or sent";
                      journal.push_back(o);
                    });
  ASSERT_EQ(journal.size(), 1U);
  EXPECT_EQ(journal[0].broker_key, first.broker_key);
  EXPECT_EQ(journal[0].status, BrokerOrderStatus::submitted);
  state = wait_for(
      trader,
      [](const BrokerSnapshot& s) {
        return !s.positions.empty() && find(s, "o1") &&
               find(s, "o1")->status == BrokerOrderStatus::filled;
      },
      10s);
  const auto* filled = find(state, "o1");
  ASSERT_TRUE(filled);
  EXPECT_EQ(filled->status, BrokerOrderStatus::filled);
  EXPECT_EQ(filled->filled, d("2"));
  EXPECT_FALSE(filled->exchange_order_id.empty());
  ASSERT_EQ(state.trades.size(), 1U);
  EXPECT_EQ(state.trades[0].order_id, "o1");
  EXPECT_EQ(state.trades[0].price, d("3500.125"));
  ASSERT_EQ(state.positions.size(), 1U);
  EXPECT_EQ(state.positions[0].side, Side::buy);
  EXPECT_EQ(state.positions[0].today, d("2"));

  // SHFE needs an explicit close bucket; a journal failure sends nothing.
  EXPECT_THROW(trader.submit(order("o2", "SHFE", "rb2610", Side::sell, "1", "3500"), Offset::close,
                             trader.snapshot().connection_generation, no_journal),
               std::invalid_argument);
  EXPECT_THROW(trader.submit(order("o3", "SHFE", "rb2610", Side::sell, "1.5", "3500"),
                             Offset::close_today, trader.snapshot().connection_generation,
                             no_journal),
               std::invalid_argument);
  EXPECT_THROW(trader.submit(order("o4", "SHFE", "rb2610", Side::sell, "1", "3500"),
                             Offset::close_today, trader.snapshot().connection_generation,
                             [](const BrokerOrder&) { throw std::runtime_error("disk full"); }),
               std::runtime_error);
  EXPECT_FALSE(find(trader.snapshot(), "o4"));
  EXPECT_THROW(trader.submit(order("o1", "SHFE", "rb2610", Side::buy, "1", "3500"), Offset::open,
                             trader.snapshot().connection_generation, no_journal),
               Error)
      << "duplicate order ID";
  EXPECT_EQ(fake.query_rejections(), 0) << "queries respect the CTP flow limit";
  trader.disconnect();
  EXPECT_EQ(trader.snapshot().phase, "disconnected");
}
TEST_F(CtpTrader, RejectionsAndCancellation) {
  ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow);
  trader.start();
  trader.connect(configuration());
  ASSERT_EQ(wait_for(trader, ready, 15s).phase, "ready");
  trader.submit(order("bad", "DCE", "zz2609", Side::buy, "1", "100"), Offset::open,
                trader.snapshot().connection_generation, no_journal);
  auto state = wait_for(
      trader,
      [](const BrokerSnapshot& s) {
        return find(s, "bad") && find(s, "bad")->status == BrokerOrderStatus::rejected;
      },
      5s);
  EXPECT_EQ(find(state, "bad")->error_code, 16);

  trader.submit(order("rest", "DCE", "m2609", Side::buy, "5", "2800"), Offset::open,
                trader.snapshot().connection_generation, no_journal);
  state = wait_for(
      trader,
      [](const BrokerSnapshot& s) {
        return find(s, "rest") && find(s, "rest")->status == BrokerOrderStatus::accepted;
      },
      5s);
  ASSERT_EQ(find(state, "rest")->status, BrokerOrderStatus::accepted);
  trader.cancel("rest");
  state = wait_for(
      trader,
      [](const BrokerSnapshot& s) {
        return find(s, "rest")->status == BrokerOrderStatus::cancelled;
      },
      5s);
  EXPECT_EQ(find(state, "rest")->status, BrokerOrderStatus::cancelled);
  EXPECT_THROW(trader.cancel("rest"), Error);
  EXPECT_THROW(trader.cancel("missing"), Error);
  // DCE closes yesterday's positions first, so the generic close is allowed.
  EXPECT_NO_THROW(trader.submit(order("close", "DCE", "m2609", Side::sell, "1", "2800"),
                                Offset::close, trader.snapshot().connection_generation,
                                no_journal));
}
TEST_F(CtpTrader, ReconnectAndRestartKeepOrdersAttributed) {
  std::string key;
  {
    ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow);
    trader.start();
    trader.connect(configuration());
    ASSERT_EQ(wait_for(trader, ready, 15s).phase, "ready");
    key = trader
              .submit(order("resting", "SHFE", "rb2610", Side::buy, "4", "3400"), Offset::open,
                      trader.snapshot().connection_generation, no_journal)
              .broker_key;
    ASSERT_EQ(wait_for(
                  trader,
                  [](const BrokerSnapshot& s) {
                    return find(s, "resting")->status == BrokerOrderStatus::accepted;
                  },
                  5s)
                  .orders.size(),
              1U);
    const auto before = trader.snapshot().sequence;
    fake.reconnect();
    auto state = wait_for(
        trader, [&](const BrokerSnapshot& s) { return s.sequence > before + 2 && ready(s); }, 15s);
    ASSERT_EQ(state.phase, "ready");
    ASSERT_EQ(state.orders.size(), 1U) << "the queried order merges with the tracked one";
    EXPECT_EQ(find(state, "resting")->broker_key, key);
    // A new session allocates keys that cannot collide with the old one.
    const auto next =
        trader.submit(order("after", "SHFE", "rb2610", Side::buy, "4", "3401"), Offset::open,
                      trader.snapshot().connection_generation, no_journal);
    EXPECT_NE(next.broker_key.substr(0, next.broker_key.rfind(':')), key.substr(0, key.rfind(':')));
    trader.cancel("resting");
    EXPECT_EQ(wait_for(
                  trader,
                  [](const BrokerSnapshot& s) {
                    return find(s, "resting")->status == BrokerOrderStatus::cancelled;
                  },
                  5s)
                  .phase,
              "ready");
  }
  // A restarted process attributes orders from its journal; others stay
  // unattributed but visible.
  ctp::Trader restarted(ASTERION_TEST_CTP_TRADER, flow);
  restarted.start();
  restarted.connect(configuration(), {{key, "resting"}});
  const auto state = wait_for(restarted, ready, 15s);
  ASSERT_EQ(state.phase, "ready");
  ASSERT_EQ(state.orders.size(), 2U);
  EXPECT_EQ(find(state, "resting")->status, BrokerOrderStatus::cancelled);
  EXPECT_TRUE(find(state, ""));
  EXPECT_THROW(restarted.cancel(""), Error);
}
TEST_F(CtpTrader, CredentialFailuresStopBeforeTrading) {
  ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow);
  trader.start();
  EXPECT_THROW(trader.connect({"http://x", "9999", "1", "p", "a", "c"}), std::invalid_argument);
  EXPECT_THROW(trader.connect(configuration("secret", "")), std::invalid_argument);
  trader.connect(configuration("secret", "bad-auth"));
  auto state = wait_for(trader, [](const BrokerSnapshot& s) { return s.phase == "error"; }, 5s);
  EXPECT_EQ(state.error_code, 63);
  trader.connect(configuration("bad"));
  state = wait_for(trader, [](const BrokerSnapshot& s) { return s.phase == "error"; }, 5s);
  EXPECT_EQ(state.error_code, 3);
  EXPECT_THROW(trader.submit(order("x", "SHFE", "rb2610", Side::buy, "1", "3500"), Offset::open,
                             trader.snapshot().connection_generation, no_journal),
               Error);
  EXPECT_THROW((ctp::Trader(fs::path("missing-library"), flow)), Error);
}
TEST_F(CtpTrader, AccountRatesFillCostsWithoutAffectingTheSession) {
  ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow);
  trader.start();
  EXPECT_THROW(trader.query_costs({{{"SHFE", "rb2610"}, "rb"}}), Error) << "not connected";
  trader.connect(configuration());
  ASSERT_EQ(wait_for(trader, ready, 15s).phase, "ready");
  trader.query_costs({{{"SHFE", "rb2610"}, "rb"}, {{"SHFE", "zz2610"}, "zz"}});
  const auto state = wait_for(
      trader,
      [](const BrokerSnapshot& s) {
        return s.costs.size() == 2 && std::ranges::none_of(s.costs, [](const BrokerCosts& c) {
                 return c.state == "querying";
               });
      },
      15s);
  ASSERT_EQ(state.costs.size(), 2U);
  EXPECT_EQ(state.phase, "ready") << "a missing rate does not end the session";
  const auto& rb = state.costs[0];
  ASSERT_EQ(rb.state, "ready");
  ASSERT_TRUE(rb.costs);
  // Margin takes the higher short rate; commission comes from the product row.
  EXPECT_EQ(rb.costs->margin_rate, d("0.12"));
  EXPECT_EQ(rb.costs->margin_per_lot, d("0"));
  EXPECT_EQ(rb.costs->open_fee_rate, d("0.0001"));
  EXPECT_EQ(rb.costs->close_yesterday_fee_rate, d("0.0001"));
  EXPECT_EQ(rb.costs->close_today_fee_rate, d("0.0003"));
  EXPECT_EQ(rb.costs->close_today_fee, d("1.5"));
  EXPECT_GT(rb.queried_ms, 0);
  EXPECT_EQ(state.costs[1].state, "unavailable");
  EXPECT_FALSE(state.costs[1].costs);
  EXPECT_EQ(fake.query_rejections(), 0) << "rate queries respect the CTP flow limit";
}

TEST_F(CtpTrader, QuoteFlowControlRetriesWithoutEndingTheConnection) {
  ctp::SharedLibrary reject(ASTERION_TEST_CTP_TRADER, "asterion_fake_trader_reject_quotes",
                            "asterion_fake_trader_reject_quotes");
  ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow);
  trader.start();
  trader.connect(configuration());
  ASSERT_EQ(wait_for(trader, ready, 15s).phase, "ready");
  for (const int code : {-2, -3}) {
    reject.symbol<void (*)(int, int)>()(code, 1);
    auto quote = trader.quote({"SHFE", "rb2610"});
    ASSERT_TRUE(quote);
    EXPECT_EQ(quote->last, d("3500"));
    EXPECT_EQ(trader.snapshot().phase, "ready");
  }
  EXPECT_EQ(fake.query_rejections(), 2);
  auto pending = trader.submit(order("after.throttle", "SHFE", "rb2610", Side::buy, "4", "3400"),
                               Offset::open, trader.snapshot().connection_generation, no_journal);
  EXPECT_NE(pending.status, BrokerOrderStatus::rejected);
  EXPECT_NO_THROW(trader.cancel("after.throttle"));
}
TEST_F(CtpTrader, ReconnectFencesAuthorizationBeforeAndDuringJournaling) {
  ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow);
  trader.start();
  trader.connect(configuration());
  auto state = wait_for(trader, ready, 15s);
  ASSERT_EQ(state.phase, "ready");
  const auto reconnect = [&] {
    const auto generation = trader.snapshot().connection_generation;
    fake.reconnect();
    return wait_for(
        trader,
        [&](const BrokerSnapshot& s) { return ready(s) && s.connection_generation != generation; },
        15s);
  };
  ASSERT_NE(reconnect().connection_generation, state.connection_generation);
  bool journaled = false;
  EXPECT_THROW(trader.submit(order("stale", "SHFE", "rb2610", Side::buy, "4", "3400"), Offset::open,
                             state.connection_generation,
                             [&](const BrokerOrder&) { journaled = true; }),
               Error);
  EXPECT_FALSE(journaled);
  state = trader.snapshot();
  auto result = trader.submit(order("during.journal", "SHFE", "rb2610", Side::buy, "4", "3400"),
                              Offset::open, state.connection_generation,
                              [&](const BrokerOrder&) { EXPECT_EQ(reconnect().phase, "ready"); });
  EXPECT_EQ(result.status, BrokerOrderStatus::rejected);
  EXPECT_EQ(result.error_code, -1003);
  trader.disconnect();
  trader.connect(configuration());
  const auto synced = wait_for(trader, ready, 15s);
  EXPECT_TRUE(synced.trades.empty());
  EXPECT_TRUE(synced.orders.empty()) << "nothing was sent in the newer connection";
}
TEST_F(CtpTrader, DisconnectCompletesAThrottledQuoteWithoutRetryingItAfterReconnect) {
  ctp::SharedLibrary reject(ASTERION_TEST_CTP_TRADER, "asterion_fake_trader_reject_quotes",
                            "asterion_fake_trader_reject_quotes");
  ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow);
  trader.start();
  trader.connect(configuration());
  ASSERT_EQ(wait_for(trader, ready, 15s).phase, "ready");
  reject.symbol<void (*)(int, int)>()(-3, 100);
  auto query = std::async(std::launch::async, [&] { return trader.quote({"SHFE", "rb2610"}); });
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!fake.query_rejections() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(20ms);
  ASSERT_GT(fake.query_rejections(), 0);
  trader.disconnect();
  ASSERT_EQ(query.wait_for(2s), std::future_status::ready);
  EXPECT_FALSE(query.get());
  reject.symbol<void (*)(int, int)>()(0, 0);
  trader.connect(configuration());
  ASSERT_EQ(wait_for(trader, ready, 15s).phase, "ready");
  EXPECT_TRUE(trader.quote({"SHFE", "rb2610"}));
}

TEST_F(CtpTrader, TimedOutQuoteRetiresRetriesAndLeavesTheWorkerAvailable) {
  ctp::SharedLibrary reject(ASTERION_TEST_CTP_TRADER, "asterion_fake_trader_reject_quotes",
                            "asterion_fake_trader_reject_quotes");
  ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow);
  trader.start();
  trader.connect(configuration());
  ASSERT_EQ(wait_for(trader, ready, 15s).phase, "ready");
  reject.symbol<void (*)(int, int)>()(-3, 100);
  EXPECT_FALSE(trader.quote({"SHFE", "rb2610"}));
  EXPECT_EQ(trader.snapshot().phase, "ready");
  reject.symbol<void (*)(int, int)>()(0, 0);
  EXPECT_TRUE(trader.quote({"SHFE", "rb2610"}));
  EXPECT_EQ(trader.snapshot().phase, "ready");
}

TEST_F(CtpTrader, LateQueryRepliesCannotReplaceReconnectedAccountState) {
  ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow);
  trader.start();
  trader.connect(configuration());
  const auto initial = wait_for(trader, ready, 15s);
  ASSERT_EQ(initial.phase, "ready");
  fake.reconnect();
  const auto synced = wait_for(
      trader,
      [&](const BrokerSnapshot& s) {
        return ready(s) && s.connection_generation != initial.connection_generation;
      },
      15s);
  ASSERT_EQ(synced.phase, "ready");
  ctp::SharedLibrary inject(ASTERION_TEST_CTP_TRADER, "asterion_fake_trader_stale_queries",
                            "asterion_fake_trader_stale_queries");
  ctp::SharedLibrary batches(ASTERION_TEST_CTP_TRADER, "asterion_fake_trader_stale_batches",
                             "asterion_fake_trader_stale_batches");
  const auto before = batches.symbol<int (*)()>()();
  inject.symbol<void (*)()>()();
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (batches.symbol<int (*)()>()() == before && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(10ms);
  ASSERT_GT(batches.symbol<int (*)()>()(), before);
  const auto after = trader.snapshot();
  EXPECT_EQ(after.sequence, synced.sequence);
  EXPECT_EQ(after.synchronized_ms, synced.synchronized_ms);
  ASSERT_TRUE(after.funds);
  EXPECT_EQ(after.funds->balance, synced.funds->balance);
  EXPECT_TRUE(after.positions.empty());
  EXPECT_TRUE(after.costs.empty());
}
