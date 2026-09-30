#include "engine.hpp"
#include <algorithm>
#include "paper_session.hpp"
#include "replay.hpp"
#include <asterion/kernel/process/child.hpp>
#include <asterion/protocol/strategy.hpp>
#include "bar_fixture.hpp"
#include <gtest/gtest.h>
using namespace asterion;
namespace fs = std::filesystem;
namespace {
struct Fixture {
  fs::path root = fs::temp_directory_path() / ("asterion-replay-" + unique_process_id());
  Json manifest;
  strategy::v1::Config config;
  std::unique_ptr<trading::PaperSession> account;
  std::unique_ptr<strategy::Session> session;
  Fixture() {
    fs::create_directory(root);
    fs::create_directory(root / "account");
    fs::create_directory(root / "strategy");
    // Six one-lot bars over two trading days settled at 105 and 110.
    std::vector<MarketBar> bars;
    std::int64_t time = 0;
    for (auto price : {100, 101, 100, 102, 99, 103}) {
      ++time;
      bars.push_back(
          test::flat(time <= 3 ? "2026-09-25" : "2026-09-28", time, std::to_string(price).c_str()));
    }
    const auto dataset =
        test::dataset(bars, {{"2026-09-25", test::dec("105")}, {"2026-09-28", test::dec("110")}});
    manifest = {{"version", 2},
                {"type", "historical_paper"},
                {"risk",
                 {{"max_order_quantity", "100"},
                  {"max_gross_quantity", "100"},
                  {"max_working_orders", std::uint64_t{100}}}},
                {"deposit", "1000"},
                {"costs",
                 {{"margin_per_lot", "100"},
                  {"open_fee", "2"},
                  {"close_today_fee", "3"},
                  {"close_yesterday_fee", "4"},
                  {"margin_rate", "0"},
                  {"open_fee_rate", "0"},
                  {"close_today_fee_rate", "0"},
                  {"close_yesterday_fee_rate", "0"}}},
                {"dataset", protocol::decode_bar_dataset(dataset)}};
    config.set_version(1);
    config.set_session_id("replay");
    config.set_stream_id("dataset");
    config.set_plugin_id("asterion.strategy.cta.sma-long-flat");
    config.set_fast(1);
    config.set_slow(2);
    config.mutable_quantity()->set_units(100000000);
    *config.mutable_contract() = dataset.contract();
    auto* plan = config.mutable_replay();
    plan->set_version(3);
    *plan->mutable_dataset() = dataset;
    plan->set_trading_session("account");
    plan->set_grant_id("grant");
    plan->set_host("localhost");
    plan->set_port(1);
    plan->set_tls_ca("test-ca");
    plan->set_tls_cert("test-cert");
    plan->set_tls_key("test-key");
    account = std::make_unique<trading::PaperSession>(root / "account", manifest);
    account->execute({{"request_id", "days"}, {"action", "replay_days"}});
    account->execute({{"request_id", "grant.request"},
                      {"action", "strategy_grant"},
                      {"grant_id", "grant"},
                      {"strategy_id", "replay"},
                      {"stream_id", "dataset"},
                      {"dataset_revision", plan->dataset().revision()},
                      {"max_quantity", "1"}});
    session = std::make_unique<strategy::Session>(root / "strategy", "replay", &config);
  }
  ~Fixture() {
    session.reset();
    account.reset();
    std::error_code e;
    fs::remove_all(root, e);
  }
  protocol::v1::Snapshot call(protocol::v1::Request r) {
    EXPECT_EQ(r.session_id(), "account");
    EXPECT_EQ(r.mode(), protocol::v1::PAPER);
    if (r.has_command())
      account->execute(protocol::decode_command(r.command()));
    else
      EXPECT_TRUE(r.has_snapshot());
    return protocol::encode_snapshot(account->snapshot());
  }
  void recover() {
    session.reset();
    account.reset();
    account = std::make_unique<trading::PaperSession>(root / "account");
    session = std::make_unique<strategy::Session>(root / "strategy", "replay");
  }
  void finish() {
    strategy::Replay driver(*session, [&](auto r) { return call(r); });
    bool complete = false;
    for (int i = 0; i < 10 && !complete; ++i)
      complete = driver.step();
    ASSERT_TRUE(complete);
    EXPECT_TRUE(driver.step());
  }
};
} // namespace
TEST(StrategyReplay, EveryCommitBoundaryRecoversWithoutSkippingEventsOrDuplicatingOrders) {
  for (const auto& boundary :
       {"before-advance", "after-advance", "before-target", "after-target", "after-revoke"}) {
    SCOPED_TRACE(boundary);
    Fixture f;
    bool interrupted = false;
    strategy::Replay driver(*f.session, [&](auto r) {
      const auto& c = r.command();
      const bool selected =
          r.has_command() &&
          ((std::string(boundary).find("advance") != std::string::npos && c.has_advance()) ||
           (std::string(boundary).find("target") != std::string::npos && c.has_strategy_target()) ||
           (std::string(boundary).find("revoke") != std::string::npos && c.has_strategy_revoke()));
      const bool fail = selected && !interrupted;
      if (fail && std::string(boundary).starts_with("before")) {
        interrupted = true;
        throw std::runtime_error("connection lost before delivery");
      }
      auto result = f.call(r);
      if (fail) {
        interrupted = true;
        throw std::runtime_error("acknowledgement lost after durable commit");
      }
      return result;
    });
    EXPECT_THROW(
        {
          for (int i = 0; i < 10; ++i)
            if (driver.step())
              break;
        },
        std::runtime_error);
    ASSERT_TRUE(interrupted);
    f.recover();
    f.finish();
    const auto account = f.account->snapshot();
    // One buy at 100, carried over settlement, one yesterday close at 103.
    EXPECT_EQ(account.at("balance"), "1024");
    EXPECT_EQ(account.at("fees"), "6");
    EXPECT_EQ(account.at("orders").size(), 2U);
    EXPECT_EQ(account.at("fills").size(), 2U);
    EXPECT_TRUE(account.at("positions").empty());
    EXPECT_FALSE(account.at("strategy").at("active").get<bool>());
    EXPECT_EQ(f.session->processed(), 6U);
  }
}
TEST(StrategyReplay, RevocationAndDivergentClocksStopWithoutFurtherMutation) {
  {
    Fixture f;
    f.account->execute(
        {{"request_id", "stop"}, {"action", "strategy_revoke"}, {"grant_id", "grant"}});
    const auto before = f.account->snapshot();
    strategy::Replay driver(*f.session, [&](auto r) { return f.call(r); });
    EXPECT_THROW(driver.step(), std::invalid_argument);
    EXPECT_EQ(f.account->snapshot(), before);
    EXPECT_EQ(f.session->processed(), 0U);
  }
  {
    Fixture f;
    for (const auto* id : {"a", "b"})
      f.account->execute({{"request_id", id}, {"action", "advance"}});
    const auto before = f.account->snapshot();
    strategy::Replay driver(*f.session, [&](auto r) { return f.call(r); });
    EXPECT_THROW(driver.step(), std::invalid_argument);
    EXPECT_EQ(f.account->snapshot(), before);
    EXPECT_EQ(f.session->processed(), 0U);
  }
}
TEST(StrategyReplay, PlanRejectsMixedConnectionsAndCorruptDatasetBeforeWriting) {
  Fixture f;
  const auto json = protocol::decode_replay_plan(f.config.replay());
  EXPECT_EQ(protocol::encode_replay_plan(json).SerializeAsString(),
            f.config.replay().SerializeAsString());
  auto mixed = f.config.replay();
  mixed.set_agent_endpoint("local-agent");
  EXPECT_THROW(protocol::decode_replay_plan(mixed), std::invalid_argument);
  auto corrupt = f.config.replay();
  corrupt.mutable_dataset()->mutable_bars(0)->mutable_close()->set_units(1);
  EXPECT_THROW(protocol::decode_replay_plan(corrupt), std::invalid_argument);
  auto wrong = f.config;
  wrong.mutable_contract()->set_symbol("rb2611");
  fs::create_directory(f.root / "wrong");
  EXPECT_THROW((strategy::Session(f.root / "wrong", "replay", &wrong)), std::invalid_argument);
  EXPECT_TRUE(fs::is_empty(f.root / "wrong"));
}

