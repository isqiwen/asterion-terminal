#include "ctp_support.hpp"
#include "ctp_trader.hpp"
#include "ctp_reconciliation.hpp"
#include <cstring>
#include <asterion/foundation/error.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/environment.hpp>
#include <asterion/kernel/service_host.hpp>
#include <gtest/gtest.h>
#include <thread>
#include <future>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
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
  template <class F, class... Args> auto call(const char* name, Args... args) {
    ctp::SharedLibrary control(ASTERION_TEST_CTP_TRADER, name, name);
    return control.symbol<F>()(args...);
  }
};
ctp::TraderConfiguration configuration(std::string password = "secret",
                                       std::string auth = "auth-code") {
  return {"tcp://127.0.0.1:41205", "9999", "000001", std::move(password), "client_app", auth};
}
LimitOrder order(std::string id, std::string venue, std::string symbol, Side side,
                 const char* quantity, const char* price) {
  return {std::move(id), {std::move(venue), std::move(symbol)}, side, d(quantity), d(price)};
}
template <class F> BrokerSnapshot wait_for(ctp::Trader& trader, F done, auto timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  trader.poll();
  auto state = trader.snapshot();
  while (!done(state) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(20ms);
    trader.poll();
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
  BrokerSendGate gate;
  fs::path flow = fs::temp_directory_path() / ("asterion-ctp-trader-" + unique_process_id());
  void SetUp() override { fake.reset(); }
  void TearDown() override {
    std::error_code e;
    fs::remove_all(flow, e);
  }
};
auto dispatch_deadline() {
  return std::chrono::steady_clock::now() + 10s;
}
std::future<BrokerDispatchResult>
dispatch_with_permission(ctp::Trader& trader, BrokerSendGate& gate,
                         std::unique_ptr<PreparedBrokerOrder> prepared) {
  static std::uint64_t sequence = 1;
  auto permit = gate.issue(gate.revision(), prepared->order().order_id);
  return trader.dispatch(std::move(prepared), std::move(permit), ++sequence);
}
template <class T> T await(ctp::Trader& trader, std::future<T> result) {
  const auto deadline = std::chrono::steady_clock::now() + 15s;
  while (result.wait_for(0s) != std::future_status::ready) {
    trader.poll();
    if (std::chrono::steady_clock::now() >= deadline)
      throw std::runtime_error("test SDK result did not complete");
    std::this_thread::sleep_for(10ms);
  }
  trader.poll();
  return result.get();
}
void cancel(ctp::Trader& trader, const std::string& order_id) {
  const auto result = await(trader, trader.cancel(order_id));
  EXPECT_TRUE(result.invoked);
  EXPECT_EQ(result.code, 0);
}
BrokerOrder send(ctp::Trader& trader, BrokerSendGate& gate, const LimitOrder& order, Offset offset,
                 std::uint64_t generation, std::uint64_t exposure,
                 std::chrono::steady_clock::time_point deadline) {
  auto prepared = trader.prepare(order, offset, generation, exposure, deadline);
  const auto pending = prepared->order();
  auto result = dispatch_with_permission(trader, gate, std::move(prepared));
  EXPECT_TRUE(await(trader, std::move(result)).invoked);
  trader.poll();
  const auto state = trader.snapshot();
  if (const auto* reported = find(state, pending.order_id))
    return *reported;
  return pending;
}
struct Socket {
  int fd;
  explicit Socket(int value) : fd(value) {}
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  ~Socket() {
    if (fd >= 0)
      ::close(fd);
  }
};
struct VendorFlow {
  fs::path path = fs::temp_directory_path() / ("asterion-vendor-trader-" + unique_process_id());
  ~VendorFlow() {
    std::error_code ignored;
    fs::remove_all(path, ignored);
  }
};
} // namespace
TEST(CtpVendor, TraderLoopbackLifecycleUsesThePackagedAbi) {
  // Match trading/market service initialization, including signal policy.
  service::install_stop_signals();
#ifdef __APPLE__
  const char* filename = "ctp-trader.dylib";
#else
  const char* filename = "ctp-trader.so";
#endif
  const auto library = environment_path("ASTERION_VENDOR_CTP_TRADER_LIBRARY")
                           .value_or(current_executable().parent_path() / filename);
  ASSERT_TRUE(fs::is_regular_file(library));
  ctp::SharedLibrary version(library, "", "_ZN19CThostFtdcTraderApi13GetApiVersionEv");
  const auto api_version = version.symbol<const char* (*)()>();
  ASSERT_NE(api_version, nullptr);
  ASSERT_NE(api_version(), nullptr);
  ASSERT_NE(std::string(api_version()).find("6.7.7"), std::string::npos);

  // Only this owned loopback listener is reachable. No CTP protocol replies,
  // authentication or orders are supplied by the fixture.
  Socket listener(::socket(AF_INET, SOCK_STREAM, 0));
  ASSERT_GE(listener.fd, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_EQ(::bind(listener.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
  socklen_t length = sizeof(address);
  ASSERT_EQ(::getsockname(listener.fd, reinterpret_cast<sockaddr*>(&address), &length), 0);
  ASSERT_EQ(::listen(listener.fd, 2), 0);
  const auto front = "tcp://127.0.0.1:" + std::to_string(ntohs(address.sin_port));
  VendorFlow flow;
  {
    BrokerSendGate gate;
    ctp::Trader trader(library, flow.path, gate, [] {});
    trader.start();
    for (int attempt = 0; attempt < 2; ++attempt) {
      SCOPED_TRACE(attempt);
      trader.connect({front, "test", "fixture", "loopback-only", "test_app", "test_auth"});
      pollfd incoming{listener.fd, POLLIN, 0};
      const auto deadline = std::chrono::steady_clock::now() + 5s;
      while (::poll(&incoming, 1, 0) == 0 && std::chrono::steady_clock::now() < deadline) {
        trader.poll();
        std::this_thread::sleep_for(10ms);
      }
      ASSERT_EQ(::poll(&incoming, 1, 0), 1);
      ASSERT_NE(incoming.revents & POLLIN, 0);
      Socket peer(::accept(listener.fd, nullptr, nullptr));
      ASSERT_GE(peer.fd, 0);
      EXPECT_NE(trader.snapshot().phase, "ready");
      EXPECT_TRUE(trader.snapshot().orders.empty());
      trader.disconnect();
      EXPECT_EQ(trader.snapshot().phase, "disconnected");
    }
  } // SDK Release and its worker complete before unloading the real library.
}
TEST(BrokerSendGate, PermissionIsBoundToOneOrderAndOneInvalidationRevision) {
  BrokerSendGate gate;
  auto old = gate.issue(gate.revision(), "old");
  EXPECT_THROW(gate.issue(gate.revision(), "parallel"), Error);
  gate.invalidate();
  auto current = gate.issue(gate.revision(), "current");
  old = BrokerSendPermit{};
  EXPECT_TRUE(current.consume("current")) << "retiring an old grant cannot close a new one";
  EXPECT_FALSE(current.consume("current"));
  auto wrong = gate.issue(gate.revision(), "expected");
  EXPECT_FALSE(wrong.consume("different"));
  const auto previous = gate.revision();
  gate.invalidate();
  EXPECT_THROW(gate.issue(previous, "stale"), Error);
  gate.pending();
  const auto pending = gate.revision();
  EXPECT_THROW(gate.issue(pending, "unapplied"), Error);
  gate.invalidate();
  gate.acknowledge(pending);
  EXPECT_THROW(gate.issue(gate.revision(), "old-acknowledgement"), Error);
  gate.acknowledge(gate.revision());
  EXPECT_TRUE(gate.issue(gate.revision(), "applied").consume("applied"));
}
TEST_F(CtpTrader, QueuedReportsInvalidatePermissionBeforeTheOwnerAppliesThem) {
  ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {});
  trader.connect(configuration());
  const auto initial = wait_for(trader, ready, 15s);
  ASSERT_EQ(initial.phase, "ready");
  send(trader, gate, order("working", "SHFE", "rb2610", Side::buy, "3", "3500"), Offset::open,
       initial.connection_generation, initial.exposure_revision, dispatch_deadline());
  const auto before = wait_for(
      trader,
      [](const BrokerSnapshot& state) {
        const auto* entry = find(state, "working");
        return entry && entry->status == BrokerOrderStatus::accepted;
      },
      2s);
  ASSERT_NE(find(before, "working"), nullptr);
  const auto revision = gate.revision();
  auto permit = gate.issue(revision, "next");
  fake.call<void (*)(int, int)>("asterion_fake_trader_replay_order", 0, 0);
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (gate.revision() == revision && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  ASSERT_GT(gate.revision(), revision);
  EXPECT_EQ(trader.snapshot().sequence, before.sequence);
  EXPECT_FALSE(permit.consume("next"));
  EXPECT_THROW(gate.issue(gate.revision(), "unapplied"), Error);
  EXPECT_THROW(trader.disconnect_checked(before.connection_generation, before.exposure_revision),
               Error);
  EXPECT_EQ(trader.snapshot().phase, "ready");
  trader.poll();
  EXPECT_TRUE(gate.issue(gate.revision(), "applied").consume("applied"));
  const auto applied = trader.snapshot();
  EXPECT_NO_THROW(
      trader.disconnect_checked(applied.connection_generation, applied.exposure_revision));
}
TEST_F(CtpTrader, CallbackOverflowRequiresReconciliationAndStillAllowsCancellation) {
  ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {});
  trader.connect(configuration());
  const auto initial = wait_for(trader, ready, 15s);
  ASSERT_EQ(initial.phase, "ready");
  send(trader, gate, order("working", "SHFE", "rb2610", Side::buy, "3", "3500"), Offset::open,
       initial.connection_generation, initial.exposure_revision, dispatch_deadline());
  ASSERT_NE(find(wait_for(
                     trader,
                     [](const BrokerSnapshot& state) {
                       const auto* entry = find(state, "working");
                       return entry && entry->status == BrokerOrderStatus::accepted;
                     },
                     2s),
                 "working"),
            nullptr);
  const auto revision = gate.revision();
  fake.call<void (*)(int)>("asterion_fake_trader_report_burst", 1100);
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (gate.revision() < revision + 1100 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  ASSERT_GE(gate.revision(), revision + 1100);
  EXPECT_THROW(gate.issue(gate.revision(), "unapplied"), Error);
  trader.poll();
  EXPECT_EQ(trader.snapshot().phase, "error");
  EXPECT_EQ(trader.snapshot().error_code, -1008);
  EXPECT_FALSE(trader.snapshot().positions_reconciled);
  EXPECT_THROW(gate.issue(gate.revision(), "lost-report"), Error);
  cancel(trader, "working");
  const auto cancelled = wait_for(
      trader,
      [](const BrokerSnapshot& state) {
        const auto* entry = find(state, "working");
        return entry && entry->status == BrokerOrderStatus::cancelled;
      },
      2s);
  ASSERT_NE(find(cancelled, "working"), nullptr);
  EXPECT_EQ(find(cancelled, "working")->status, BrokerOrderStatus::cancelled);
  EXPECT_THROW(gate.issue(gate.revision(), "still-unreconciled"), Error);
  trader.connect(configuration());
  ASSERT_EQ(wait_for(trader, ready, 15s).phase, "ready");
  EXPECT_TRUE(gate.issue(gate.revision(), "reconciled").consume("reconciled"));
}
TEST_F(CtpTrader, DisconnectRetainsAlreadyReceivedOrderAndTradeFacts) {
  ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {});
  trader.connect(configuration());
  const auto initial = wait_for(trader, ready, 15s);
  ASSERT_EQ(initial.phase, "ready");
  send(trader, gate, order("working", "SHFE", "rb2610", Side::buy, "3", "3500"), Offset::open,
       initial.connection_generation, initial.exposure_revision, dispatch_deadline());
  const auto accepted = wait_for(
      trader,
      [](const BrokerSnapshot& state) {
        const auto* entry = find(state, "working");
        return entry && entry->status == BrokerOrderStatus::accepted;
      },
      2s);
  ASSERT_NE(find(accepted, "working"), nullptr);
  const auto revision = gate.revision();
  fake.call<void (*)(int, int)>("asterion_fake_trader_fill", 0, 1);
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (gate.revision() < revision + 2 && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(1ms);
  ASSERT_GE(gate.revision(), revision + 2);
  EXPECT_TRUE(trader.snapshot().trades.empty()) << "only the owner may apply the queued facts";
  trader.disconnect();
  trader.poll();
  const auto closed = trader.snapshot();
  EXPECT_EQ(closed.phase, "disconnected");
  ASSERT_NE(find(closed, "working"), nullptr);
  EXPECT_EQ(find(closed, "working")->filled, d("1"));
  ASSERT_EQ(closed.trades.size(), 1U);
  EXPECT_EQ(closed.trades.front().quantity, d("1"));
}
TEST_F(CtpTrader, RevokedPermissionNeverInvokesTheSdkOrInventsABrokerOrder) {
  ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {});
  trader.connect(configuration());
  const auto state = wait_for(trader, ready, 15s);
  ASSERT_EQ(state.phase, "ready");
  auto prepared =
      trader.prepare(order("revoked", "SHFE", "rb2610", Side::buy, "1", "3500"), Offset::open,
                     state.connection_generation, state.exposure_revision, dispatch_deadline());
  auto permit = gate.issue(gate.revision(), "revoked");
  const auto effects = fake.call<int (*)()>("asterion_fake_catalog_side_effects");
  gate.invalidate();
  const auto result = await(trader, trader.dispatch(std::move(prepared), std::move(permit), 1));
  EXPECT_FALSE(result.invoked);
  EXPECT_EQ(result.code, -1007);
  EXPECT_TRUE(trader.snapshot().orders.empty());
  EXPECT_EQ(fake.call<int (*)()>("asterion_fake_catalog_side_effects"), effects);
}
TEST_F(CtpTrader, SdkLifecycleHasOneOwnerAndBlockedReleaseDoesNotBlockControl) {
  {
    ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {});
    // Unblock Release before Trader destruction even if an assertion fails.
    struct ReleaseGuard {
      FakeControl& fake;
      ~ReleaseGuard() { fake.call<void (*)(int)>("asterion_fake_trader_hold_release", 0); }
    } release{fake};
    trader.connect(configuration());
    const auto initial = wait_for(trader, ready, 15s);
    ASSERT_EQ(initial.phase, "ready");
    auto pending =
        send(trader, gate, order("resting", "SHFE", "rb2610", Side::buy, "3", "3500"), Offset::open,
             initial.connection_generation, initial.exposure_revision, dispatch_deadline());
    const auto accepted = wait_for(
        trader,
        [](const BrokerSnapshot& state) {
          const auto* entry = find(state, "resting");
          return entry && entry->status == BrokerOrderStatus::accepted;
        },
        5s);
    ASSERT_NE(find(accepted, "resting"), nullptr);
    ASSERT_EQ(find(accepted, "resting")->status, BrokerOrderStatus::accepted);
    cancel(trader, pending.order_id);
    ASSERT_TRUE(await(trader, trader.quote({"SHFE", "rb2610"})));
    fake.call<void (*)(int)>("asterion_fake_trader_hold_release", 1);
    trader.disconnect();
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!fake.call<int (*)()>("asterion_fake_trader_sdk_releases") &&
           std::chrono::steady_clock::now() < deadline) {
      trader.poll();
      std::this_thread::sleep_for(10ms);
    }
    ASSERT_EQ(fake.call<int (*)()>("asterion_fake_trader_sdk_releases"), 1);
    EXPECT_EQ(trader.snapshot().phase, "disconnected");

    auto replacement = std::async(std::launch::async, [&] { trader.connect(configuration()); });
    const auto responsive = replacement.wait_for(1s);
    if (responsive != std::future_status::ready)
      fake.call<void (*)(int)>("asterion_fake_trader_hold_release", 0);
    replacement.get();
    ASSERT_EQ(responsive, std::future_status::ready);
    EXPECT_EQ(trader.snapshot().phase, "connecting");
    EXPECT_EQ(fake.call<int (*)()>("asterion_fake_trader_sdk_creations"), 1);
    // A disconnect cancels the waiting replacement before it creates an SDK.
    trader.disconnect();
    EXPECT_EQ(trader.snapshot().phase, "disconnected");
    fake.call<void (*)(int)>("asterion_fake_trader_hold_release", 0);
    trader.connect(configuration());
    const auto restored = wait_for(trader, ready, 15s);
    ASSERT_EQ(restored.phase, "ready");
    EXPECT_NE(restored.connection_generation, initial.connection_generation);
    EXPECT_EQ(fake.call<int (*)()>("asterion_fake_trader_sdk_creations"), 2);
  }
  EXPECT_EQ(fake.call<int (*)()>("asterion_fake_trader_sdk_releases"), 2);
  EXPECT_EQ(fake.call<int (*)()>("asterion_fake_trader_sdk_thread_mismatches"), 0);
}
TEST_F(CtpTrader, PartialQuoteResponsesRequireSuccessfulCompletionAndCurrentIdentity) {
  ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {});
  trader.start();
  trader.connect(configuration());
  ASSERT_EQ(wait_for(trader, ready, 15s).phase, "ready");
  for (const int mode : {1, 3, 4}) {
    fake.call<void (*)(int)>("asterion_fake_trader_quote_mode", mode);
    EXPECT_FALSE(await(trader, trader.quote({"SHFE", "rb2610"}))) << "mode " << mode;
  }
  fake.call<void (*)(int)>("asterion_fake_trader_quote_mode", 5);
  const auto started = std::chrono::steady_clock::now();
  const auto quote = await(trader, trader.quote({"SHFE", "rb2610"}));
  ASSERT_TRUE(quote);
  EXPECT_EQ(quote->last, d("3500"));
  EXPECT_EQ(quote->trading_day, trader.snapshot().trading_day);
  EXPECT_EQ(quote->connection_generation, trader.snapshot().connection_generation);
  EXPECT_GE(quote->completed_at, started);
  EXPECT_LE(quote->completed_at, std::chrono::steady_clock::now());
  fake.call<void (*)(int)>("asterion_fake_trader_quote_mode", 2);
  EXPECT_FALSE(await(trader, trader.quote({"SHFE", "rb2610"})));
}
TEST_F(CtpTrader, DispatchReturnsWithoutWaitingAndLateSdkResultCannotOverwriteBrokerFacts) {
  ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {});
  struct ReturnGuard {
    FakeControl& fake;
    ~ReturnGuard() {
      fake.call<void (*)(int, int)>("asterion_fake_trader_hold_insert_return", 0, 0);
    }
  } release{fake};
  trader.connect(configuration());
  const auto state = wait_for(trader, ready, 15s);
  ASSERT_EQ(state.phase, "ready");
  auto prepared =
      trader.prepare(order("early.fill", "SHFE", "rb2610", Side::buy, "1", "3500"), Offset::open,
                     state.connection_generation, state.exposure_revision, dispatch_deadline());
  fake.call<void (*)(int, int)>("asterion_fake_trader_hold_insert_return", 1, -7);
  auto completion = dispatch_with_permission(trader, gate, std::move(prepared));
  const auto filled = wait_for(
      trader,
      [](const BrokerSnapshot& current) {
        const auto* order = find(current, "early.fill");
        return order && order->status == BrokerOrderStatus::filled;
      },
      2s);
  ASSERT_NE(find(filled, "early.fill"), nullptr);
  ASSERT_EQ(find(filled, "early.fill")->status, BrokerOrderStatus::filled);
  EXPECT_EQ(completion.wait_for(0s), std::future_status::timeout);
  EXPECT_THROW(trader.prepare(order("queued", "SHFE", "rb2610", Side::buy, "1", "3500"),
                              Offset::open, filled.connection_generation, filled.exposure_revision,
                              dispatch_deadline()),
               Error);
  fake.call<void (*)(int, int)>("asterion_fake_trader_hold_insert_return", 0, -7);
  const auto result = await(trader, std::move(completion));
  EXPECT_TRUE(result.invoked);
  EXPECT_EQ(result.code, -7);
  const auto current = trader.snapshot();
  ASSERT_NE(find(current, "early.fill"), nullptr);
  EXPECT_EQ(find(current, "early.fill")->status, BrokerOrderStatus::filled);
  EXPECT_EQ(find(current, "early.fill")->filled, d("1"));
}
TEST_F(CtpTrader, ExpiredSubmissionNeverDispatchesEvenIfJournalingCompletesLater) {
  ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {});
  trader.start();
  trader.connect(configuration());
  auto state = wait_for(trader, ready, 15s);
  ASSERT_TRUE(state.positions_reconciled);
  EXPECT_THROW(trader.prepare(order("expired", "SHFE", "rb2610", Side::buy, "4", "3500"),
                              Offset::open, state.connection_generation, state.exposure_revision,
                              std::chrono::steady_clock::now()),
               Error);
  const auto effects = fake.call<int (*)()>("asterion_fake_catalog_side_effects");
  const auto deadline = std::chrono::steady_clock::now() + 100ms;
  auto prepared =
      trader.prepare(order("slow.disk", "SHFE", "rb2610", Side::buy, "4", "3500"), Offset::open,
                     state.connection_generation, state.exposure_revision, deadline);
  std::this_thread::sleep_until(deadline + 20ms);
  const auto result = await(trader, dispatch_with_permission(trader, gate, std::move(prepared)));
  EXPECT_FALSE(result.invoked);
  EXPECT_EQ(result.code, -1005);
  EXPECT_EQ(fake.call<int (*)()>("asterion_fake_catalog_side_effects"), effects);
  EXPECT_TRUE(trader.snapshot().orders.empty());
}
TEST_F(CtpTrader, FilledOrderWaitsForTradeReportsAndLateOrdersCannotReduceExposure) {
  ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {});
  trader.start();
  trader.connect(configuration());
  ASSERT_TRUE(wait_for(trader, ready, 15s).positions_reconciled);
  fake.call<void (*)(int)>("asterion_fake_trader_hold_trades", 1);
  send(trader, gate, order("filled", "SHFE", "rb2610", Side::buy, "2", "3500"), Offset::open,
       trader.snapshot().connection_generation, trader.snapshot().exposure_revision,
       dispatch_deadline());
  auto state = wait_for(
      trader,
      [](const BrokerSnapshot& s) {
        return find(s, "filled") && find(s, "filled")->status == BrokerOrderStatus::filled &&
               !s.positions.empty();
      },
      10s);
  ASSERT_FALSE(state.positions.empty());
  EXPECT_FALSE(state.positions_reconciled) << "position rows do not replace missing fill reports";
  EXPECT_THROW(trader.prepare(order("blocked", "SHFE", "rb2610", Side::buy, "2", "3500"),
                              Offset::open, state.connection_generation, state.exposure_revision,
                              dispatch_deadline()),
               Error);
  const auto revision = state.exposure_revision;
  fake.call<void (*)(int, int)>("asterion_fake_trader_replay_order", 0, 0);
  state =
      wait_for(trader, [&](const BrokerSnapshot& s) { return s.exposure_revision > revision; }, 2s);
  ASSERT_TRUE(find(state, "filled"));
  EXPECT_EQ(find(state, "filled")->filled, d("2"));
  EXPECT_EQ(find(state, "filled")->status, BrokerOrderStatus::filled);
  EXPECT_FALSE(state.positions_reconciled);
  fake.call<void (*)(int)>("asterion_fake_trader_hold_trades", 0);
  state = wait_for(trader, [](const BrokerSnapshot& s) { return s.positions_reconciled; }, 10s);
  ASSERT_TRUE(state.positions_reconciled);
  EXPECT_EQ(state.trades.size(), 1U);
  EXPECT_EQ(state.positions[0].today, d("2"));
}
TEST_F(CtpTrader, AdditionalFillDuringPositionQueryForcesAnotherRefreshAndAllowsCancellation) {
  ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {});
  trader.start();
  trader.connect(configuration());
  ASSERT_TRUE(wait_for(trader, ready, 15s).positions_reconciled);
  send(trader, gate, order("partial", "SHFE", "rb2610", Side::buy, "5", "3500"), Offset::open,
       trader.snapshot().connection_generation, trader.snapshot().exposure_revision,
       dispatch_deadline());
  ASSERT_TRUE(find(wait_for(
                       trader,
                       [](const BrokerSnapshot& s) {
                         return find(s, "partial") &&
                                find(s, "partial")->status == BrokerOrderStatus::accepted;
                       },
                       2s),
                   "partial"));
  fake.call<void (*)(int)>("asterion_fake_trader_hold_positions", 1);
  fake.call<void (*)(int, int)>("asterion_fake_trader_fill", 0, 1);
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!fake.call<int (*)()>("asterion_fake_trader_pending_positions") &&
         std::chrono::steady_clock::now() < deadline) {
    trader.poll();
    std::this_thread::sleep_for(20ms);
  }
  ASSERT_GT(fake.call<int (*)()>("asterion_fake_trader_pending_positions"), 0);
  fake.call<void (*)(int, int)>("asterion_fake_trader_fill", 0, 1);
  auto state = wait_for(trader, [](const BrokerSnapshot& s) { return s.trades.size() == 2; }, 2s);
  ASSERT_EQ(state.trades.size(), 2U);
  EXPECT_FALSE(state.positions_reconciled);
  EXPECT_NO_THROW(cancel(trader, "partial"));
  fake.call<void (*)(int)>("asterion_fake_trader_hold_positions", 0);
  state = wait_for(trader, [](const BrokerSnapshot& s) { return s.positions_reconciled; }, 10s);
  ASSERT_TRUE(state.positions_reconciled);
  ASSERT_EQ(state.positions.size(), 1U);
  EXPECT_EQ(state.positions[0].today, d("2")) << "the held first query only contained one lot";
  EXPECT_EQ(find(state, "partial")->status, BrokerOrderStatus::cancelled);
}
TEST_F(CtpTrader, ExposureChangesBeforeOrDuringJournalingRejectTheEvaluatedOrder) {
  ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {});
  trader.start();
  trader.connect(configuration());
  ASSERT_TRUE(wait_for(trader, ready, 15s).positions_reconciled);
  send(trader, gate, order("working", "SHFE", "rb2610", Side::buy, "5", "3500"), Offset::open,
       trader.snapshot().connection_generation, trader.snapshot().exposure_revision,
       dispatch_deadline());
  auto before = wait_for(
      trader,
      [](const BrokerSnapshot& s) {
        return find(s, "working") && find(s, "working")->status == BrokerOrderStatus::accepted;
      },
      2s);
  cancel(trader, "working");
  auto state = wait_for(
      trader,
      [](const BrokerSnapshot& s) {
        return find(s, "working") && find(s, "working")->status == BrokerOrderStatus::cancelled;
      },
      2s);
  EXPECT_THROW(trader.prepare(order("stale.risk", "SHFE", "rb2610", Side::buy, "5", "3500"),
                              Offset::open, before.connection_generation, before.exposure_revision,
                              dispatch_deadline()),
               Error);
  auto prepared =
      trader.prepare(order("during.risk", "SHFE", "rb2610", Side::buy, "5", "3500"), Offset::open,
                     state.connection_generation, state.exposure_revision, dispatch_deadline());
  fake.call<void (*)(int, int)>("asterion_fake_trader_replay_order", 0, 0);
  ASSERT_GT(wait_for(
                trader,
                [&](const BrokerSnapshot& current) {
                  return current.exposure_revision > state.exposure_revision;
                },
                2s)
                .exposure_revision,
            state.exposure_revision);
  const auto result = await(trader, dispatch_with_permission(trader, gate, std::move(prepared)));
  EXPECT_FALSE(result.invoked);
  EXPECT_EQ(result.code, -1004);
  EXPECT_FALSE(find(trader.snapshot(), "during.risk"));
}
TEST_F(CtpTrader, PreparationHasNoBrokerEffectsAndDispatchTracksFills) {
  ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {});
  trader.start();
  EXPECT_THROW(send(trader, gate, order("o0", "SHFE", "rb2610", Side::buy, "1", "3500"),
                    Offset::open, trader.snapshot().connection_generation,
                    trader.snapshot().exposure_revision, dispatch_deadline()),
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

  auto prepared =
      trader.prepare(order("o1", "SHFE", "rb2610", Side::buy, "2", "3500.125"), Offset::open,
                     state.connection_generation, state.exposure_revision, dispatch_deadline());
  EXPECT_TRUE(trader.snapshot().orders.empty()) << "preparation cannot dispatch an order";
  const auto recorded = prepared->order();
  EXPECT_EQ(recorded.status, BrokerOrderStatus::submitted);
  EXPECT_TRUE(await(trader, dispatch_with_permission(trader, gate, std::move(prepared))).invoked);
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
  EXPECT_EQ(filled->broker_key, recorded.broker_key);
  EXPECT_EQ(filled->filled, d("2"));
  EXPECT_FALSE(filled->exchange_order_id.empty());
  ASSERT_EQ(state.trades.size(), 1U);
  EXPECT_EQ(state.trades[0].order_id, "o1");
  EXPECT_EQ(state.trades[0].price, d("3500.125"));
  ASSERT_EQ(state.positions.size(), 1U);
  EXPECT_EQ(state.positions[0].side, Side::buy);
  EXPECT_EQ(state.positions[0].today, d("2"));

  // SHFE requires a close bucket; an abandoned preparation sends nothing.
  EXPECT_THROW(send(trader, gate, order("o2", "SHFE", "rb2610", Side::sell, "1", "3500"),
                    Offset::close, trader.snapshot().connection_generation,
                    trader.snapshot().exposure_revision, dispatch_deadline()),
               std::invalid_argument);
  EXPECT_THROW(send(trader, gate, order("o3", "SHFE", "rb2610", Side::sell, "1.5", "3500"),
                    Offset::close_today, trader.snapshot().connection_generation,
                    trader.snapshot().exposure_revision, dispatch_deadline()),
               std::invalid_argument);
  auto abandoned = trader.prepare(order("o4", "SHFE", "rb2610", Side::sell, "1", "3500"),
                                  Offset::close_today, trader.snapshot().connection_generation,
                                  trader.snapshot().exposure_revision, dispatch_deadline());
  abandoned.reset();
  EXPECT_FALSE(find(trader.snapshot(), "o4"));
  EXPECT_THROW(send(trader, gate, order("o1", "SHFE", "rb2610", Side::buy, "1", "3500"),
                    Offset::open, trader.snapshot().connection_generation,
                    trader.snapshot().exposure_revision, dispatch_deadline()),
               Error)
      << "duplicate order ID";
  EXPECT_EQ(fake.query_rejections(), 0) << "queries respect the CTP flow limit";
  trader.disconnect();
  EXPECT_EQ(trader.snapshot().phase, "disconnected");
}
TEST_F(CtpTrader, RejectionsAndCancellation) {
  ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {});
  trader.start();
  trader.connect(configuration());
  ASSERT_EQ(wait_for(trader, ready, 15s).phase, "ready");
  send(trader, gate, order("bad", "DCE", "zz2609", Side::buy, "1", "100"), Offset::open,
       trader.snapshot().connection_generation, trader.snapshot().exposure_revision,
       dispatch_deadline());
  auto state = wait_for(
      trader,
      [](const BrokerSnapshot& s) {
        return find(s, "bad") && find(s, "bad")->status == BrokerOrderStatus::rejected;
      },
      5s);
  EXPECT_EQ(find(state, "bad")->error_code, 16);

  send(trader, gate, order("rest", "DCE", "m2609", Side::buy, "5", "2800"), Offset::open,
       trader.snapshot().connection_generation, trader.snapshot().exposure_revision,
       dispatch_deadline());
  state = wait_for(
      trader,
      [](const BrokerSnapshot& s) {
        return find(s, "rest") && find(s, "rest")->status == BrokerOrderStatus::accepted;
      },
      5s);
  ASSERT_EQ(find(state, "rest")->status, BrokerOrderStatus::accepted);
  cancel(trader, "rest");
  state = wait_for(
      trader,
      [](const BrokerSnapshot& s) {
        return find(s, "rest") && find(s, "rest")->status == BrokerOrderStatus::cancelled;
      },
      5s);
  EXPECT_EQ(find(state, "rest")->status, BrokerOrderStatus::cancelled);
  EXPECT_THROW(cancel(trader, "rest"), Error);
  EXPECT_THROW(cancel(trader, "missing"), Error);
  // DCE closes yesterday's positions first, so the generic close is allowed.
  EXPECT_NO_THROW(send(trader, gate, order("close", "DCE", "m2609", Side::sell, "1", "2800"),
                       Offset::close, trader.snapshot().connection_generation,
                       trader.snapshot().exposure_revision, dispatch_deadline()));
}
TEST_F(CtpTrader, ReconnectAndRestartKeepOrdersAttributed) {
  std::string key, next_key;
  {
    ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {});
    trader.start();
    trader.connect(configuration());
    ASSERT_EQ(wait_for(trader, ready, 15s).phase, "ready");
    key = send(trader, gate, order("resting", "SHFE", "rb2610", Side::buy, "4", "3400"),
               Offset::open, trader.snapshot().connection_generation,
               trader.snapshot().exposure_revision, dispatch_deadline())
              .broker_key;
    ASSERT_EQ(wait_for(
                  trader,
                  [](const BrokerSnapshot& s) {
                    return find(s, "resting") &&
                           find(s, "resting")->status == BrokerOrderStatus::accepted;
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
    const auto next = send(trader, gate, order("after", "SHFE", "rb2610", Side::buy, "4", "3401"),
                           Offset::open, trader.snapshot().connection_generation,
                           trader.snapshot().exposure_revision, dispatch_deadline());
    next_key = next.broker_key;
    EXPECT_NE(next.broker_key.substr(0, next.broker_key.rfind(':')), key.substr(0, key.rfind(':')));
    cancel(trader, "resting");
    EXPECT_EQ(wait_for(
                  trader,
                  [](const BrokerSnapshot& s) {
                    return find(s, "resting") &&
                           find(s, "resting")->status == BrokerOrderStatus::cancelled;
                  },
                  5s)
                  .phase,
              "ready");
  }
  // A restarted process attributes orders from its journal; others stay
  // unattributed but visible.
  ctp::Trader restarted(ASTERION_TEST_CTP_TRADER, flow, gate, [] {});
  restarted.start();
  restarted.connect(configuration(), {{{"20260928", key}, {"resting", 1}},
                                      {{"20260927", next_key}, {"yesterday", 2}}});
  const auto state = wait_for(restarted, ready, 15s);
  ASSERT_EQ(state.phase, "ready");
  ASSERT_EQ(state.orders.size(), 2U);
  EXPECT_EQ(find(state, "resting")->status, BrokerOrderStatus::cancelled);
  EXPECT_TRUE(find(state, ""));
  EXPECT_EQ(find(state, "yesterday"), nullptr);
  const auto effects = fake.call<int (*)()>("asterion_fake_catalog_side_effects");
  EXPECT_THROW(cancel(restarted, "yesterday"), Error);
  EXPECT_EQ(fake.call<int (*)()>("asterion_fake_catalog_side_effects"), effects);
  EXPECT_THROW(cancel(restarted, ""), Error);
}
TEST(ReportReconciliation, RestoredIdentityUsesCurrentDayAndLatestJournalSequence) {
  BrokerSnapshot state;
  state.trading_day = "20260928";
  ctp::ReportReconciler reports(state);
  reports.reset({{{"20260927", "1:1:1"}, {"old", 1}}, {{"20260928", "1:1:1"}, {"current", 2}}});
  CThostFtdcOrderField order{};
  order.FrontID = 1;
  order.SessionID = 1;
  order.Direction = THOST_FTDC_D_Buy;
  order.CombOffsetFlag[0] = THOST_FTDC_OF_Open;
  order.LimitPrice = 3500;
  order.VolumeTotalOriginal = order.VolumeTotal = 3;
  order.OrderSubmitStatus = THOST_FTDC_OSS_Accepted;
  order.OrderStatus = THOST_FTDC_OST_NoTradeQueueing;
  std::strcpy(order.ExchangeID, "SHFE");
  std::strcpy(order.InstrumentID, "rb2610");
  std::strcpy(order.OrderRef, "1");
  std::strcpy(order.TradingDay, "20260927");
  EXPECT_FALSE(reports.apply(order));
  EXPECT_TRUE(state.orders.empty());
  EXPECT_EQ(state.exposure_revision, 0U);
  std::strcpy(order.TradingDay, "20260928");
  reports.apply(order);
  ASSERT_EQ(state.orders.size(), 1U);
  EXPECT_EQ(state.orders.front().order_id, "current");
  EXPECT_EQ(reports.find_order("old"), nullptr);
  const auto revision = state.exposure_revision;
  CThostFtdcTradeField trade{};
  std::strcpy(trade.TradingDay, "20260927");
  EXPECT_FALSE(reports.apply(trade));
  EXPECT_TRUE(state.trades.empty());
  EXPECT_EQ(state.exposure_revision, revision);
  order.TradingDay[0] = '\0';
  EXPECT_THROW(reports.apply(order), std::runtime_error);
  reports.remember({"20260928", "1:1:1"}, {"latest", 5});
  reports.remember({"20260928", "1:1:1"}, {"stale", 3});
  EXPECT_EQ(state.orders.front().order_id, "latest");
  EXPECT_EQ(reports.find_order("current"), nullptr);
  EXPECT_EQ(reports.find_order("stale"), nullptr);
  EXPECT_NE(reports.find_order("latest"), nullptr);
}
TEST_F(CtpTrader, ReusedReferenceRetiresThePreviousCallerBinding) {
  ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {});
  const auto key =
      "1:" + std::to_string(fake.call<int (*)()>("asterion_fake_trader_next_session")) + ":1";
  // The journal can contain a request which never reached the broker. Its
  // reference is then absent from the broker's MaxOrderRef on recovery.
  trader.connect(configuration(), {{{"20260928", key}, {"unconfirmed", 1}}});
  ASSERT_EQ(wait_for(trader, ready, 15s).phase, "ready");
  const auto sent =
      send(trader, gate, order("replacement", "SHFE", "rb2610", Side::buy, "3", "3500"),
           Offset::open, trader.snapshot().connection_generation,
           trader.snapshot().exposure_revision, dispatch_deadline());
  ASSERT_EQ(sent.broker_key, key);
  const auto state = wait_for(
      trader, [](const BrokerSnapshot& value) { return find(value, "replacement") != nullptr; },
      5s);
  ASSERT_NE(find(state, "replacement"), nullptr);
  EXPECT_EQ(find(state, "unconfirmed"), nullptr);
  const auto effects = fake.call<int (*)()>("asterion_fake_catalog_side_effects");
  EXPECT_THROW(cancel(trader, "unconfirmed"), Error);
  EXPECT_EQ(fake.call<int (*)()>("asterion_fake_catalog_side_effects"), effects);
  EXPECT_NO_THROW(cancel(trader, "replacement"));
}
TEST_F(CtpTrader, DelayedInputRejectionCannotRejectAReconnectedOrder) {
  ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {});
  trader.connect(configuration());
  ASSERT_EQ(wait_for(trader, ready, 15s).phase, "ready");
  fake.call<void (*)(int)>("asterion_fake_trader_hold_insert_errors", 1);
  const auto old = send(trader, gate, order("old", "DCE", "zz2609", Side::buy, "3", "100"),
                        Offset::open, trader.snapshot().connection_generation,
                        trader.snapshot().exposure_revision, dispatch_deadline());
  const auto generation = trader.snapshot().connection_generation;
  fake.reconnect();
  ASSERT_EQ(wait_for(
                trader,
                [&](const BrokerSnapshot& state) {
                  return ready(state) && state.connection_generation != generation;
                },
                15s)
                .phase,
            "ready");
  const auto current = send(trader, gate, order("current", "DCE", "m2609", Side::buy, "3", "2800"),
                            Offset::open, trader.snapshot().connection_generation,
                            trader.snapshot().exposure_revision, dispatch_deadline());
  ASSERT_NE(old.broker_key, current.broker_key);
  ASSERT_EQ(old.broker_key.substr(old.broker_key.rfind(':')),
            current.broker_key.substr(current.broker_key.rfind(':')));
  ASSERT_NE(find(wait_for(
                     trader,
                     [](const BrokerSnapshot& state) {
                       const auto* order = find(state, "current");
                       return order && order->status == BrokerOrderStatus::accepted;
                     },
                     5s),
                 "current"),
            nullptr);
  fake.call<void (*)(int)>("asterion_fake_trader_hold_insert_errors", 0);
  // The quote response follows the released failures on the fake callback queue.
  ASSERT_TRUE(await(trader, trader.quote({"DCE", "m2609"})));
  const auto state = trader.snapshot();
  ASSERT_NE(find(state, "current"), nullptr);
  EXPECT_EQ(find(state, "current")->status, BrokerOrderStatus::accepted);
  EXPECT_EQ(find(state, "current")->instrument.symbol, "m2609");
  EXPECT_EQ(find(state, "old"), nullptr);
  EXPECT_NO_THROW(cancel(trader, "current"));
}
TEST_F(CtpTrader, CredentialFailuresStopBeforeTrading) {
  ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {});
  trader.start();
  EXPECT_THROW(trader.connect({"http://x", "9999", "1", "p", "a", "c"}), std::invalid_argument);
  EXPECT_THROW(trader.connect(configuration("secret", "")), std::invalid_argument);
  trader.connect(configuration("secret", "bad-auth"));
  auto state = wait_for(trader, [](const BrokerSnapshot& s) { return s.phase == "error"; }, 5s);
  EXPECT_EQ(state.error_code, 63);
  trader.connect(configuration("bad"));
  state = wait_for(trader, [](const BrokerSnapshot& s) { return s.phase == "error"; }, 5s);
  EXPECT_EQ(state.error_code, 3);
  EXPECT_THROW(send(trader, gate, order("x", "SHFE", "rb2610", Side::buy, "1", "3500"),
                    Offset::open, trader.snapshot().connection_generation,
                    trader.snapshot().exposure_revision, dispatch_deadline()),
               Error);
  EXPECT_THROW((ctp::Trader(fs::path("missing-library"), flow, gate, [] {})), Error);
}
TEST_F(CtpTrader, AccountRatesFillCostsWithoutAffectingTheSession) {
  ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {});
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
  ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {});
  trader.start();
  trader.connect(configuration());
  ASSERT_EQ(wait_for(trader, ready, 15s).phase, "ready");
  for (const int code : {-2, -3}) {
    reject.symbol<void (*)(int, int)>()(code, 1);
    auto quote = await(trader, trader.quote({"SHFE", "rb2610"}));
    ASSERT_TRUE(quote);
    EXPECT_EQ(quote->last, d("3500"));
    EXPECT_EQ(trader.snapshot().phase, "ready");
  }
  EXPECT_EQ(fake.query_rejections(), 2);
  auto pending =
      send(trader, gate, order("after.throttle", "SHFE", "rb2610", Side::buy, "4", "3400"),
           Offset::open, trader.snapshot().connection_generation,
           trader.snapshot().exposure_revision, dispatch_deadline());
  EXPECT_NE(pending.status, BrokerOrderStatus::rejected);
  EXPECT_NO_THROW(cancel(trader, "after.throttle"));
}
TEST_F(CtpTrader, ReconnectFencesAuthorizationBeforeAndDuringJournaling) {
  ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {});
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
  EXPECT_THROW(trader.prepare(order("stale", "SHFE", "rb2610", Side::buy, "4", "3400"),
                              Offset::open, state.connection_generation, state.exposure_revision,
                              dispatch_deadline()),
               Error);
  state = trader.snapshot();
  auto prepared = trader.prepare(order("during.journal", "SHFE", "rb2610", Side::buy, "4", "3400"),
                                 Offset::open, state.connection_generation, state.exposure_revision,
                                 dispatch_deadline());
  ASSERT_EQ(reconnect().phase, "ready");
  const auto result = await(trader, dispatch_with_permission(trader, gate, std::move(prepared)));
  EXPECT_FALSE(result.invoked);
  EXPECT_EQ(result.code, -1003);
  trader.disconnect();
  trader.connect(configuration());
  const auto synced = wait_for(trader, ready, 15s);
  EXPECT_TRUE(synced.trades.empty());
  EXPECT_TRUE(synced.orders.empty()) << "nothing was sent in the newer connection";
}
TEST_F(CtpTrader, DisconnectCompletesAThrottledQuoteWithoutRetryingItAfterReconnect) {
  ctp::SharedLibrary reject(ASTERION_TEST_CTP_TRADER, "asterion_fake_trader_reject_quotes",
                            "asterion_fake_trader_reject_quotes");
  ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {});
  trader.start();
  trader.connect(configuration());
  ASSERT_EQ(wait_for(trader, ready, 15s).phase, "ready");
  reject.symbol<void (*)(int, int)>()(-3, 100);
  auto query = trader.quote({"SHFE", "rb2610"});
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (!fake.query_rejections() && std::chrono::steady_clock::now() < deadline) {
    trader.poll();
    std::this_thread::sleep_for(20ms);
  }
  ASSERT_GT(fake.query_rejections(), 0);
  trader.disconnect();
  ASSERT_EQ(query.wait_for(2s), std::future_status::ready);
  EXPECT_FALSE(query.get());
  reject.symbol<void (*)(int, int)>()(0, 0);
  trader.connect(configuration());
  ASSERT_EQ(wait_for(trader, ready, 15s).phase, "ready");
  EXPECT_TRUE(await(trader, trader.quote({"SHFE", "rb2610"})));
}

