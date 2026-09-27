#include "engine.hpp"
#include <algorithm>
#include "paper_session.hpp"
#include "replay.hpp"
#include <asterion/kernel/process/child.hpp>
#include <asterion/protocol/strategy.hpp>
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
  Fixture(bool scheduled = false) {
    fs::create_directory(root);
    fs::create_directory(root / "account");
    fs::create_directory(root / "strategy");
    manifest = {{"version", 1},
                {"type", "historical_paper"},
                {"risk",
                 {{"max_order_quantity", "100"},
                  {"max_gross_quantity", "100"},
                  {"max_working_orders", std::uint64_t{100}}}},
                {"deposit", "1000"},
                {"contract",
                 {{"venue", "SHFE"},
                  {"symbol", "rb2610"},
                  {"currency", "CNY"},
                  {"price_increment", "1"},
                  {"quantity_increment", "1"},
                  {"multiplier", "10"},
                  {"product", "rb"},
                  {"delivery_month", "2026-10"}}},
                {"costs",
                 {{"margin_per_lot", "100"},
                  {"open_fee", "2"},
                  {"close_today_fee", "3"},
                  {"close_yesterday_fee", "4"}}},
                {"ticks", Json::array()}};
    std::int64_t time = 0;
    for (auto price : {100, 101, 100, 102, 99, 103})
      manifest["ticks"].push_back({{"timestamp_ns", std::to_string(++time)},
                                   {"price", std::to_string(price)},
                                   {"quantity", "1"}});
    const auto p = protocol::encode_input(manifest);
    config.set_version(1);
    config.set_session_id("replay");
    config.set_stream_id("dataset");
    config.set_plugin_id("asterion.strategy.cta.sma-long-flat");
    config.set_fast(1);
    config.set_slow(2);
    config.mutable_quantity()->set_units(100000000);
    *config.mutable_contract() = p.contract();
    auto* plan = config.mutable_replay();
    plan->set_version(2);
    *plan->mutable_dataset() = protocol::make_trade_dataset(p.contract(), p.ticks());
    plan->set_trading_session("account");
    plan->set_grant_id("grant");
    plan->set_host("localhost");
    plan->set_port(1);
    plan->set_tls_ca("test-ca");
    plan->set_tls_cert("test-cert");
    plan->set_tls_key("test-key");
    account = std::make_unique<trading::PaperSession>(root / "account", manifest);
    if (scheduled) {
      google::protobuf::RepeatedPtrField<data::v1::SettlementDay> days;
      protocol::encode_settlement_days(
          Json::array({{{"trading_day", "2026-09-25"},
                        {"sessions", Json::array({{{"begin_ns", "1"}, {"end_ns", "4"}}})},
                        {"schedule_source", "fixture"},
                        {"settlement_price", "105"},
                        {"settlement_source", "fixture"}},
                       {{"trading_day", "2026-09-28"},
                        {"sessions", Json::array({{{"begin_ns", "4"}, {"end_ns", "7"}}})},
                        {"schedule_source", "fixture"},
                        {"settlement_price", "110"},
                        {"settlement_source", "fixture"}}}),
          days);
      data::v1::CalendarPublication publication;
      publication.set_version(1);
      *publication.mutable_calendar() = protocol::make_settlement_calendar(p.contract(), days);
      publication.set_source_name("fixture.csv");
      publication.set_source_sha256(std::string(64, 'a'));
      publication.set_source_bytes(1);
      publication.set_importer("asterion.csv.settlement.v1");
      publication.set_id(protocol::calendar_publication_id(publication));
      *plan->mutable_calendar_publication() = publication;
      account->execute({{"request_id", "calendar"},
                        {"action", "replay_calendar"},
                        {"publication", protocol::decode_calendar_publication(publication)}});
    }
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
    EXPECT_EQ(account.at("balance"), "1050");
    EXPECT_EQ(account.at("fees"), "10");
    EXPECT_EQ(account.at("orders").size(), 4U);
    EXPECT_EQ(account.at("fills").size(), 4U);
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
  corrupt.mutable_dataset()->mutable_ticks(0)->mutable_price()->set_units(1);
  EXPECT_THROW(protocol::decode_replay_plan(corrupt), std::invalid_argument);
  auto wrong = f.config;
  wrong.mutable_contract()->set_symbol("rb2611");
  fs::create_directory(f.root / "wrong");
  EXPECT_THROW((strategy::Session(f.root / "wrong", "replay", &wrong)), std::invalid_argument);
  EXPECT_TRUE(fs::is_empty(f.root / "wrong"));
}

TEST(StrategyReplay, ScheduledReplayRecoversBeforeAndAfterEachSettlementCommit) {
  Fixture reference(true);
  reference.finish();
  const auto expected = reference.account->snapshot();
  EXPECT_EQ(expected.at("balance"), "1024");
  EXPECT_EQ(expected.at("fees"), "6");
  EXPECT_EQ(expected.at("replay").at("settled_days"), 2);
  EXPECT_FALSE(expected.at("strategy").at("active").get<bool>());
  for (unsigned day : {0U, 1U})
    for (bool committed : {false, true}) {
      Fixture f(true);
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
TEST(StrategyReplay, CalendarIdentityAndScheduleModeMustMatchAccount) {
  Fixture scheduled(true);
  auto call = [&](auto request) {
    auto state = scheduled.call(request);
    state.clear_replay();
    return state;
  };
  strategy::Replay missing(*scheduled.session, call);
  EXPECT_THROW(missing.probe(), std::invalid_argument);
  auto forged = [&](auto request) {
    auto state = scheduled.call(request);
    auto* publication = state.mutable_replay()->mutable_publication();
    publication->set_source_name("other.csv");
    publication->set_id(protocol::calendar_publication_id(*publication));
    return state;
  };
  strategy::Replay different(*scheduled.session, forged);
  EXPECT_THROW(different.probe(), std::invalid_argument);
  auto old = scheduled.config.replay();
  old.set_version(1);
  EXPECT_THROW(protocol::decode_replay_plan(old), std::invalid_argument);
}

TEST(StrategyReplay, ScheduledExecutionMatchesBacktestLedgerAndFillEconomics) {
  Fixture f(true);
  f.finish();
  research::v1::BacktestInput input;
  input.set_version(5);
  *input.mutable_paper() = protocol::encode_input(f.manifest);
  input.set_dataset_revision(f.config.replay().dataset().revision());
  *input.mutable_calendar_publication() = f.config.replay().calendar_publication();
  *input.mutable_days() = input.calendar_publication().calendar().days();
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
