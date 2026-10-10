#include "ctp_support.hpp"
#include "support/local_listener.hpp"
#include "trading/journal_fixture.hpp"
#include "live_session.hpp"
#include "risk_module.hpp"
#include <barrier>
#include <future>
#include <asterion/foundation/error.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/v1/market.pb.h>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <thread>
using namespace asterion;

using namespace std::chrono_literals;
namespace fs = std::filesystem;
namespace {
// Blocking driver for account behavior tests. Production callers await these
// futures without occupying their I/O thread.
class LiveSession {
  trading::LiveSession session_;

public:
  template <class... Args>
  explicit LiveSession(Args&&... args) : session_(std::forward<Args>(args)...) {
    session_.initialized().get();
  }
  void connect(std::string password, std::string auth) {
    session_.connect(std::move(password), std::move(auth)).get();
  }
  void disconnect() { session_.disconnect().get(); }
  void query_costs() { session_.query_costs().get(); }
  Json snapshot() const { return session_.snapshot().get(); }
  auto execute(std::string_view record, std::string_view policy, const Json& command) {
    return session_.execute(record, policy, command);
  }
  bool recovery_required() const { return session_.recovery_required(); }
  auto health() const { return session_.health(); }
  void close_admission() { session_.close_admission(); }
  void stop() { session_.stop(); }
  auto stopped() const { return session_.stopped(); }
};
// Test control entry point exported by the CTP SDK double. The handle keeps
// the library, and so its exchange state, loaded across sessions.
struct FakeExchange {
  ctp::SharedLibrary library{ASTERION_TEST_CTP_TRADER, "asterion_fake_trader_reset"};
  void reset() { library.symbol<void (*)()>()(); }
  template <class F, class... Args> auto call(const char* name, Args... args) {
    ctp::SharedLibrary control(ASTERION_TEST_CTP_TRADER, name);
    return control.symbol<F>()(args...);
  }
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
  return {{"version", 5},
          {"account_id", "fixture-account"},
          {"type", "live_ctp"},
          {"broker",
           {{"front", "tcp://127.0.0.1:41205"},
            {"broker_id", "9999"},
            {"user_id", "000001"},
            {"app_id", "client_app"}}},
          {"policy",
           {{"risk",
             {{"max_order_quantity", "5"},
              {"max_gross_quantity", "10"},
              {"max_working_orders", working_orders}}},
            {"max_price_deviation", deviation},
            {"contracts", Json::array({contract("rb2610")})}}}};
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
bool order_has_status(const Json& state, const std::string& id, std::string_view status) {
  const auto order = find_order(state, id);
  return order.is_object() && order.at("status") == status;
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
void act(LiveSession& session, const Json& command) {
  const auto state = session.snapshot();
  session
      .execute(state.at("account_id").get<std::string>(),
               state.at("policy_revision").get<std::string>(), command)
      .get();
}
Json policy_command(const LiveSession& session, Json definition) {
  return {{"request_id", "policy.change"},
          {"action", "live_policy"},
          {"risk_artifact", session.snapshot().at("risk_artifact")},
          {"policy", std::move(definition)}};
}
Json ready(LiveSession& session) {
  session.connect("secret", "auth-code");
  return wait_for(session, [](const Json& s) { return s.at("phase") == "ready"; });
}
struct HeldCommit {
  HeldCommit() { sqlite::hold_commits_for_testing(true); }
  ~HeldCommit() { release(); }
  void release() { sqlite::hold_commits_for_testing(false); }
  bool waiting() const {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!sqlite::commit_waiting_for_testing() && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(10ms);
    return sqlite::commit_waiting_for_testing();
  }
};
struct HeldRiskPlugin {
  fs::path original = native_plugin_directory();
  ctp::SharedLibrary hold_library{RISK_FIXTURE, "asterion_fixture_risk_hold"};
  ctp::SharedLibrary waiting_library{RISK_FIXTURE, "asterion_fixture_risk_waiting"};
  explicit HeldRiskPlugin(int stage) {
    configure_native_plugins(fs::path(RISK_FIXTURE).parent_path());
    hold(stage);
  }
  ~HeldRiskPlugin() {
    hold(0);
    configure_native_plugins(original);
  }
  void hold(int stage) { hold_library.symbol<void (*)(int)>()(stage); }
  bool waiting(int stage) const {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (waiting_library.symbol<int (*)()>()() != stage &&
           std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(10ms);
    return waiting_library.symbol<int (*)()>()() == stage;
  }
};
// The market service of the account's machine as a strategy run reads it: the
// minute series of one contract, set by the test. The last bar is the one
// still forming.
class FakeMarket {
public:
  FakeMarket()
      : endpoint_("/tmp/ast-market-" + unique_process_id().substr(0, 12) + ".sock"),
        listener_(endpoint_, 8), thread_([this](std::stop_token stop) { serve(stop); }) {}
  // The series of one trading day, beginning at this minute of the test's clock.
  void set(const std::string& trading_day, std::initializer_list<const char*> closes,
           bool interrupted = false, int minute = 0) {
    market::v1::MinuteSeries series;
    series.set_trading_day(trading_day);
    std::int64_t start = base + minute * 60000;
    series.set_first_observation_ms(start - 1);
    series.set_interrupted(interrupted);
    for (const auto* close : closes) {
      auto* bar = series.add_bars();
      bar->set_start_ms(start);
      for (auto* price :
           {bar->mutable_open(), bar->mutable_high(), bar->mutable_low(), bar->mutable_close()})
        *price = close;
      bar->set_volume(1);
      start += 60000;
    }
    std::lock_guard lock(mutex_);
    series_ = std::move(series);
  }
  Json start(std::string run, const char* quantity, const char* sides = "long",
             Json rule = {{"kind", "moving_average"}, {"fast", 1}, {"slow", 2}}) const {
    return {{"request_id", std::move(run)},
            {"action", "strategy_start"},
            {"venue", "SHFE"},
            {"symbol", "rb2610"},
            {"strategy", {{"quantity", quantity}, {"sides", sides}, {"rule", std::move(rule)}}},
            {"market_endpoint", endpoint_},
            {"market_service", "market"}};
  }
  // The order a run places for the bar at this position of the series.
  static std::string order(const std::string& run, int bar, const char* suffix = "") {
    return run + "." + std::to_string(base + bar * 60000) + suffix;
  }

private:
  static constexpr std::int64_t base = 1'790'000'040'000;
  void serve(const std::stop_token& stop) {
    while (!stop.stop_requested()) {
      try {
        auto peer = listener_.accept(20ms);
        market::v1::Request request;
        if (!request.ParseFromString(peer.receive(1s)))
          continue;
        market::v1::Response response;
        response.set_version(1);
        response.set_service_id(request.service_id());
        response.set_correlation_id(request.correlation_id());
        {
          std::lock_guard lock(mutex_);
          *response.mutable_minutes() = series_;
        }
        *response.mutable_minutes()->mutable_instrument() = request.minutes().instrument();
        peer.send(response.SerializeAsString(), 1s);
      } catch (const std::exception&) {
        // Idle accept deadline, or a reader that went away.
      }
    }
  }
  std::string endpoint_;
  testing_support::LocalListener listener_;
  std::mutex mutex_;
  market::v1::MinuteSeries series_;
  std::jthread thread_;
};
Decimal long_position(const Json& state) {
  Decimal held;
  for (const auto& position : state.at("positions"))
    if (position.at("side") == "buy")
      held = held + Decimal::parse(position.at("today").get<std::string>()) +
             Decimal::parse(position.at("yesterday").get<std::string>());
  return held;
}
Decimal short_position(const Json& state) {
  Decimal held;
  for (const auto& position : state.at("positions"))
    if (position.at("side") == "sell")
      held = held + Decimal::parse(position.at("today").get<std::string>()) +
             Decimal::parse(position.at("yesterday").get<std::string>());
  return held;
}
Json stop_strategy(std::string id) {
  return {{"request_id", std::move(id)}, {"action", "strategy_stop"}};
}
bool strategy_stopped(const Json& state) {
  return state.at("strategy").is_object() && state.at("strategy").at("state") == "stopped";
}
class Live : public ::testing::Test {
protected:
  FakeExchange exchange;
  Directory directory;
  Directory owners;
  void SetUp() override { exchange.reset(); }
};
} // namespace
TEST_F(Live, PolicyRevisionsRetainExposureFenceStaleCommandsAndSurviveRecoveryAndSegments) {
  test::SmallJournalSegments budget(2);
  std::string initial, revised, record;
  Json change;
  {
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest(2));
    initial = session.snapshot().at("policy_revision");
    record = session.snapshot().at("account_id");
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    act(session, submit("position", "2"));
    ASSERT_FALSE(wait_for(session, [](const Json& state) { return !state.at("positions").empty(); })
                     .at("positions")
                     .empty());
    auto next = manifest(2).at("policy");
    next["risk"]["max_gross_quantity"] = "1";
    next["contracts"].push_back(contract("rb2611", "rb", "2026-11"));
    change = policy_command(session, next);
    act(session, change);
    const auto state = session.snapshot();
    revised = state.at("policy_revision");
    EXPECT_NE(revised, initial);
    EXPECT_EQ(state.at("account_id"), record);
    EXPECT_EQ(state.at("contracts").size(), 2U);
    EXPECT_EQ(state.at("phase"), "disconnected");
    EXPECT_TRUE(state.at("authorization").is_null());
    const auto count = session.snapshot().at("capacity").at("records_used");
    EXPECT_NO_THROW(session.execute(record, initial, change).get());
    EXPECT_EQ(session.snapshot().at("capacity").at("records_used"), count);
    EXPECT_THROW(session.execute(record, initial, submit("stale", "1")).get(), Error);
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize("new.policy.authorization"));
    EXPECT_THROW(act(session, submit("risk.remains", "1")), std::invalid_argument);
    auto close = submit("close", "2");
    close["side"] = "sell";
    close["offset"] = "close_today";
    EXPECT_NO_THROW(act(session, close));
    EXPECT_TRUE(wait_for(session, [](const Json& state) { return state.at("positions").empty(); })
                    .at("positions")
                    .empty());
  }
  {
    LiveSession recovered(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
    EXPECT_EQ(recovered.snapshot().at("policy_revision"), revised);
    EXPECT_EQ(recovered.snapshot().at("risk").at("max_gross_quantity"), "1");
    EXPECT_NO_THROW(recovered.execute(record, initial, change).get());
    ASSERT_EQ(ready(recovered).at("phase"), "ready");
    EXPECT_EQ(recovered.snapshot().at("policy_revision"), revised);
    EXPECT_EQ(recovered.snapshot().at("account_id"), record);
  }
  LiveSession continued(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
  EXPECT_EQ(continued.snapshot().at("policy_revision"), revised);
  EXPECT_EQ(continued.snapshot().at("risk").at("max_gross_quantity"), "1");
}
TEST_F(Live, PolicyCannotRemoveOrRedefineContractsWithWorkingOrders) {
  LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
  ASSERT_EQ(ready(session).at("phase"), "ready");
  act(session, authorize());
  act(session, submit("resting", "3"));
  const auto revision = session.snapshot().at("policy_revision");
  auto next = manifest().at("policy");
  next["contracts"] = Json::array({contract("rb2611", "rb", "2026-11")});
  EXPECT_THROW(act(session, policy_command(session, next)), Error);
  next = manifest().at("policy");
  next["contracts"][0]["multiplier"] = "20";
  EXPECT_THROW(act(session, policy_command(session, next)), Error);
  EXPECT_EQ(session.snapshot().at("policy_revision"), revision);
  EXPECT_EQ(session.snapshot().at("phase"), "ready");
}
TEST_F(Live, PolicyJournalFailureKeepsOriginalRevisionAndRequiresRecovery) {
  std::string revision;
  {
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    revision = session.snapshot().at("policy_revision");
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    auto next = manifest().at("policy");
    next["risk"]["max_gross_quantity"] = "8";
    const auto command = policy_command(session, next);
    sqlite::fail_next_commits_for_testing(1);
    EXPECT_THROW(act(session, command), std::runtime_error);
    EXPECT_TRUE(session.recovery_required());
    EXPECT_EQ(session.snapshot().at("phase"), "disconnected");
    EXPECT_EQ(session.snapshot().at("policy_revision"), revision);
    EXPECT_TRUE(session.snapshot().at("authorization").is_null());
  }
  LiveSession recovered(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
  EXPECT_EQ(recovered.snapshot().at("policy_revision"), revision);
  EXPECT_EQ(recovered.snapshot().at("risk").at("max_gross_quantity"), "10");
}
TEST_F(Live, PolicyChangeCannotReplaceMissingOwnedAlgorithmWithInstalledBytes) {
  LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
  ASSERT_EQ(ready(session).at("phase"), "ready");
  const auto before = session.snapshot();
  const auto file = risk_providers::Module::filename(directory.path / "plugins" /
                                                     before.at("risk_artifact").get<std::string>());
  Directory held;
  fs::rename(file, held.path / file.filename());
  EXPECT_THROW(act(session, policy_command(session, manifest().at("policy"))),
               std::invalid_argument);
  EXPECT_EQ(session.snapshot().at("policy_revision"), before.at("policy_revision"));
  EXPECT_FALSE(fs::exists(file));
  fs::rename(held.path / file.filename(), file);
}
TEST_F(Live, SlowPolicyPreparationRechecksTheAccountAndRetiresRejectedAlgorithmsOffItsThread) {
  LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
  ASSERT_EQ(ready(session).at("phase"), "ready");
  act(session, authorize());
  const auto before = session.snapshot();
  auto command = policy_command(session, manifest().at("policy"));
  command["risk_artifact"] = sha256_file(RISK_FIXTURE);
  const auto record = before.at("account_id").get<std::string>();
  const auto policy = before.at("policy_revision").get<std::string>();
  std::future<void> changed, revoked, disconnected;
  std::future<Json> observed;
  HeldRiskPlugin held(1);
  changed = session.execute(record, policy, command);
  ASSERT_TRUE(held.waiting(1));
  revoked = session.execute(record, policy,
                            {{"request_id", "revoke.preparing"}, {"action", "live_revoke"}});
  observed = std::async(std::launch::async, [&] { return session.snapshot(); });
  ASSERT_EQ(observed.wait_for(1s), std::future_status::ready);
  const auto preparing = observed.get();
  EXPECT_TRUE(preparing.at("authorization").is_null());
  EXPECT_EQ(preparing.at("policy_revision"), policy);
  EXPECT_EQ(preparing.at("capacity"), before.at("capacity"));
  EXPECT_EQ(changed.wait_for(0ms), std::future_status::timeout);
  EXPECT_EQ(revoked.wait_for(0ms), std::future_status::timeout);
  disconnected = std::async(std::launch::async, [&] { session.disconnect(); });
  ASSERT_EQ(disconnected.wait_for(1s), std::future_status::ready);
  disconnected.get();
  // The candidate must now fail fresh account validation and release off-thread.
  held.hold(2);
  ASSERT_TRUE(held.waiting(2));
  observed = std::async(std::launch::async, [&] { return session.snapshot(); });
  ASSERT_EQ(observed.wait_for(1s), std::future_status::ready);
  const auto retiring = observed.get();
  EXPECT_EQ(retiring.at("phase"), "disconnected");
  EXPECT_EQ(retiring.at("policy_revision"), policy);
  EXPECT_EQ(retiring.at("risk_artifact"), before.at("risk_artifact"));
  EXPECT_EQ(changed.wait_for(0ms), std::future_status::timeout);
  held.hold(0);
  EXPECT_THROW(changed.get(), Error);
  EXPECT_NO_THROW(revoked.get());
  EXPECT_FALSE(session.recovery_required());
}
TEST_F(Live, PolicyAdoptionPrecedesSlowRetirementWithoutBlockingAccountViews) {
  LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
  ASSERT_EQ(ready(session).at("phase"), "ready");
  const auto original_artifact = session.snapshot().at("risk_artifact");
  std::future<void> changed;
  std::future<Json> observed;
  HeldRiskPlugin held(0);
  auto upgrade = policy_command(session, manifest().at("policy"));
  upgrade["risk_artifact"] = sha256_file(RISK_FIXTURE);
  act(session, upgrade);
  ASSERT_EQ(ready(session).at("phase"), "ready");
  const auto before = session.snapshot();
  configure_native_plugins(held.original);
  auto replacement = policy_command(session, manifest().at("policy"));
  replacement["request_id"] = "policy.replace";
  replacement["risk_artifact"] = original_artifact;
  held.hold(2);
  changed = session.execute(before.at("account_id").get<std::string>(),
                            before.at("policy_revision").get<std::string>(), replacement);
  ASSERT_TRUE(held.waiting(2));
  observed = std::async(std::launch::async, [&] { return session.snapshot(); });
  ASSERT_EQ(observed.wait_for(1s), std::future_status::ready);
  const auto adopted = observed.get();
  EXPECT_EQ(adopted.at("risk_artifact"), original_artifact);
  EXPECT_NE(adopted.at("policy_revision"), before.at("policy_revision"));
  EXPECT_EQ(adopted.at("phase"), "disconnected");
  EXPECT_TRUE(adopted.at("authorization").is_null());
  EXPECT_EQ(changed.wait_for(0ms), std::future_status::timeout);
  held.hold(0);
  EXPECT_NO_THROW(changed.get());
}
TEST_F(Live, AccountRecordOwnershipSurvivesConnectionChangesAndProcessLifetimes) {
  Directory alias;
  auto alternative = manifest();
  alternative["broker"]["front"] = "tcp://127.0.0.1:41206";
  alternative["broker"]["app_id"] = "another_app";
  {
    LiveSession owner(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    EXPECT_THROW((LiveSession{alias.path, ASTERION_TEST_CTP_TRADER, owners.path, alternative}),
                 Error);
    EXPECT_TRUE(fs::is_empty(alias.path));
  }
  // Releasing the process lock does not make a second risk history valid.
  EXPECT_THROW((LiveSession{alias.path, ASTERION_TEST_CTP_TRADER, owners.path, alternative}),
               Error);
  EXPECT_TRUE(fs::is_empty(alias.path));
  LiveSession recovered(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
  EXPECT_TRUE(recovered.snapshot().at("authorization").is_null());
  // Separate account records can connect with the same broker/investor details.
  alternative["account_id"] = "another-account";
  EXPECT_NO_THROW((LiveSession{alias.path, ASTERION_TEST_CTP_TRADER, owners.path, alternative}));
}
TEST_F(Live, MissingInitializedJournalCannotBeRecreatedAsAnEmptyAccount) {
  {
    LiveSession owner(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
  }
  Directory held;
  fs::rename(directory.path / "journal.sqlite", held.path / "journal.sqlite");
  EXPECT_THROW((LiveSession{directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest()}),
               Error);
  EXPECT_FALSE(fs::exists(directory.path / "journal.sqlite"));
  fs::rename(held.path / "journal.sqlite", directory.path / "journal.sqlite");
  EXPECT_NO_THROW((LiveSession{directory.path, ASTERION_TEST_CTP_TRADER, owners.path}));
}
TEST_F(Live, CancellationJournalFailureCannotReachTheBroker) {
  {
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    act(session, submit("resting", "4"));
    ASSERT_EQ(find_order(wait_for(session,
                                  [](const Json& state) {
                                    return order_has_status(state, "resting", "accepted");
                                  }),
                         "resting")
                  .at("status"),
              "accepted");
    const auto effects = exchange.call<int (*)()>("asterion_fake_catalog_side_effects");
    sqlite::fail_next_commits_for_testing(1);
    EXPECT_THROW(
        act(session, {{"request_id", "cancel"}, {"action", "cancel"}, {"order_id", "resting"}}),
        std::runtime_error);
    EXPECT_EQ(exchange.call<int (*)()>("asterion_fake_catalog_side_effects"), effects);
    EXPECT_EQ(find_order(session.snapshot(), "resting").at("status"), "accepted");
    EXPECT_THROW(act(session, submit("blocked", "1")), std::invalid_argument);
  }
  EXPECT_EQ(test::journal_size(directory.path), 3U);
}
TEST_F(Live, FailedCancellationRecordsUnknownOutcomeAndNeverReplaysTheRequest) {
  const Json cancel{{"request_id", "cancel"}, {"action", "cancel"}, {"order_id", "resting"}};
  {
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    act(session, submit("resting", "4"));
    exchange.call<void (*)(int)>("asterion_fake_trader_cancel_code", -2);
    EXPECT_THROW(act(session, cancel), Error);
    exchange.call<void (*)(int)>("asterion_fake_trader_cancel_code", 0);
    const auto effects = exchange.call<int (*)()>("asterion_fake_catalog_side_effects");
    EXPECT_NO_THROW(act(session, cancel));
    EXPECT_EQ(exchange.call<int (*)()>("asterion_fake_catalog_side_effects"), effects);
  }
  ASSERT_EQ(test::journal_size(directory.path), 5U);
  EXPECT_EQ(
      test::read_record(test::journal_record(directory.path, 3)),
      Json({{"command", cancel},
            {"policy_revision",
             test::read_record(test::journal_record(directory.path, 0)).at("policy_revision")}}));
  EXPECT_EQ(test::read_record(test::journal_record(directory.path, 4)),
            Json({{"cancel_result", "cancel"}, {"outcome", "unknown"}}));
  {
    LiveSession recovered(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
    ASSERT_EQ(ready(recovered).at("phase"), "ready");
    const auto effects = exchange.call<int (*)()>("asterion_fake_catalog_side_effects");
    EXPECT_NO_THROW(act(recovered, cancel));
    EXPECT_EQ(exchange.call<int (*)()>("asterion_fake_catalog_side_effects"), effects);
    EXPECT_EQ(find_order(recovered.snapshot(), "resting").at("status"), "accepted");
  }
}
TEST_F(Live, IncompleteFailedOrForeignDayQuotesNeverJournalAnOrder) {
  {
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    for (const int mode : {1, 3, 4}) {
      exchange.call<void (*)(int)>("asterion_fake_trader_quote_mode", mode);
      EXPECT_THROW(act(session, submit("quote." + std::to_string(mode), "1")),
                   std::invalid_argument);
      EXPECT_TRUE(session.snapshot().at("orders").empty());
    }
    exchange.call<void (*)(int)>("asterion_fake_trader_quote_mode", 5);
    EXPECT_NO_THROW(act(session, submit("completed", "1")));
  }
  SqliteJournal journal(directory.path, {"plugins", "ctp-flow"});
  journal.start();
  for (const int mode : {1, 3, 4})
    EXPECT_TRUE(journal.command("submit.quote." + std::to_string(mode)).is_null());
  EXPECT_EQ(journal.command("submit.completed").at("command"), submit("completed", "1"));
}
TEST_F(Live, FilledOrdersDoNotCreateRiskHeadroomBeforePositionsAreReconciled) {
  auto input = manifest(3);
  input["policy"]["risk"]["max_gross_quantity"] = "2";
  LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, input);
  ASSERT_EQ(ready(session).at("phase"), "ready");
  act(session, authorize());
  exchange.call<void (*)(int)>("asterion_fake_trader_hold_positions", 1);
  act(session, submit("filled", "2"));
  auto state = wait_for(session, [](const Json& s) {
    const auto order = find_order(s, "filled");
    return order.is_object() && order.at("status") == "filled";
  });
  ASSERT_EQ(find_order(state, "filled").at("status"), "filled");
  ASSERT_TRUE(state.at("positions").empty());
  EXPECT_THROW(act(session, submit("too.early", "2")), Error);
  EXPECT_TRUE(find_order(session.snapshot(), "too.early").is_null());
  exchange.call<void (*)(int)>("asterion_fake_trader_hold_positions", 0);
  state = wait_for(session, [](const Json& s) { return !s.at("positions").empty(); });
  ASSERT_EQ(state.at("positions")[0].at("today"), "2");
  EXPECT_THROW(act(session, submit("over.limit", "2")), std::invalid_argument);
  EXPECT_TRUE(find_order(session.snapshot(), "over.limit").is_null());
  EXPECT_EQ(session.snapshot().at("orders").size(), 1U);
}
TEST_F(Live, SlowDurabilityAllowsSnapshotsAndRevocationWithoutSendingThePreparedOrder) {
  test::SmallJournalSegments budget(1);
  {
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    const auto before = session.snapshot();
    std::future<void> placed, revoked, queued_grant, queued_order, queued_cancel;
    std::vector<std::future<void>> backlog;
    std::future<Json> observed;
    // Declared after futures so the barrier is released before they are destroyed.
    HeldCommit held;
    placed = std::async(std::launch::async, [&] { act(session, submit("slow", "2")); });
    ASSERT_TRUE(held.waiting());
    std::this_thread::sleep_for(750ms);
    const auto health = session.health();
    EXPECT_TRUE(health.state.observed);
    EXPECT_LT(health.state.age_ms, 700U);
    EXPECT_TRUE(health.persistence.pending);
    EXPECT_GE(health.persistence.age_ms, 750U);
    EXPECT_TRUE(health.command.pending);
    EXPECT_FALSE(session.recovery_required());
    observed = std::async(std::launch::async, [&] { return session.snapshot(); });
    ASSERT_EQ(observed.wait_for(1s), std::future_status::ready);
    const auto during = observed.get();
    EXPECT_TRUE(during.at("orders").empty());
    EXPECT_EQ(during.at("capacity"), before.at("capacity"));
    const auto record = before.at("account_id").get<std::string>();
    const auto policy = before.at("policy_revision").get<std::string>();
    queued_grant = session.execute(record, policy, authorize("queued.grant"));
    queued_order = session.execute(record, policy, submit("queued.order", "1"));
    for (int i = 0; i < 61; ++i)
      backlog.push_back(session.execute(record, policy, submit("queued.order", "1")));
    EXPECT_THROW(static_cast<void>(session.execute(record, policy, submit("overflow", "1"))),
                 Error);
    // Controls and durable completion retain capacity with all 64 normal slots occupied.
    queued_cancel = session.execute(
        record, policy,
        {{"request_id", "cancel.slow"}, {"action", "cancel"}, {"order_id", "slow"}});
    // FIFO read proves these requests have entered account admission before revoke.
    EXPECT_FALSE(session.snapshot().at("authorization").is_null());
    revoked = std::async(std::launch::async, [&] {
      act(session, {{"request_id", "revoke.slow"}, {"action", "live_revoke"}});
    });
    observed = std::async(std::launch::async, [&] {
      return wait_for(session,
                      [](const Json& state) { return state.at("authorization").is_null(); });
    });
    ASSERT_EQ(observed.wait_for(1s), std::future_status::ready);
    EXPECT_TRUE(observed.get().at("authorization").is_null());
    EXPECT_EQ(placed.wait_for(0ms), std::future_status::timeout);
    EXPECT_EQ(revoked.wait_for(0ms), std::future_status::timeout);
    held.release();
    EXPECT_THROW(placed.get(), Error);
    EXPECT_THROW(queued_grant.get(), Error);
    EXPECT_THROW(queued_order.get(), Error);
    for (auto& request : backlog)
      EXPECT_THROW(request.get(), Error);
    EXPECT_THROW(queued_cancel.get(), Error) << "the completed intent is definitely not sent";
    EXPECT_NO_THROW(revoked.get());
    const auto after = session.snapshot();
    EXPECT_TRUE(after.at("orders").empty());
    EXPECT_TRUE(after.at("unconfirmed").empty());
    EXPECT_GT(after.at("segment_count"), before.at("segment_count"));
    EXPECT_EQ(after.at("account_id"), before.at("account_id"));
    EXPECT_NO_THROW(act(session, submit("slow", "2"))) << "retry acknowledges, never resends";
  }
  LiveSession recovered(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
  const auto state = ready(recovered);
  EXPECT_TRUE(state.at("orders").empty());
  EXPECT_TRUE(state.at("unconfirmed").empty());
  EXPECT_TRUE(state.at("authorization").is_null());
}
TEST_F(Live, CommandsQueuedBeforeEntryRevocationCannotBorrowItsNewRevision) {
  LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
  ASSERT_EQ(ready(session).at("phase"), "ready");
  std::future<void> evaluating, queued_grant, revoked;
  HeldRiskPlugin held(0);
  auto upgrade = policy_command(session, manifest().at("policy"));
  upgrade["risk_artifact"] = sha256_file(RISK_FIXTURE);
  act(session, upgrade);
  ASSERT_EQ(ready(session).at("phase"), "ready");
  act(session, authorize());
  const auto before = session.snapshot();
  const auto record = before.at("account_id").get<std::string>();
  const auto policy = before.at("policy_revision").get<std::string>();
  held.hold(3);
  evaluating = session.execute(record, policy, submit("slow.risk", "1"));
  ASSERT_TRUE(held.waiting(3));
  const auto blocked = session.health();
  std::this_thread::sleep_for(50ms);
  const auto later = session.health();
  EXPECT_TRUE(later.state.pending);
  EXPECT_GE(later.state.age_ms, blocked.state.age_ms + 50);
  EXPECT_FALSE(later.persistence.pending);
  EXPECT_FALSE(session.recovery_required());
  // Both requests enter while the account is inside a trusted risk plugin.
  // Taking the old grant's revision only when the owner dequeues it would let
  // that grant borrow the revoke's newer revision and restore authorization.
  queued_grant = session.execute(record, policy, authorize("queued.before.revoke"));
  revoked =
      session.execute(record, policy, {{"request_id", "entry.revoke"}, {"action", "live_revoke"}});
  held.hold(0);
  EXPECT_THROW(evaluating.get(), std::invalid_argument); // Fixture rejects risk.
  EXPECT_THROW(queued_grant.get(), Error);
  EXPECT_NO_THROW(revoked.get());
  const auto after = session.snapshot();
  EXPECT_TRUE(after.at("authorization").is_null());
  EXPECT_TRUE(after.at("orders").empty());
  EXPECT_EQ(after.at("capacity").at("records_used").get<int>(),
            before.at("capacity").at("records_used").get<int>() + 1);
  EXPECT_NO_THROW(act(session, authorize("fresh.after.revoke")));
  EXPECT_FALSE(session.snapshot().at("authorization").is_null());
}
TEST_F(Live, RevocationDuringDurableAuthorizationCannotRestorePermission) {
  LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
  ASSERT_EQ(ready(session).at("phase"), "ready");
  act(session, authorize("initial.grant"));
  std::future<void> granted, revoked;
  std::future<Json> observed;
  HeldCommit held;
  granted = std::async(std::launch::async, [&] { act(session, authorize("renew.grant")); });
  ASSERT_TRUE(held.waiting());
  revoked = std::async(std::launch::async, [&] {
    act(session, {{"request_id", "revoke.pending.grant"}, {"action", "live_revoke"}});
  });
  observed = std::async(std::launch::async, [&] {
    return wait_for(session, [](const Json& state) { return state.at("authorization").is_null(); });
  });
  ASSERT_EQ(observed.wait_for(1s), std::future_status::ready);
  EXPECT_TRUE(observed.get().at("authorization").is_null());
  held.release();
  EXPECT_THROW(granted.get(), Error);
  EXPECT_NO_THROW(revoked.get());
  EXPECT_TRUE(session.snapshot().at("authorization").is_null());
  EXPECT_THROW(act(session, submit("after.revocation", "1")), std::invalid_argument);
}
TEST_F(Live, SlowSdkReturnsKeepObservationAndRevocationResponsiveAndRetainLateResults) {
  LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
  ASSERT_EQ(ready(session).at("phase"), "ready");
  act(session, authorize());
  const auto identity = session.snapshot();
  const auto record = identity.at("account_id").get<std::string>();
  const auto policy = identity.at("policy_revision").get<std::string>();
  std::future<void> placed, revoked, cancelled;
  std::future<Json> observed;
  struct Release {
    FakeExchange& exchange;
    ~Release() {
      exchange.call<void (*)(int, int)>("asterion_fake_trader_hold_insert_return", 0, 0);
      exchange.call<void (*)(int)>("asterion_fake_trader_hold_cancel_return", 0);
    }
  } release{exchange};
  exchange.call<void (*)(int, int)>("asterion_fake_trader_hold_insert_return", 1, -7);
  placed = session.execute(record, policy, submit("slow.sdk", "3"));
  observed = std::async(std::launch::async, [&] {
    return wait_for(
        session, [](const Json& state) { return order_has_status(state, "slow.sdk", "accepted"); });
  });
  ASSERT_EQ(observed.wait_for(3s), std::future_status::ready);
  EXPECT_TRUE(order_has_status(observed.get(), "slow.sdk", "accepted"));
  revoked =
      session.execute(record, policy, {{"request_id", "revoke.sdk"}, {"action", "live_revoke"}});
  observed = std::async(std::launch::async, [&] { return session.snapshot(); });
  ASSERT_EQ(observed.wait_for(1s), std::future_status::ready);
  EXPECT_TRUE(observed.get().at("authorization").is_null());
  // A response taking longer than the previous five-second wait is still owned.
  EXPECT_EQ(placed.wait_for(5100ms), std::future_status::timeout);
  exchange.call<void (*)(int, int)>("asterion_fake_trader_hold_insert_return", 0, -7);
  EXPECT_NO_THROW(placed.get()) << "late SDK error cannot replace an actual accepted report";
  EXPECT_NO_THROW(revoked.get());
  exchange.call<void (*)(int)>("asterion_fake_trader_hold_cancel_return", 1);
  cancelled = session.execute(
      record, policy,
      {{"request_id", "cancel.sdk"}, {"action", "cancel"}, {"order_id", "slow.sdk"}});
  observed = std::async(std::launch::async, [&] {
    return wait_for(session, [](const Json& state) {
      return order_has_status(state, "slow.sdk", "cancelled");
    });
  });
  ASSERT_EQ(observed.wait_for(2s), std::future_status::ready);
  EXPECT_TRUE(order_has_status(observed.get(), "slow.sdk", "cancelled"));
  EXPECT_EQ(cancelled.wait_for(0s), std::future_status::timeout);
  session.disconnect();
  EXPECT_EQ(session.snapshot().at("phase"), "disconnected");
  exchange.call<void (*)(int)>("asterion_fake_trader_hold_cancel_return", 0);
  EXPECT_NO_THROW(cancelled.get());
}
TEST_F(Live, ThrottledQuoteWaitAllowsControlAndDisconnectRetiresItWithoutAnOrder) {
  LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
  ASSERT_EQ(ready(session).at("phase"), "ready");
  act(session, authorize());
  const auto identity = session.snapshot();
  const auto record = identity.at("account_id").get<std::string>();
  const auto policy = identity.at("policy_revision").get<std::string>();
  std::future<void> placed, revoked;
  std::future<Json> observed;
  struct Release {
    FakeExchange& exchange;
    ~Release() { exchange.call<void (*)(int, int)>("asterion_fake_trader_reject_quotes", 0, 0); }
  } release{exchange};
  exchange.call<void (*)(int, int)>("asterion_fake_trader_reject_quotes", -2, 100000);
  placed = session.execute(record, policy, submit("waiting.quote", "1"));
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (!exchange.call<int (*)()>("asterion_fake_trader_query_rejections") &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(10ms);
  ASSERT_GT(exchange.call<int (*)()>("asterion_fake_trader_query_rejections"), 0);
  revoked =
      session.execute(record, policy, {{"request_id", "revoke.quote"}, {"action", "live_revoke"}});
  observed = std::async(std::launch::async, [&] { return session.snapshot(); });
  ASSERT_EQ(observed.wait_for(1s), std::future_status::ready);
  EXPECT_TRUE(observed.get().at("authorization").is_null());
  EXPECT_EQ(placed.wait_for(0s), std::future_status::timeout);
  session.disconnect();
  EXPECT_THROW(placed.get(), Error);
  EXPECT_NO_THROW(revoked.get());
  const auto after = session.snapshot();
  EXPECT_TRUE(after.at("orders").empty());
  EXPECT_TRUE(after.at("unconfirmed").empty());
  EXPECT_EQ(after.at("capacity").at("records_used").get<int>(),
            identity.at("capacity").at("records_used").get<int>() + 1);
}
// A submission waiting for its broker quote has recorded nothing, so a cancel of
// an order the account already holds does not wait behind it. A cancel of the
// order that submission is about to place keeps its place in the queue.
TEST_F(Live, CancelOfARecordedOrderRunsWhileASubmissionWaitsForItsQuote) {
  LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
  ASSERT_EQ(ready(session).at("phase"), "ready");
  act(session, authorize());
  act(session, submit("resting", "4"));
  const auto identity = wait_for(
      session, [](const Json& state) { return order_has_status(state, "resting", "accepted"); });
  const auto record = identity.at("account_id").get<std::string>();
  const auto policy = identity.at("policy_revision").get<std::string>();
  const auto cancel = [](const char* request, const char* order) {
    return Json{{"request_id", request}, {"action", "cancel"}, {"order_id", order}};
  };
  std::future<void> placed, premature, cancelled;
  struct Release {
    FakeExchange& exchange;
    ~Release() { exchange.call<void (*)(int, int)>("asterion_fake_trader_reject_quotes", 0, 0); }
  } release{exchange};
  exchange.call<void (*)(int, int)>("asterion_fake_trader_reject_quotes", -2, 100000);
  placed = session.execute(record, policy, submit("waiting", "3"));
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (!exchange.call<int (*)()>("asterion_fake_trader_query_rejections") &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(10ms);
  ASSERT_GT(exchange.call<int (*)()>("asterion_fake_trader_query_rejections"), 0);
  premature = session.execute(record, policy, cancel("cancel.waiting", "waiting"));
  cancelled = session.execute(record, policy, cancel("cancel.resting", "resting"));
  ASSERT_EQ(cancelled.wait_for(3s), std::future_status::ready);
  EXPECT_NO_THROW(cancelled.get());
  EXPECT_EQ(placed.wait_for(0s), std::future_status::timeout);
  EXPECT_EQ(premature.wait_for(0s), std::future_status::timeout);
  EXPECT_EQ(find_order(wait_for(session,
                                [](const Json& state) {
                                  return order_has_status(state, "resting", "cancelled");
                                }),
                       "resting")
                .at("status"),
            "cancelled");
  // The cancellation report changed the basis the waiting submission was
  // admitted on. It is refused as any submission overtaken by a broker report
  // is, and its own cancel then finds nothing to cancel.
  exchange.call<void (*)(int, int)>("asterion_fake_trader_reject_quotes", 0, 0);
  EXPECT_THROW(placed.get(), Error);
  EXPECT_THROW(premature.get(), Error);
  const auto after = session.snapshot();
  EXPECT_TRUE(after.at("unconfirmed").empty());
  EXPECT_EQ(after.at("orders").size(), 1U);
  EXPECT_NO_THROW(act(session, submit("next", "3"))) << "the account keeps trading";
}
TEST_F(Live, QuoteDeadlineAdvancesWithoutPollingWhileTheSdkCallIsBlocked) {
  LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
  ASSERT_EQ(ready(session).at("phase"), "ready");
  act(session, authorize());
  const auto before = session.snapshot();
  struct Release {
    FakeExchange& exchange;
    ~Release() { exchange.call<void (*)(int)>("asterion_fake_trader_hold_quote_return", 0); }
  } release{exchange};
  exchange.call<void (*)(int)>("asterion_fake_trader_hold_quote_return", 1);
  auto placed = session.execute(before.at("account_id").get<std::string>(),
                                before.at("policy_revision").get<std::string>(),
                                submit("expired.quote", "1"));
  const auto deadline = std::chrono::steady_clock::now() + 3s;
  while (!exchange.call<int (*)()>("asterion_fake_trader_quote_return_waiting") &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(10ms);
  ASSERT_TRUE(exchange.call<int (*)()>("asterion_fake_trader_quote_return_waiting"));
  // No snapshot, control request or external polling drives the account here.
  ASSERT_EQ(placed.wait_for(12s), std::future_status::ready);
  EXPECT_THROW(placed.get(), std::invalid_argument);
  EXPECT_TRUE(exchange.call<int (*)()>("asterion_fake_trader_quote_return_waiting"));
  const auto after = session.snapshot();
  EXPECT_TRUE(after.at("orders").empty());
  EXPECT_TRUE(after.at("unconfirmed").empty());
  EXPECT_EQ(after.at("capacity"), before.at("capacity"));
}
TEST_F(Live, ConcurrentRetriesHaveOneDurableIntentAndOneBrokerOrder) {
  LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
  ASSERT_EQ(ready(session).at("phase"), "ready");
  act(session, authorize());
  const auto before = session.snapshot();
  const auto record = before.at("account_id").get<std::string>();
  const auto policy = before.at("policy_revision").get<std::string>();
  const auto command = submit("concurrent", "2");
  std::barrier start(5);
  std::vector<std::future<void>> callers;
  for (int i = 0; i < 4; ++i)
    callers.push_back(std::async(std::launch::async, [&] {
      start.arrive_and_wait();
      session.execute(record, policy, command).get();
      EXPECT_EQ(session.snapshot().at("account_id"), record);
    }));
  start.arrive_and_wait();
  for (auto& caller : callers)
    EXPECT_NO_THROW(caller.get());
  const auto after = wait_for(session, [](const Json& state) {
    return order_has_status(state, "concurrent", "filled") && !state.at("positions").empty();
  });
  EXPECT_EQ(after.at("orders").size(), 1U);
  EXPECT_EQ(after.at("trades").size(), 1U);
  ASSERT_EQ(after.at("positions").size(), 1U);
  EXPECT_EQ(after.at("positions")[0].at("today"), "2");
  session.stop();
  session.stopped().get();
  SqliteJournal journal(directory.path, {"plugins", "ctp-flow"});
  journal.start();
  EXPECT_EQ(journal.command("submit.concurrent").at("command"), command);
  EXPECT_EQ(journal.order_sequence("concurrent"), journal.command_sequence("submit.concurrent"));
}
TEST_F(Live, OrdersPassAuthorizationAllowlistUnitsAndRiskBeforeReachingTheBroker) {
  LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
  EXPECT_EQ(session.snapshot().at("phase"), "disconnected");
  EXPECT_THROW(act(session, submit("early", "1")), std::invalid_argument);
  EXPECT_THROW(act(session, authorize()), Error) << "authorizing needs a ready account";
  ASSERT_EQ(ready(session).at("phase"), "ready");
  EXPECT_THROW(act(session, submit("unauthorized", "1")), std::invalid_argument);
  EXPECT_THROW(act(session, authorize("other", "000002")), std::invalid_argument);
  act(session, authorize());
  EXPECT_FALSE(session.snapshot().at("authorization").is_null());
  EXPECT_THROW(act(session, submit("listed", "1", "3500", "rb2611")), std::invalid_argument);
  EXPECT_THROW(act(session, submit("tick", "1", "3500.5")), std::invalid_argument);
  EXPECT_THROW(act(session, submit("lot", "0.5")), std::invalid_argument);
  try {
    act(session, submit("far", "1", "3600"));
    ADD_FAILURE() << "2.9% from the latest price exceeds the 2% bound";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("deviates"), std::string::npos);
  }
  try {
    act(session, submit("large", "6"));
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

  act(session, submit("filled", "2"));
  auto state = wait_for(session, [](const Json& s) {
    return find_order(s, "filled").is_object() && order_has_status(s, "filled", "filled") &&
           !s.at("trades").empty() && !s.at("positions").empty();
  });
  EXPECT_EQ(find_order(state, "filled").at("status"), "filled");
  ASSERT_EQ(state.at("positions").size(), 1U);
  EXPECT_EQ(state.at("positions")[0].at("today"), "2");
  EXPECT_EQ(state.at("trades").size(), 1U);

  act(session, submit("resting", "3"));
  state = wait_for(session, [](const Json& s) {
    return find_order(s, "resting").is_object() && order_has_status(s, "resting", "accepted");
  });
  EXPECT_EQ(find_order(state, "resting").at("status"), "accepted");
  try {
    act(session, submit("second", "1"));
    ADD_FAILURE() << "one working order is the limit";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("working_orders"), std::string::npos);
  }
  // A retried request is acknowledged without sending again; a reused ID is refused.
  const auto orders = session.snapshot().at("orders").size();
  act(session, submit("resting", "3"));
  EXPECT_EQ(session.snapshot().at("orders").size(), orders);
  EXPECT_THROW(act(session, submit("resting", "4")), Error);

  act(session, {{"request_id", "cancel"}, {"action", "cancel"}, {"order_id", "resting"}});
  state =
      wait_for(session, [](const Json& s) { return order_has_status(s, "resting", "cancelled"); });
  EXPECT_EQ(find_order(state, "resting").at("status"), "cancelled");
  act(session, {{"request_id", "revoke"}, {"action", "live_revoke"}});
  EXPECT_TRUE(session.snapshot().at("authorization").is_null());
  EXPECT_THROW(act(session, submit("revoked", "1")), std::invalid_argument);
  EXPECT_THROW(act(session, authorize()), Error) << "authorization IDs are never reused";
  act(session, authorize("authorize.again"));
  session.disconnect();
  EXPECT_FALSE(session.snapshot().at("authorization").is_null())
      << "a disconnect ends the connection, not the owner's permission";
  try {
    act(session, submit("disconnected", "1"));
    FAIL() << "an order was accepted without a broker connection";
  } catch (const Error& error) {
    EXPECT_EQ(error.code(), ErrorCode::unavailable);
  }
}
TEST_F(Live, LimitPricesStayWithinExchangeLimitsAndTheMarketReference) {
  auto wide = manifest(2, "0.1");
  wide["policy"]["contracts"] = Json::array(
      {contract("rb2610"), contract("rb2611", "rb", "2026-11"), contract("zz2610", "zz")});
  LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, wide);
  ASSERT_EQ(ready(session).at("phase"), "ready");
  act(session, authorize());
  const auto refused = [&](Json command, const char* reason) {
    try {
      act(session, command);
      ADD_FAILURE() << command.dump();
    } catch (const std::invalid_argument& error) {
      EXPECT_NE(std::string(error.what()).find(reason), std::string::npos) << error.what();
    }
  };
  // Within 10% of 3500, but above the exchange's upper limit of 3700.
  refused(submit("limit", "1", "3750"), "price limits");
  refused(submit("quiet", "1", "3500", "zz2610"), "no current market price");
  // rb2611 has not traded: the pre-settlement price 3490 is the reference.
  act(session, submit("settled", "1", "3500", "rb2611"));
  ASSERT_FALSE(wait_for(session, [](const Json& s) { return !s.at("positions").empty(); })
                   .at("positions")
                   .empty());
  act(session, submit("near", "1", "3690"));
  EXPECT_EQ(wait_for(session, [](const Json& state) { return state.at("orders").size() == 2; })
                .at("orders")
                .size(),
            2U);
  EXPECT_EQ(session.snapshot().at("max_price_deviation"), "0.1");
}
TEST_F(Live, RecoveryAttributesRecordedOrdersAndNeverResendsThem) {
  {
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest(2));
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    act(session, submit("resting", "3"));
    ASSERT_EQ(find_order(wait_for(session,
                                  [](const Json& s) {
                                    return order_has_status(s, "resting", "accepted");
                                  }),
                         "resting")
                  .at("status"),
              "accepted");
  }
  LiveSession restored(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
  EXPECT_TRUE(restored.snapshot().at("authorization").is_null());
  EXPECT_THROW(act(restored, submit("after", "1")), std::invalid_argument);
  auto state = ready(restored);
  state = wait_for(restored, [](const Json& s) { return find_order(s, "resting").is_object(); });
  ASSERT_TRUE(find_order(state, "resting").is_object()) << "broker report keeps its order ID";
  EXPECT_TRUE(state.at("unconfirmed").empty());
  const auto orders = state.at("orders").size();
  act(restored, submit("resting", "3"));
  EXPECT_EQ(restored.snapshot().at("orders").size(), orders) << "never resent after recovery";
  EXPECT_THROW(act(restored, authorize()), Error);
}
TEST_F(Live, ReusedBrokerKeyDoesNotConfirmAnOlderUnknownIntent) {
  const auto key =
      "1:" + std::to_string(exchange.call<int (*)()>("asterion_fake_trader_next_session")) + ":1";
  std::string policy;
  {
    LiveSession initial(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest(2));
    policy = initial.snapshot().at("policy_revision");
  }
  const auto unknown = submit("z.unknown", "1");
  {
    SqliteJournal journal(directory.path, {"plugins", "ctp-flow"});
    journal.start();
    journal.append({{"command", unknown},
                    {"policy_revision", policy},
                    {"broker_key", key},
                    {"trading_day", "20260928"}},
                   unknown.at("request_id").get<std::string>(), "z.unknown");
  }
  {
    LiveSession restored(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
    ASSERT_EQ(ready(restored).at("unconfirmed").size(), 1U);
    act(restored, authorize());
    act(restored, submit("a.replacement", "3"));
    const auto state = wait_for(restored, [](const Json& value) {
      return order_has_status(value, "a.replacement", "accepted");
    });
    ASSERT_TRUE(order_has_status(state, "a.replacement", "accepted"));
    EXPECT_EQ(find_order(state, "a.replacement").at("broker_key"), key);
    ASSERT_EQ(state.at("unconfirmed").size(), 1U);
    EXPECT_EQ(state.at("unconfirmed")[0].at("id"), "z.unknown");
    EXPECT_THROW(act(restored, submit("over.limit", "1")), std::invalid_argument);
  }
  // Lexical order is the opposite of durable order. Recovery must keep the
  // later broker attribution and leave the older intent unknown.
  {
    LiveSession again(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
    const auto state = ready(again);
    ASSERT_TRUE(order_has_status(state, "a.replacement", "accepted"));
    ASSERT_EQ(state.at("unconfirmed").size(), 1U);
    EXPECT_EQ(state.at("unconfirmed")[0].at("id"), "z.unknown");
    exchange.call<void (*)(int, int)>("asterion_fake_trader_fill", 0, 3);
    ASSERT_TRUE(order_has_status(wait_for(again,
                                          [](const Json& value) {
                                            return order_has_status(value, "a.replacement",
                                                                    "filled");
                                          }),
                                 "a.replacement", "filled"));
  }
  // The later intent is now retired from memory. Restore its identity before
  // using a terminal report as evidence about the older unresolved intent.
  LiveSession closed(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
  const auto state = ready(closed);
  ASSERT_TRUE(order_has_status(state, "a.replacement", "filled"));
  ASSERT_EQ(state.at("unconfirmed").size(), 1U);
  EXPECT_EQ(state.at("unconfirmed")[0].at("id"), "z.unknown");
}
TEST_F(Live, RecordedOrdersTheBrokerDoesNotReportCountAsWorkingExposure) {
  {
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    act(session, submit("lost", "3"));
  }
  // The exchange forgets the order, as if it never arrived.
  exchange.reset();
  LiveSession restored(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
  const auto state = ready(restored);
  ASSERT_EQ(state.at("unconfirmed").size(), 1U);
  EXPECT_EQ(state.at("unconfirmed")[0].at("id"), "lost");
  EXPECT_THROW(act(restored, policy_command(restored, manifest().at("policy"))), Error);
  EXPECT_TRUE(state.at("orders").empty()) << "the unconfirmed order is not resent";
  act(restored, authorize("authorize.restored"));
  EXPECT_THROW(act(restored, submit("next", "1")), std::invalid_argument)
      << "the unconfirmed order still occupies the working-order limit";
  const auto resolve = [](std::string id, std::string order) {
    return Json{{"request_id", std::move(id)}, {"action", "live_resolve"}, {"order_id", order}};
  };
  EXPECT_THROW(act(restored, resolve("resolve.unknown", "missing")), std::invalid_argument);
  // The owner verified at the broker that "lost" does not exist.
  act(restored, resolve("resolve.lost", "lost"));
  EXPECT_TRUE(restored.snapshot().at("unconfirmed").empty());
  act(restored, resolve("resolve.lost", "lost")); // retried request: acknowledged
  EXPECT_THROW(act(restored, resolve("resolve.again", "lost")), std::invalid_argument)
      << "a resolved order is no longer unconfirmed";
  act(restored, submit("next", "1"));
  const auto confirmed =
      wait_for(restored, [](const Json& state) { return find_order(state, "next").is_object(); });
  EXPECT_TRUE(find_order(confirmed, "next").is_object());
}
TEST_F(Live, SdkFailureWithoutBrokerReportsKeepsAnUnknownIntentAndNoInventedOrder) {
  LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
  ASSERT_EQ(ready(session).at("phase"), "ready");
  act(session, authorize());
  exchange.call<void (*)(int)>("asterion_fake_trader_cancel_code", -2);
  EXPECT_THROW(act(session, submit("unknown", "3")), Error);
  exchange.call<void (*)(int)>("asterion_fake_trader_cancel_code", 0);
  const auto state = session.snapshot();
  EXPECT_TRUE(state.at("orders").empty()) << "an SDK return is not a broker order report";
  ASSERT_EQ(state.at("unconfirmed").size(), 1U);
  EXPECT_EQ(state.at("unconfirmed")[0].at("id"), "unknown");
  EXPECT_THROW(act(session, submit("next", "1")), std::invalid_argument)
      << "the unknown intent still occupies the account risk budget";
  const auto effects = exchange.call<int (*)()>("asterion_fake_catalog_side_effects");
  EXPECT_NO_THROW(act(session, submit("unknown", "3")));
  EXPECT_EQ(exchange.call<int (*)()>("asterion_fake_catalog_side_effects"), effects);
}
TEST_F(Live, DurableNotSentEvidenceAcrossPagesReleasesIntentWithoutResendingOnRecovery) {
  std::string revision;
  {
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    revision = session.snapshot().at("policy_revision").get<std::string>();
  }
  // Recovery fixture: a committed intent followed by a definite pre-SDK refusal.
  const auto command = submit("not.sent", "3");
  {
    SqliteJournal journal(directory.path, {"ctp-flow", "plugins", "archives"});
    journal.start();
    // Put the intent at the end of one page and its result on the next page.
    for (std::size_t i = 1; i < SqliteJournal::page_size - 1; ++i) {
      const auto id = "revoke." + std::to_string(i);
      journal.append({{"command", {{"request_id", id}, {"action", "live_revoke"}}},
                      {"policy_revision", revision}},
                     id);
    }
    journal.append({{"command", command},
                    {"policy_revision", revision},
                    {"broker_key", "1:99:1"},
                    {"trading_day", "20260928"}},
                   command.at("request_id").get<std::string>(), "not.sent");
    journal.append({{"order_not_sent", "not.sent"}, {"error_code", -1005}}, {}, {}, "not.sent");
  }
  LiveSession restored(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
  const auto state = ready(restored);
  EXPECT_TRUE(state.at("unconfirmed").empty());
  EXPECT_TRUE(state.at("orders").empty());
  act(restored, command);
  EXPECT_TRUE(restored.snapshot().at("orders").empty()) << "retry only acknowledges the intent";
  act(restored, authorize());
  auto reused = command;
  reused["request_id"] = "different.request";
  EXPECT_THROW(act(restored, reused), Error) << "a different request cannot reuse an old order ID";
  EXPECT_NO_THROW(act(restored, submit("next", "3"))) << "unsent intent occupies no working slot";
}
TEST_F(Live, ResolvedOrdersStayResolvedAcrossRecovery) {
  {
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    act(session, submit("lost", "3"));
  }
  exchange.reset();
  {
    LiveSession restored(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
    ASSERT_EQ(ready(restored).at("unconfirmed").size(), 1U);
    act(restored, {{"request_id", "resolve"}, {"action", "live_resolve"}, {"order_id", "lost"}});
  }
  LiveSession again(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
  EXPECT_TRUE(ready(again).at("unconfirmed").empty());
}
TEST_F(Live, MissingOrderIdentityIndexRejectsRecoveryWithoutChangingEvidence) {
  std::string revision;
  {
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    revision = session.snapshot().at("policy_revision");
  }
  const auto command = submit("missing.index", "3");
  {
    SqliteJournal journal(directory.path, {"ctp-flow", "plugins"});
    journal.start();
    journal.append({{"command", command},
                    {"policy_revision", revision},
                    {"broker_key", "1:99:1"},
                    {"trading_day", "20260928"}},
                   command.at("request_id").get<std::string>());
  }
  const auto digest = sha256_file(directory.path / "journal.sqlite");
  EXPECT_THROW((LiveSession{directory.path, ASTERION_TEST_CTP_TRADER, owners.path}),
               std::invalid_argument);
  EXPECT_EQ(sha256_file(directory.path / "journal.sqlite"), digest);
}
TEST_F(Live, CredentialsAreNeverWrittenAndHeadersPinTheEngine) {
  {
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
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
  EXPECT_EQ(header.at("engine"), "asterion.live-futures.v28");
  EXPECT_EQ(header.at("manifest"), manifest());
  header["engine"] = "asterion.live-futures.v4";
  test::write_record(header_file, header);
  const auto before = sha256_file(directory.path / "journal.sqlite");
  EXPECT_THROW((LiveSession{directory.path, ASTERION_TEST_CTP_TRADER, owners.path}),
               std::invalid_argument);
  EXPECT_EQ(sha256_file(directory.path / "journal.sqlite"), before);
}
TEST_F(Live, InvalidInputsAndMissingSdkWriteNothing) {
  auto bad = manifest();
  bad["broker"]["front"] = "http://127.0.0.1:41205";
  EXPECT_THROW((LiveSession{directory.path, ASTERION_TEST_CTP_TRADER, owners.path, bad}),
               std::invalid_argument);
  bad = manifest();
  bad["policy"]["contracts"].push_back(contract("rb2610"));
  EXPECT_THROW((LiveSession{directory.path, ASTERION_TEST_CTP_TRADER, owners.path, bad}),
               std::invalid_argument);
  EXPECT_THROW(
      (LiveSession{directory.path, directory.path / "missing.dylib", owners.path, manifest()}),
      Error);
  EXPECT_EQ(test::journal_size(directory.path), 0U);
  EXPECT_FALSE(fs::exists(directory.path / "plugins"));
}
TEST_F(Live, StrategyRunOwnsTheAccountAndItsTargetsTakeTheOrderPath) {
  FakeMarket market;
  LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
  const auto day = ready(session).at("trading_day").get<std::string>();
  // A rising close asks for the position; the bar after it is still forming.
  market.set(day, {"3500", "3501", "3501"});
  EXPECT_THROW(act(session, market.start("early", "1")), std::invalid_argument)
      << "a run lives inside the owner's authorization";
  act(session, authorize());
  auto wrong = market.start("wrong", "1");
  wrong["symbol"] = "rb2611";
  EXPECT_THROW(act(session, wrong), std::invalid_argument) << "only an allowed contract";
  act(session, market.start("run", "1"));
  EXPECT_THROW(act(session, market.start("second", "1")), Error);
  const auto bought = FakeMarket::order("run", 1);
  auto state =
      wait_for(session, [&](const Json& s) { return order_has_status(s, bought, "filled"); });
  const auto order = find_order(state, bought);
  ASSERT_TRUE(order.is_object()) << state.at("strategy").dump();
  EXPECT_EQ(order.at("side"), "buy");
  EXPECT_EQ(order.at("offset"), "open");
  EXPECT_EQ(order.at("quantity"), "1");
  EXPECT_EQ(order.at("limit_price"), "3501") << "the deciding bar's close";
  EXPECT_EQ(state.at("strategy").at("state"), "running");
  EXPECT_EQ(state.at("strategy").at("target"), "1");
  try {
    act(session, submit("manual", "1"));
    FAIL() << "a manual order was accepted while a strategy controls the account";
  } catch (const Error& error) {
    EXPECT_EQ(error.code(), ErrorCode::conflict);
  }
  EXPECT_TRUE(find_order(session.snapshot(), "manual").is_null());
  // The run and the authorization outlive a reconnect: nobody confirms again.
  exchange.call<void (*)()>("asterion_fake_trader_reconnect");
  ASSERT_NE(wait_for(session, [](const Json& s) { return s.at("phase") != "ready"; }).at("phase"),
            "ready");
  state = wait_for(session, [](const Json& s) { return s.at("phase") == "ready"; });
  ASSERT_EQ(state.at("phase"), "ready");
  EXPECT_EQ(state.at("strategy").at("state"), "running");
  // A falling close asks for no position: today's lot is closed.
  market.set(day, {"3500", "3501", "3499", "3499"});
  const auto sold = FakeMarket::order("run", 2, ".today");
  state = wait_for(session, [&](const Json& s) {
    return order_has_status(s, sold, "filled") && long_position(s) == Decimal{};
  });
  ASSERT_TRUE(order_has_status(state, sold, "filled")) << state.at("strategy").dump();
  EXPECT_EQ(find_order(state, sold).at("offset"), "close_today");
  EXPECT_EQ(long_position(state), Decimal{});
  act(session, stop_strategy("stop"));
  state = session.snapshot();
  EXPECT_TRUE(strategy_stopped(state));
  EXPECT_FALSE(state.at("authorization").is_null()) << "stopping a strategy is not a revoke";
  act(session, submit("manual", "1"));
  // An accepted order is listed once the broker reports it.
  state = wait_for(session, [](const Json& s) { return find_order(s, "manual").is_object(); });
  EXPECT_TRUE(find_order(state, "manual").is_object());
}
TEST_F(Live, StrategyRunSellsShortAndReversesByClosingBeforeOpening) {
  FakeMarket market;
  LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
  const auto day = ready(session).at("trading_day").get<std::string>();
  act(session, authorize());
  // A falling close asks for a short position.
  market.set(day, {"3500", "3499", "3499"});
  act(session, market.start("run", "1", "both"));
  const auto sold = FakeMarket::order("run", 1);
  auto state =
      wait_for(session, [&](const Json& s) { return order_has_status(s, sold, "filled"); });
  ASSERT_TRUE(order_has_status(state, sold, "filled")) << state.at("strategy").dump();
  EXPECT_EQ(find_order(state, sold).at("side"), "sell");
  EXPECT_EQ(find_order(state, sold).at("offset"), "open");
  EXPECT_EQ(find_order(state, sold).at("limit_price"), "3499");
  EXPECT_EQ(state.at("strategy").at("target"), "-1");
  EXPECT_EQ(state.at("strategy").at("strategy").at("sides"), "both");
  state = wait_for(session, [](const Json& s) { return short_position(s) == Decimal::parse("1"); });
  EXPECT_EQ(short_position(state), Decimal::parse("1"));
  // A rising close asks for a long one. That bar only buys the short back.
  market.set(day, {"3500", "3499", "3501", "3501"});
  const auto covered = FakeMarket::order("run", 2, ".today");
  state = wait_for(session, [&](const Json& s) {
    return order_has_status(s, covered, "filled") && short_position(s) == Decimal{};
  });
  ASSERT_TRUE(order_has_status(state, covered, "filled")) << state.at("strategy").dump();
  EXPECT_EQ(find_order(state, covered).at("side"), "buy");
  EXPECT_EQ(find_order(state, covered).at("offset"), "close_today");
  EXPECT_EQ(state.at("strategy").at("target"), "1");
  EXPECT_EQ(long_position(state), Decimal{}) << "the two sides are never held together";
  EXPECT_TRUE(find_order(state, FakeMarket::order("run", 2)).is_null());
  // The next bar still asks for the long position; flat now, it opens.
  market.set(day, {"3500", "3499", "3501", "3502", "3502"});
  const auto bought = FakeMarket::order("run", 3);
  state = wait_for(session, [&](const Json& s) {
    return order_has_status(s, bought, "filled") && long_position(s) == Decimal::parse("1");
  });
  ASSERT_TRUE(order_has_status(state, bought, "filled")) << state.at("strategy").dump();
  EXPECT_EQ(find_order(state, bought).at("side"), "buy");
  EXPECT_EQ(find_order(state, bought).at("offset"), "open");
  EXPECT_EQ(short_position(state), Decimal{});
  act(session, stop_strategy("stop"));
  EXPECT_TRUE(strategy_stopped(session.snapshot()));
}
TEST_F(Live, StrategyTakesOverOneSideAndRefusesAContractHeldOnBoth) {
  FakeMarket market;
  LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
  const auto day = ready(session).at("trading_day").get<std::string>();
  act(session, authorize());
  auto sell = submit("short", "1");
  sell["side"] = "sell";
  act(session, sell);
  auto state =
      wait_for(session, [](const Json& s) { return short_position(s) == Decimal::parse("1"); });
  ASSERT_EQ(short_position(state), Decimal::parse("1"));
  // The owner's short position is the run's to manage: a long-only run buys it back.
  market.set(day, {"3500", "3501", "3501"});
  act(session, market.start("run", "1", "long"));
  const auto covered = FakeMarket::order("run", 1, ".today");
  state = wait_for(session, [&](const Json& s) {
    return order_has_status(s, covered, "filled") && short_position(s) == Decimal{};
  });
  ASSERT_TRUE(order_has_status(state, covered, "filled")) << state.at("strategy").dump();
  EXPECT_EQ(find_order(state, covered).at("offset"), "close_today");
  act(session, stop_strategy("stop"));
  // Both sides at once are not one position for a target to be measured against.
  act(session, submit("long", "1"));
  // Opening pauses until the broker's positions include the last fill.
  state = wait_for(session, [](const Json& s) { return long_position(s) == Decimal::parse("1"); });
  ASSERT_EQ(long_position(state), Decimal::parse("1"));
  sell = submit("short.again", "1");
  sell["side"] = "sell";
  act(session, sell);
  state = wait_for(session, [](const Json& s) {
    return long_position(s) == Decimal::parse("1") && short_position(s) == Decimal::parse("1");
  });
  ASSERT_EQ(short_position(state), Decimal::parse("1"));
  try {
    act(session, market.start("both", "1", "both"));
    FAIL() << "a run started on a contract held on both sides";
  } catch (const Error& error) {
    EXPECT_EQ(error.code(), ErrorCode::conflict);
  }
}
TEST_F(Live, ARunTradesByTheRuleItWasStartedWith) {
  FakeMarket market;
  LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
  const auto day = ready(session).at("trading_day").get<std::string>();
  act(session, authorize());
  market.set(day, {"3500", "3501", "3501"});
  // Windows that make no strategy are refused before a run exists.
  EXPECT_THROW(act(session, market.start("bad", "1", "both",
                                         {{"kind", "breakout"}, {"entry", 1}, {"exit", 2}})),
               std::invalid_argument);
  // So is a rule that ranks several contracts: a run trades one.
  auto ranked = market.start("ranked", "1");
  ranked["strategy"] = {{"sides", "both"},
                        {"rule",
                         {{"kind", "cross_momentum"},
                          {"reverse", false},
                          {"lookback", 1},
                          {"rebalance", 1},
                          {"count", 1},
                          {"notional", "1000"}}}};
  EXPECT_THROW(act(session, ranked), std::invalid_argument);
  EXPECT_TRUE(session.snapshot().at("strategy").is_null());
  // Momentum over one bar: 3501 is above the close before it.
  const Json rule{{"kind", "momentum"}, {"lookback", 1}};
  act(session, market.start("run", "1", "both", rule));
  const auto bought = FakeMarket::order("run", 1);
  const auto state =
      wait_for(session, [&](const Json& s) { return order_has_status(s, bought, "filled"); });
  ASSERT_TRUE(order_has_status(state, bought, "filled")) << state.at("strategy").dump();
  EXPECT_EQ(find_order(state, bought).at("side"), "buy");
  EXPECT_EQ(find_order(state, bought).at("offset"), "open");
  EXPECT_EQ(state.at("strategy").at("strategy").at("rule"), rule);
  EXPECT_EQ(state.at("strategy").at("warmup"), 2);
  act(session, stop_strategy("stop"));
}
TEST_F(Live, StrategyReplacesItsWorkingOrderAndStoppingRequestsItsCancellation) {
  FakeMarket market;
  LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
  const auto day = ready(session).at("trading_day").get<std::string>();
  act(session, authorize());
  // Three lots rest at the fake exchange.
  market.set(day, {"3500", "3501", "3501"});
  act(session, market.start("run", "3"));
  const auto first = FakeMarket::order("run", 1), second = FakeMarket::order("run", 2);
  auto state =
      wait_for(session, [&](const Json& s) { return order_has_status(s, first, "accepted"); });
  ASSERT_TRUE(order_has_status(state, first, "accepted")) << state.at("strategy").dump();
  // The next bar asks again: the unfilled order leaves before its successor
  // enters, so the run never holds two orders.
  market.set(day, {"3500", "3501", "3502", "3502"});
  state = wait_for(session, [&](const Json& s) { return order_has_status(s, second, "accepted"); });
  ASSERT_TRUE(order_has_status(state, second, "accepted")) << state.at("strategy").dump();
  EXPECT_TRUE(order_has_status(state, first, "cancelled"));
  EXPECT_EQ(find_order(state, second).at("limit_price"), "3502");
  EXPECT_EQ(state.at("strategy").at("orders"), Json::array({second}));
  act(session, stop_strategy("stop"));
  state =
      wait_for(session, [&](const Json& s) { return order_has_status(s, second, "cancelled"); });
  EXPECT_TRUE(order_has_status(state, second, "cancelled"));
  EXPECT_TRUE(strategy_stopped(state));
  EXPECT_EQ(long_position(state), Decimal{}) << "stopping cancels orders, never trades";
}
TEST_F(Live, StrategyContinuesIntoTheNextTradingDayAndClosesYesterdaysLot) {
  FakeMarket market;
  LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
  const auto day = ready(session).at("trading_day").get<std::string>();
  act(session, authorize());
  market.set(day, {"3500", "3501", "3501"});
  act(session, market.start("run", "1"));
  auto state =
      wait_for(session, [](const Json& s) { return long_position(s) == Decimal::parse("1"); });
  ASSERT_EQ(long_position(state), Decimal::parse("1")) << state.at("strategy").dump();
  // The account enters the next trading day before the market's first bar of
  // it: the run waits, and nobody authorizes or starts anything again.
  exchange.call<void (*)(const char*)>("asterion_fake_trader_next_day", "20260929");
  state = wait_for(session, [](const Json& s) {
    return s.at("phase") == "ready" && s.at("trading_day") == "20260929";
  });
  ASSERT_EQ(state.at("trading_day"), "20260929");
  EXPECT_EQ(state.at("strategy").at("state"), "running");
  EXPECT_TRUE(state.at("orders").empty());
  // The new day's first whole bar closes lower: the lot bought yesterday is closed.
  market.set("20260929", {"3499", "3499"}, false, 10);
  const auto sold = FakeMarket::order("run", 10, ".yesterday");
  state = wait_for(session, [&](const Json& s) {
    return order_has_status(s, sold, "filled") && long_position(s) == Decimal{};
  });
  ASSERT_TRUE(order_has_status(state, sold, "filled")) << state.at("strategy").dump();
  EXPECT_EQ(find_order(state, sold).at("offset"), "close_yesterday");
  EXPECT_EQ(state.at("strategy").at("state"), "running");
}
TEST_F(Live, StrategyRunEndsWithARevokeOrAFailedStepAndIsNeverRestored) {
  FakeMarket market;
  {
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    const auto day = ready(session).at("trading_day").get<std::string>();
    act(session, authorize());
    market.set(day, {"3500", "3501", "3501"});
    act(session, market.start("resting", "3"));
    const auto resting = FakeMarket::order("resting", 1);
    wait_for(session, [&](const Json& s) { return order_has_status(s, resting, "accepted"); });
    // A revoke ends the run and requests nothing: cancelling stays the owner's.
    act(session, Json{{"request_id", "revoke"}, {"action", "live_revoke"}});
    auto state = session.snapshot();
    EXPECT_TRUE(strategy_stopped(state));
    EXPECT_TRUE(order_has_status(state, resting, "accepted"));
    act(session, authorize("again"));
    EXPECT_THROW(act(session, market.start("blocked", "1")), Error)
        << "a run starts without working orders";
    act(session, Json{{"request_id", "cancel"}, {"action", "cancel"}, {"order_id", resting}});
    wait_for(session, [&](const Json& s) { return order_has_status(s, resting, "cancelled"); });
    // Six lots exceed the order limit: the step fails and the run ends.
    act(session, market.start("refused", "6"));
    state = wait_for(session, strategy_stopped);
    ASSERT_TRUE(strategy_stopped(state));
    EXPECT_NE(state.at("strategy").at("reason").get<std::string>().find("risk"), std::string::npos);
    EXPECT_TRUE(find_order(state, FakeMarket::order("refused", 1)).is_null());
    // So does an order the exchange refuses: the next bar does not try again.
    exchange.call<void (*)(int)>("asterion_fake_trader_reject_inserts", 1);
    act(session, market.start("unwanted", "1"));
    state = wait_for(session, strategy_stopped);
    exchange.call<void (*)(int)>("asterion_fake_trader_reject_inserts", 0);
    EXPECT_NE(state.at("strategy").at("reason").get<std::string>().find("rejected a strategy"),
              std::string::npos)
        << state.at("strategy").dump();
    EXPECT_EQ(long_position(state), Decimal{});
    // Lost observations end the run as well; bars are never invented.
    market.set(day, {"3500", "3501", "3501"}, true);
    act(session, market.start("blind", "1"));
    state = wait_for(session, strategy_stopped);
    EXPECT_NE(state.at("strategy").at("reason").get<std::string>().find("interrupted"),
              std::string::npos);
    EXPECT_EQ(long_position(state), Decimal{});
  }
  LiveSession recovered(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
  EXPECT_TRUE(recovered.snapshot().at("strategy").is_null());
}
TEST(LiveProtocol, SnapshotAndCommandsRoundTrip) {
  Json snapshot{
      {"account_id", "fixture-record"},
      {"policy_revision", "fixture-policy"},
      {"risk_artifact", std::string(64, 'a')},
      {"segment_count", std::uint64_t{0}},
      {"broker", manifest().at("broker")},
      {"risk", manifest().at("policy").at("risk")},
      {"max_price_deviation", "0.02"},
      {"contracts", manifest().at("policy").at("contracts")},
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
      {"authorization", {{"authorized_at_ms", 7}}},
      {"strategy",
       {{"id", "run"},
        {"venue", "SHFE"},
        {"symbol", "rb2610"},
        {"strategy",
         {{"quantity", "1"},
          {"sides", "both"},
          {"rule", {{"kind", "moving_average"}, {"fast", 5}, {"slow", 20}}}}},
        {"warmup", 20},
        {"state", "running"},
        {"reason", ""},
        {"started_ms", 8},
        {"bars", 21},
        {"bar_ms", 1790000040000},
        {"target", "1"},
        {"orders", Json::array({"run.1790000040000"})}}},
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
      {"storage_state", "ready"},
      {"capacity",
       {{"records_used", 12},
        {"records_limit", 100001},
        {"bytes_used", 2048},
        {"bytes_limit", 268435456}}}};
  auto expected = snapshot;
  expected["mode"] = "live";
  EXPECT_EQ(protocol::decode_live_snapshot(protocol::encode_live_snapshot(snapshot)), expected);
  auto missing_capacity = protocol::encode_live_snapshot(snapshot);
  missing_capacity.clear_capacity();
  EXPECT_THROW(protocol::decode_live_snapshot(missing_capacity), std::invalid_argument);
  auto invalid_capacity = protocol::encode_live_snapshot(snapshot);
  invalid_capacity.mutable_capacity()->set_records_used(100002);
  EXPECT_THROW(protocol::decode_live_snapshot(invalid_capacity), std::invalid_argument);
  for (const auto& command :
       {authorize(), Json{{"request_id", "r"}, {"action", "live_revoke"}},
        Json{{"request_id", "s"}, {"action", "live_resolve"}, {"order_id", "o"}},
        FakeMarket().start("run", "1"), stop_strategy("stop")})
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

TEST_F(Live, AuthorizationOutlivesAReconnectAndEndsOnlyWhenRevoked) {
  LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
  ASSERT_EQ(ready(session).at("phase"), "ready");
  act(session, authorize());
  ctp::SharedLibrary reconnect(ASTERION_TEST_CTP_TRADER, "asterion_fake_trader_reconnect");
  reconnect.symbol<void (*)()>()();
  // Nothing is sent while the new connection synchronizes; the permission stays.
  auto state = wait_for(session, [](const Json& s) { return s.at("phase") != "ready"; });
  ASSERT_NE(state.at("phase"), "ready");
  EXPECT_FALSE(state.at("authorization").is_null());
  try {
    act(session, submit("unsynchronized", "1"));
    FAIL() << "an order was accepted before the new connection synchronized";
  } catch (const Error& error) {
    // Refused as unsynchronized, or as admitted under a control revision that
    // the reconnect advanced before the order ran. Neither sends anything.
    EXPECT_TRUE(error.code() == ErrorCode::unavailable || error.code() == ErrorCode::conflict)
        << error.what();
  }
  ASSERT_EQ(wait_for(session, [](const Json& s) { return s.at("phase") == "ready"; }).at("phase"),
            "ready");
  EXPECT_NO_THROW(act(session, submit("same.authorization", "1")));
  act(session, Json{{"request_id", "revoke"}, {"action", "live_revoke"}});
  EXPECT_TRUE(session.snapshot().at("authorization").is_null());
  EXPECT_THROW(act(session, submit("revoked", "1")), std::invalid_argument);
}

TEST_F(Live, TradingDayRolloverRebuildsReportsAndRatesWithoutResending) {
  LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
  ASSERT_EQ(ready(session).at("phase"), "ready");
  act(session, authorize());
  act(session, submit("day0.filled", "1"));
  ASSERT_FALSE(wait_for(session, [](const Json& s) { return !s.at("positions").empty(); })
                   .at("positions")
                   .empty());
  act(session, submit("day0.resting", "3"));
  session.query_costs();
  ASSERT_EQ(wait_for(session,
                     [](const Json& s) {
                       return !s.at("costs").empty() && s.at("costs")[0].at("state") == "ready";
                     })
                .at("costs")[0]
                .at("state"),
            "ready");
  ctp::SharedLibrary rollover(ASTERION_TEST_CTP_TRADER, "asterion_fake_trader_next_day");
  int index = 0;
  for (const auto* day : {"20260929", "20260930"}) {
    SCOPED_TRACE(day);
    rollover.symbol<void (*)(const char*)>()(day);
    const auto state = wait_for(session, [&](const Json& s) {
      return s.at("phase") == "ready" && s.at("trading_day") == day &&
             s.at("unconfirmed").size() == (index == 0 ? 1U : 0U);
    });
    ASSERT_EQ(state.at("trading_day"), day);
    ASSERT_EQ(state.at("phase"), "ready");
    EXPECT_FALSE(state.at("authorization").is_null())
        << "a new trading day asks for no new authorization";
    EXPECT_TRUE(state.at("orders").empty());
    EXPECT_TRUE(state.at("trades").empty());
    EXPECT_TRUE(state.at("costs").empty());
    if (index == 0) {
      EXPECT_EQ(state.at("unconfirmed")[0].at("id"), "day0.resting");
      EXPECT_EQ(state.at("unconfirmed")[0].at("trading_day"), "20260928");
    }
    ASSERT_EQ(state.at("positions").size(), 1U);
    EXPECT_EQ(state.at("positions")[0].at("today"), "0");
    EXPECT_EQ(state.at("positions")[0].at("yesterday"), std::to_string(index + 1));
    act(session, submit("day0.filled", "1")); // historical request: acknowledge only
    EXPECT_TRUE(session.snapshot().at("orders").empty());
    if (++index == 1) {
      EXPECT_THROW(act(session, submit("still.unknown", "1")), std::invalid_argument);
      act(session, {{"request_id", "resolve.yesterday"},
                    {"action", "live_resolve"},
                    {"order_id", "day0.resting"}});
    }
    const auto id = "day" + std::to_string(index) + ".filled";
    act(session, submit(id, "1"));
    const auto filled = wait_for(session, [&](const Json& s) {
      return !s.at("trades").empty() && s.at("trades").back().at("order_id") == id;
    });
    ASSERT_EQ(filled.at("trades").size(), 1U);
    EXPECT_EQ(filled.at("trades")[0].at("order_id"), id);
    EXPECT_EQ(filled.at("trades")[0].at("trading_day"), day);
  }
}
TEST_F(Live, TerminalEvidenceSurvivesSegmentsAndRestoresCurrentDayAttribution) {
  test::SmallJournalSegments budget(1);
  {
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    act(session, submit("finished", "1"));
    ASSERT_TRUE(order_has_status(
        wait_for(session,
                 [](const Json& state) { return order_has_status(state, "finished", "filled"); }),
        "finished", "filled"));
  }
  {
    LiveSession restored(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
    ASSERT_EQ(ready(restored).at("phase"), "ready");
    const auto state = wait_for(
        restored, [](const Json& value) { return order_has_status(value, "finished", "filled"); });
    ASSERT_TRUE(order_has_status(state, "finished", "filled"));
    EXPECT_TRUE(state.at("unconfirmed").empty());
    ASSERT_FALSE(state.at("trades").empty());
    EXPECT_EQ(state.at("trades")[0].at("order_id"), "finished");
    const auto effects = exchange.call<int (*)()>("asterion_fake_catalog_side_effects");
    act(restored, submit("finished", "1"));
    EXPECT_EQ(exchange.call<int (*)()>("asterion_fake_catalog_side_effects"), effects);
  }
  exchange.call<void (*)(const char*)>("asterion_fake_trader_next_day", "20260929");
  LiveSession next_day(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
  const auto state = ready(next_day);
  EXPECT_TRUE(state.at("unconfirmed").empty());
  EXPECT_TRUE(state.at("orders").empty());
  EXPECT_GE(state.at("segment_count").get<std::uint64_t>(), 2U);
}
TEST_F(Live, FailedTerminalCommitRetainsUnknownRiskAcrossRolloverAndRecovery) {
  {
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    act(session, submit("unsettled", "3"));
    ASSERT_TRUE(order_has_status(wait_for(session,
                                          [](const Json& state) {
                                            return order_has_status(state, "unsettled", "accepted");
                                          }),
                                 "unsettled", "accepted"));
    sqlite::fail_next_commits_for_testing(1);
    exchange.call<void (*)(int, int)>("asterion_fake_trader_fill", 0, 3);
    const auto failed = wait_for(session, [](const Json& state) {
      return state.at("storage_state") == "recovery_required";
    });
    ASSERT_EQ(failed.at("storage_state"), "recovery_required");
  }
  exchange.call<void (*)(const char*)>("asterion_fake_trader_next_day", "20260929");
  LiveSession restored(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
  const auto state = ready(restored);
  ASSERT_EQ(state.at("unconfirmed").size(), 1U);
  EXPECT_EQ(state.at("unconfirmed")[0].at("id"), "unsettled");
  EXPECT_EQ(state.at("unconfirmed")[0].at("trading_day"), "20260928");
  act(restored, authorize("after.failure"));
  EXPECT_THROW(act(restored, submit("blocked", "1")), std::invalid_argument);
}
TEST_F(Live, SameDayReconnectDoesNotMistakeCachedOrdersForBrokerConfirmation) {
  LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
  ASSERT_EQ(ready(session).at("phase"), "ready");
  act(session, authorize());
  act(session, submit("missing", "3"));
  ASSERT_TRUE(
      wait_for(session, [](const Json& s) { return !s.at("orders").empty(); }).at("orders").size());
  exchange.reset(); // broker no longer reports the order
  ctp::SharedLibrary reconnect(ASTERION_TEST_CTP_TRADER, "asterion_fake_trader_reconnect");
  reconnect.symbol<void (*)()>()();
  const auto state = wait_for(session, [](const Json& s) {
    return s.at("phase") == "ready" && s.at("authorization").is_null();
  });
  ASSERT_EQ(state.at("phase"), "ready");
  EXPECT_TRUE(state.at("orders").empty());
  ASSERT_EQ(state.at("unconfirmed").size(), 1U);
  EXPECT_EQ(state.at("unconfirmed")[0].at("id"), "missing");
  act(session, authorize("again"));
  EXPECT_THROW(act(session, submit("next", "1")), std::invalid_argument);
}

TEST_F(Live, ForeignJournalIsRejectedWithoutAddingTablesOrChangingBytes) {
  {
    sqlite::Database database(directory.path / "journal.sqlite");
    database.execute("CREATE TABLE foreign_data(value TEXT)");
    database.execute("INSERT INTO foreign_data VALUES('preserve')");
  }
  const auto before = sha256_file(directory.path / "journal.sqlite");
  SqliteJournal journal(directory.path);
  EXPECT_THROW(journal.start(), std::invalid_argument);
  EXPECT_EQ(sha256_file(directory.path / "journal.sqlite"), before);
  sqlite::Database inspect(directory.path / "journal.sqlite", sqlite::Database::Access::read_only);
  sqlite::Database::Statement tables(inspect,
                                     "SELECT COUNT(*) FROM sqlite_schema WHERE type='table'");
  ASSERT_TRUE(tables.step());
  EXPECT_EQ(tables.integer(0), 1);
}

TEST_F(Live, AutomaticSegmentsPreserveAccountAuthorizationAndDeduplicationAcrossRecovery) {
  test::SmallJournalSegments budget(1);
  std::string account, policy;
  {
    LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
    ASSERT_EQ(ready(session).at("phase"), "ready");
    act(session, authorize());
    const auto before = session.snapshot();
    account = before.at("account_id");
    policy = before.at("policy_revision");
    act(session, submit("resting", "3"));
    const auto after = session.snapshot();
    EXPECT_EQ(after.at("account_id"), account);
    EXPECT_EQ(after.at("policy_revision"), policy);
    EXPECT_EQ(after.at("authorization"), before.at("authorization"));
    EXPECT_EQ(after.at("phase"), "ready");
    EXPECT_EQ(after.at("segment_count"), 1);
    const auto effects = exchange.call<int (*)()>("asterion_fake_catalog_side_effects");
    EXPECT_NO_THROW(act(session, submit("resting", "3")));
    EXPECT_THROW(act(session, submit("resting", "2")), Error);
    EXPECT_EQ(exchange.call<int (*)()>("asterion_fake_catalog_side_effects"), effects);
  }
  LiveSession restored(directory.path, ASTERION_TEST_CTP_TRADER, owners.path);
  const auto state = ready(restored);
  EXPECT_EQ(state.at("account_id"), account);
  EXPECT_EQ(state.at("policy_revision"), policy);
  EXPECT_TRUE(state.at("authorization").is_null());
  const auto effects = exchange.call<int (*)()>("asterion_fake_catalog_side_effects");
  EXPECT_NO_THROW(act(restored, submit("resting", "3")));
  EXPECT_THROW(act(restored, submit("resting", "2")), Error);
  EXPECT_EQ(exchange.call<int (*)()>("asterion_fake_catalog_side_effects"), effects);
  act(restored, {{"request_id", "cancel"}, {"action", "cancel"}, {"order_id", "resting"}});
  EXPECT_GT(restored.snapshot().at("segment_count"), 1);
}

TEST_F(Live, IdleAccountPublishesItsOwnProgressWithoutClientOrBrokerEvents) {
  LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
  std::this_thread::sleep_for(1250ms);
  const auto idle = session.health();
  EXPECT_TRUE(idle.state.observed);
  EXPECT_LT(idle.state.age_ms, 700U);
  EXPECT_TRUE(idle.persistence.observed);
  EXPECT_FALSE(idle.persistence.pending);
  EXPECT_FALSE(idle.business_ready);
  ASSERT_EQ(ready(session).at("phase"), "ready");
  const auto deadline = std::chrono::steady_clock::now() + 1s;
  while (!session.health().business_ready && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(10ms);
  EXPECT_TRUE(session.health().business_ready);
}

TEST_F(Live, ShutdownFencesPendingSendsDrainsRepliesAndWaitsForSdkRelease) {
  LiveSession session(directory.path, ASTERION_TEST_CTP_TRADER, owners.path, manifest());
  ASSERT_EQ(ready(session).at("phase"), "ready");
  act(session, authorize());
  const auto state = session.snapshot();
  HeldCommit commit;
  auto placed =
      session.execute(state.at("account_id").get<std::string>(),
                      state.at("policy_revision").get<std::string>(), submit("stopping", "1"));
  ASSERT_TRUE(commit.waiting());
  session.close_admission();
  EXPECT_THROW(session.connect("test-only", "test-only"), Error);
  EXPECT_TRUE(session.snapshot().at("orders").empty()); // Accepted replies can still read.
  commit.release();
  EXPECT_THROW(placed.get(), Error);
  EXPECT_TRUE(session.snapshot().at("unconfirmed").empty());
  struct Release {
    FakeExchange& exchange;
    ~Release() { exchange.call<void (*)(int)>("asterion_fake_trader_hold_release", 0); }
  } release{exchange};
  exchange.call<void (*)(int)>("asterion_fake_trader_hold_release", 1);
  session.stop();
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!exchange.call<int (*)()>("asterion_fake_trader_sdk_releases") &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(10ms);
  ASSERT_EQ(exchange.call<int (*)()>("asterion_fake_trader_sdk_releases"), 1);
  EXPECT_EQ(session.stopped().wait_for(100ms), std::future_status::timeout);
  EXPECT_FALSE(session.health().business_ready);
  EXPECT_THROW(session.snapshot(), Error);
  exchange.call<void (*)(int)>("asterion_fake_trader_hold_release", 0);
  ASSERT_EQ(session.stopped().wait_for(2s), std::future_status::ready);
  EXPECT_NO_THROW(session.stopped().get());
  EXPECT_EQ(exchange.call<int (*)()>("asterion_fake_trader_sdk_thread_mismatches"), 0);
}