TEST_F(CtpTrader, TimedOutQuoteRetiresRetriesAndLeavesTheWorkerAvailable) {
  ctp::SharedLibrary reject(ASTERION_TEST_CTP_TRADER, "asterion_fake_trader_reject_quotes",
                            "asterion_fake_trader_reject_quotes");
  ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {});
  trader.start();
  trader.connect(configuration());
  ASSERT_EQ(wait_for(trader, ready, 15s).phase, "ready");
  reject.symbol<void (*)(int, int)>()(-3, 100);
  EXPECT_FALSE(await(trader, trader.quote({"SHFE", "rb2610"})));
  EXPECT_EQ(trader.snapshot().phase, "ready");
  reject.symbol<void (*)(int, int)>()(0, 0);
  EXPECT_TRUE(await(trader, trader.quote({"SHFE", "rb2610"})));
  EXPECT_EQ(trader.snapshot().phase, "ready");
}

TEST_F(CtpTrader, LateQueryRepliesCannotReplaceReconnectedAccountState) {
  ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {});
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
  trader.poll();
  const auto after = trader.snapshot();
  EXPECT_EQ(after.sequence, synced.sequence);
  EXPECT_EQ(after.synchronized_ms, synced.synchronized_ms);
  ASSERT_TRUE(after.funds);
  EXPECT_EQ(after.funds->balance, synced.funds->balance);
  EXPECT_TRUE(after.positions.empty());
  EXPECT_TRUE(after.costs.empty());
}
TEST_F(CtpTrader, QuotesGoAheadOfQueuedRateQueries) {
  ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {});
  trader.start();
  trader.connect(configuration());
  ASSERT_EQ(wait_for(trader, ready, 15s).phase, "ready");
  // Ten rate queries need over ten seconds at the CTP query limit.
  std::vector<std::pair<InstrumentId, std::string>> contracts;
  for (const auto* symbol : {"rb2610", "rb2611", "rb2612", "rb2701", "rb2702"})
    contracts.push_back({{"SHFE", symbol}, "rb"});
  trader.query_costs(contracts);
  const auto started = std::chrono::steady_clock::now();
  const auto quote = await(trader, trader.quote({"SHFE", "rb2610"}));
  ASSERT_TRUE(quote);
  EXPECT_EQ(quote->last, d("3500"));
  EXPECT_LT(std::chrono::steady_clock::now() - started, 4s)
      << "an order's quote must not wait behind rate queries";
}