TEST(StrategyReplay, ScheduledReplayRecoversBeforeAndAfterEachSettlementCommit) {
  Fixture reference;
  reference.finish();
  const auto expected = reference.account->snapshot();
  EXPECT_EQ(expected.at("balance"), "1024");
  EXPECT_EQ(expected.at("fees"), "6");
  EXPECT_EQ(expected.at("replay").at("settled_days"), 2);
  EXPECT_FALSE(expected.at("strategy").at("active").get<bool>());
  for (unsigned day : {0U, 1U})
    for (bool committed : {false, true}) {
      Fixture f;
      bool injected = false;
      {
        strategy::Replay driver(*f.session, [&](auto request) {
          if (!injected && request.has_command() && request.command().has_replay_settle() &&
              request.command().replay_settle().day_index() == day) {
            injected = true;
            if (committed)
              static_cast<void>(f.call(request));
            throw std::runtime_error("test settlement transport interruption");
          }
          return f.call(request);
        });
        EXPECT_THROW(
            {
              for (int i = 0; i < 10; ++i)
                if (driver.step())
                  break;
            },
            std::runtime_error);
      }
      ASSERT_TRUE(injected);
      f.recover();
      f.finish();
      EXPECT_EQ(f.account->snapshot(), expected);
      f.recover();
      f.finish();
      EXPECT_EQ(f.account->snapshot(), expected);
    }
}
TEST(StrategyReplay, AccountWithoutDayEndBindingIsRefused) {
  Fixture scheduled;
  auto call = [&](auto request) {
    auto state = scheduled.call(request);
    state.clear_replay();
    return state;
  };
  strategy::Replay missing(*scheduled.session, call);
  EXPECT_THROW(missing.probe(), std::invalid_argument);
  auto old = scheduled.config.replay();
  old.set_version(2);
  EXPECT_THROW(protocol::decode_replay_plan(old), std::invalid_argument);
}