TEST_F(CtpTrader, RecordContinuationRejectsAChangedConnectionOrExposureBeforeDisconnecting) {
  ctp::Trader trader(ASTERION_TEST_CTP_TRADER, flow, gate, [] {});
  trader.start();
  trader.connect(configuration());
  const auto before = wait_for(trader, ready, 15s);
  ASSERT_TRUE(before.positions_reconciled);
  EXPECT_THROW(
      trader.disconnect_checked(before.connection_generation + 1, before.exposure_revision), Error);
  EXPECT_EQ(trader.snapshot().phase, "ready");
  send(trader, gate, order("working", "SHFE", "rb2610", Side::buy, "3", "3500"), Offset::open,
       before.connection_generation, before.exposure_revision, dispatch_deadline());
  ASSERT_TRUE(wait_for(
                  trader,
                  [](const BrokerSnapshot& state) {
                    return find(state, "working") &&
                           find(state, "working")->status == BrokerOrderStatus::accepted;
                  },
                  2s)
                  .phase == "ready");
  EXPECT_THROW(trader.disconnect_checked(before.connection_generation, before.exposure_revision),
               Error);
  EXPECT_EQ(trader.snapshot().phase, "ready");
  cancel(trader, "working");
  const auto after = wait_for(
      trader,
      [](const BrokerSnapshot& state) {
        return find(state, "working") &&
               find(state, "working")->status == BrokerOrderStatus::cancelled;
      },
      2s);
  ASSERT_EQ(find(after, "working")->status, BrokerOrderStatus::cancelled);
  EXPECT_NO_THROW(trader.disconnect_checked(after.connection_generation, after.exposure_revision));
  EXPECT_EQ(trader.snapshot().phase, "disconnected");
}