TEST(StrategyReplay, ScheduledExecutionMatchesBacktestLedgerAndFillEconomics) {
  Fixture f;
  f.finish();
  research::v1::BacktestInput input;
  input.set_version(6);
  *input.mutable_paper() = protocol::encode_input(f.manifest);
  input.set_dataset_revision(f.config.replay().dataset().revision());
  input.mutable_sma()->set_fast(f.config.fast());
  input.mutable_sma()->set_slow(f.config.slow());
  *input.mutable_sma()->mutable_quantity() = f.config.quantity();
  const auto result = backtest::run(input);
  const auto expected = protocol::decode_snapshot(result.account());
  const auto actual = f.account->snapshot();
  for (const auto* key : {"balance", "equity", "available", "margin", "frozen", "fees", "realized",
                          "unrealized", "mark", "positions", "cursor", "total"})
    EXPECT_EQ(actual.at(key), expected.at(key)) << key;
  const auto fills = [](const Json& account) {
    Json values = Json::array();
    for (const auto& fill : account.at("fills")) {
      const auto& orders = account.at("orders");
      const auto order = std::find_if(orders.begin(), orders.end(), [&](const auto& candidate) {
        return candidate.at("id") == fill.at("order_id");
      });
      if (order == orders.end())
        throw std::runtime_error("missing fill order");
      values.push_back({{"price", fill.at("price")},
                        {"quantity", fill.at("quantity")},
                        {"side", order->at("side")},
                        {"offset", order->at("offset")}});
    }
    return values;
  };
  EXPECT_EQ(fills(actual), fills(expected));
  ASSERT_EQ(result.settlements_size(), 2);
  EXPECT_EQ(actual.at("replay").at("settled_days"), result.settlements_size());
}
