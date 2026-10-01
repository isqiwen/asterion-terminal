#include "journal_fixture.hpp"
#include "timing.hpp"
#include <gtest/gtest.h>
#include <asterion/domain/account.hpp>
#include <asterion/kernel/process/child.hpp>
#include "paper_execution.hpp"
#include "order_limits.hpp"
#include "sqlite_journal.hpp"
#include "paper_session.hpp"
#include <asterion/protocol/data.hpp>
#include "bar_fixture.hpp"
#include <chrono>
#include <fstream>
using namespace asterion;
using asterion::trading::PaperSession;
namespace {
Decimal d(const char* value) {
  return Decimal::parse(value);
}
Instrument instrument() {
  return {{"SHFE", "rb2610"}, AssetClass::futures, "CNY", d("1"), d("1"), d("10")};
}
std::shared_ptr<const RiskPort> risk() {
  auto value = std::make_shared<OrderLimits>(OrderLimitsConfig{d("100"), d("100"), 100});
  value->start();
  return value;
}
FuturesCosts costs() {
  return {d("100"), d("2"), d("3"), d("4")};
}
LimitOrder order(std::string id, Side side, const char* quantity, const char* price) {
  return {std::move(id), instrument().id, side, d(quantity), d(price)};
}
struct Directory {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() / ("asterion-paper-中文-" + unique_process_id());
  Directory() { std::filesystem::create_directory(path); }
  ~Directory() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};
// Three one-price bars with volume 10: one lot fills per bar at the 10%
// participation rate. Two trading days settle at 105 and 120.
std::vector<MarketBar> bars() {
  return {test::flat("2026-09-25", 100, "100"), test::flat("2026-09-25", 200, "99"),
          test::flat("2026-09-28", 300, "110")};
}
Json cost_json() {
  return {{"margin_per_lot", "100"},     {"open_fee", "2"},
          {"close_today_fee", "3"},      {"close_yesterday_fee", "4"},
          {"margin_rate", "0"},          {"open_fee_rate", "0"},
          {"close_today_fee_rate", "0"}, {"close_yesterday_fee_rate", "0"}};
}
Json manifest() {
  return {{"version", 3},
          {"type", "historical_paper"},
          {"risk",
           {{"max_order_quantity", "100"},
            {"max_gross_quantity", "100"},
            {"max_working_orders", std::uint64_t{100}}}},
          {"deposit", "1000"},
          {"contracts",
           {{{"dataset",
              test::dataset_json(bars(), {{"2026-09-25", d("105")}, {"2026-09-28", d("120")}})},
             {"costs", cost_json()}}}}};
}
Json advance(std::string id) {
  return {{"request_id", id}, {"action", "advance"}};
}
Json submit(std::string id, std::string side, std::string offset, std::string price) {
  return {{"request_id", id}, {"action", "submit"}, {"order_id", "order." + id},
          {"venue", "SHFE"},  {"symbol", "rb2610"}, {"side", side},
          {"offset", offset}, {"quantity", "1"},    {"price", price}};
}
} // namespace
TEST(FuturesAccount, FreezesPartialFillsClosesAndDeduplicates) {
  FuturesAccount account(d("1000"), {{instrument(), costs()}});
  EXPECT_THROW(account.submit(order("a", Side::buy, "2", "100"), Offset::open),
               std::invalid_argument);
  account.mark(instrument().id, d("100"));
  account.submit(order("a", Side::buy, "2", "100"), Offset::open);
  EXPECT_EQ(account.snapshot()["frozen"], "204");
  EXPECT_EQ(account.snapshot()["available"], "796");
  Fill report{"fill.1", "a", d("1"), d("99")};
  ASSERT_TRUE(account.fill(report));
  EXPECT_EQ(account.snapshot()["balance"], "998");
  EXPECT_EQ(account.snapshot()["margin"], "100");
  EXPECT_EQ(account.snapshot()["frozen"], "102");
  const auto state = account.snapshot();
  EXPECT_FALSE(account.fill(report));
  EXPECT_EQ(account.snapshot(), state);
  auto conflict = report;
  conflict.price = d("98");
  EXPECT_THROW(account.fill(conflict), std::invalid_argument);
  EXPECT_EQ(account.snapshot(), state);
  account.cancel("a");
  EXPECT_EQ(account.snapshot()["frozen"], "0");
  account.submit(order("close", Side::sell, "1", "110"), Offset::close_today);
  EXPECT_THROW(account.submit(order("double", Side::sell, "1", "110"), Offset::close_today),
               std::invalid_argument);
  account.fill({"fill.2", "close", d("1"), d("111")});
  EXPECT_EQ(account.snapshot()["balance"], "1115");
  EXPECT_EQ(account.snapshot()["realized"], "120");
  EXPECT_EQ(account.snapshot()["fees"], "5");
  EXPECT_TRUE(account.snapshot()["positions"].empty());
  EXPECT_EQ(account.snapshot()["available"], "1115");
}
TEST(FuturesAccount, RejectsWithoutMutationAndSettlesShortPositions) {
  FuturesAccount account(d("1000"), {{instrument(), costs()}});
  account.mark(instrument().id, d("100"));
  const auto empty = account.snapshot();
  EXPECT_THROW(account.submit(order("too-big", Side::buy, "10", "100"), Offset::open),
               std::invalid_argument);
  EXPECT_THROW(account.submit(order("fraction", Side::buy, "0.5", "100"), Offset::open),
               std::invalid_argument);
  EXPECT_THROW(
      account.submit(order("no-position", Side::sell, "1", "100"), Offset::close_yesterday),
      std::invalid_argument);
  EXPECT_EQ(account.snapshot(), empty);
  account.submit(order("short", Side::sell, "2", "100"), Offset::open);
  EXPECT_THROW(account.settle({d("90")}), std::invalid_argument);
  account.fill({"f", "short", d("2"), d("100")});
  account.settle({d("90")});
  EXPECT_EQ(account.snapshot()["balance"], "1196");
  EXPECT_EQ(account.snapshot()["unrealized"], "0");
  EXPECT_EQ(account.snapshot()["positions"][0]["bucket"], "yesterday");
  EXPECT_THROW(account.submit(order("wrong-bucket", Side::buy, "1", "90"), Offset::close_today),
               std::invalid_argument);
  account.submit(order("close", Side::buy, "2", "90"), Offset::close_yesterday);
  account.fill({"close.f", "close", d("2"), d("89")});
  EXPECT_EQ(account.snapshot()["balance"], "1208");
  EXPECT_EQ(account.snapshot()["fees"], "12");
}
TEST(PaperExecution, UsesOnlyFollowingBarsAndSharesParticipationInArrivalOrder) {
  auto spec = instrument();
  PaperExecution engine(
      d("1000"),
      {{{spec, costs()},
        {test::flat("2026-09-28", 100, "100", "20"), test::flat("2026-09-28", 200, "99", "10"),
         test::flat("2026-09-28", 300, "101", "30")}}},
      risk());
  EXPECT_THROW(engine.advance(), std::logic_error);
  engine.start();
  engine.advance();
  engine.submit(order("a", Side::buy, "2", "101"), Offset::open);
  engine.submit(order("b", Side::buy, "2", "101"), Offset::open);
  EXPECT_TRUE(engine.snapshot()["fills"].empty());
  engine.advance();
  EXPECT_EQ(engine.snapshot()["orders"][0]["filled"], "1");
  EXPECT_EQ(engine.snapshot()["orders"][1]["filled"], "0");
  engine.advance();
  EXPECT_EQ(engine.snapshot()["orders"][0]["filled"], "2");
  EXPECT_EQ(engine.snapshot()["orders"][1]["filled"], "2");
  EXPECT_EQ(engine.snapshot()["fills"].size(), 3);
  auto state = engine.snapshot();
  EXPECT_THROW(engine.advance(), std::invalid_argument);
  EXPECT_EQ(engine.snapshot(), state);
}
TEST(SqliteJournal, ExclusiveWriterRecoveryAndCorruptionRejection) {
  Directory directory;
  {
    SqliteJournal journal(directory.path);
    journal.start();
    journal.append({{"test", 1}});
    SqliteJournal other(directory.path);
    EXPECT_THROW(other.start(), std::runtime_error);
    journal.append({{"test", 2}});
    EXPECT_EQ(journal.read().size(), 2);
  }
  {
    SqliteJournal recovered(directory.path);
    recovered.start();
    EXPECT_EQ(recovered.read().size(), 2);
    EXPECT_EQ(recovered.read().back(), (Json{{"test", 2}}));
  }
  // Unknown files are never adopted or removed.
  std::ofstream(directory.path / "notes.txt") << "user file";
  {
    SqliteJournal stray(directory.path);
    EXPECT_THROW(stray.start(), std::invalid_argument);
  }
  std::filesystem::remove(directory.path / "notes.txt");
  const auto file = directory.path / "journal.sqlite";
  const auto size = std::filesystem::file_size(file);
  {
    std::fstream corrupt(file, std::ios::in | std::ios::out | std::ios::binary);
    corrupt.seekp(0);
    corrupt << "not a database file";
  }
  SqliteJournal corrupt(directory.path);
  EXPECT_ANY_THROW(corrupt.start());
  EXPECT_EQ(std::filesystem::file_size(file), size) << "corrupt journal kept for inspection";
}
TEST(PaperSession, RecoversExactLedgerAndIdempotencyAcrossRestart) {
  Directory directory;
  Json expected;
  const auto buy = submit("buy", "buy", "open", "100");
  {
    PaperSession session(directory.path, manifest());
    session.execute(advance("tick1"));
    session.execute(buy);
    session.execute(advance("tick2")); // fills at 99; the first trading day ends
    EXPECT_THROW(session.execute(advance("early")), std::invalid_argument);
    session.execute({{"request_id", "day0"}, {"action", "replay_settle"}, {"day_index", 0}});
    session.execute(advance("tick3"));
    expected = session.snapshot();
    EXPECT_EQ(expected["balance"], "1058"); // 1000 - 2 fee + (105 - 99) x 10 at settlement
    EXPECT_EQ(expected["positions"].at(0).at("bucket"), "yesterday");
  }
  {
    PaperSession recovered(directory.path);
    EXPECT_EQ(recovered.snapshot(), expected);
    recovered.execute(buy);
    EXPECT_EQ(recovered.snapshot(), expected);
    auto conflict = buy;
    conflict["quantity"] = "2";
    EXPECT_THROW(recovered.execute(conflict), std::invalid_argument);
    EXPECT_EQ(recovered.snapshot(), expected);
  }
  EXPECT_THROW(PaperSession(directory.path, manifest()), std::invalid_argument);
}
TEST(PaperSession, FailedCommitDoesNotPublishAndRequiresRecovery) {
  Directory directory;
  {
    PaperSession session(directory.path, manifest());
    asterion::sqlite::fail_next_commits_for_testing(1);
    EXPECT_THROW(session.execute(advance("tick1")), std::runtime_error);
    EXPECT_EQ(session.snapshot()["cursor"], 0);
    EXPECT_EQ(session.snapshot()["storage_state"], "recovery_required");
    EXPECT_THROW(session.execute(advance("tick2")), std::runtime_error);
    asterion::sqlite::fail_next_commits_for_testing(0);
  }
  PaperSession recovered(directory.path);
  EXPECT_EQ(recovered.snapshot()["cursor"], 0);
  recovered.execute(advance("tick1"));
  EXPECT_EQ(recovered.snapshot()["cursor"], 1);
}
TEST(PaperSession, RejectsUnsupportedOrTamperedInputWithoutWrites) {
  Directory directory;
  auto bad = manifest();
  bad["version"] = 2;
  EXPECT_THROW(PaperSession(directory.path, bad), std::invalid_argument);
  EXPECT_EQ(test::journal_size(directory.path), 0U);
  PaperSession session(directory.path, manifest());
  auto bad_command = advance("one");
  bad_command["extra"] = true;
  EXPECT_ANY_THROW(session.execute(bad_command));
  EXPECT_EQ(session.snapshot()["cursor"], 0);
  EXPECT_EQ(session.snapshot()["storage_state"], "ready");
}

TEST(FuturesAccount, LossesBlockNewExposureButDoNotBlockClosing) {
  FuturesAccount account(d("200"), {{instrument(), costs()}});
  account.mark(instrument().id, d("100"));
  account.submit(order("open", Side::buy, "1", "100"), Offset::open);
  account.fill({"f1", "open", d("1"), d("100")});
  account.mark(instrument().id, d("1"));
  EXPECT_EQ(account.snapshot()["available"], "-892");
  EXPECT_THROW(account.submit(order("more", Side::buy, "1", "1"), Offset::open),
               std::invalid_argument);
  account.submit(order("close", Side::sell, "1", "1"), Offset::close_today);
  account.fill({"f2", "close", d("1"), d("1")});
  EXPECT_EQ(account.snapshot()["balance"], "-795");
  EXPECT_TRUE(account.snapshot()["positions"].empty());
}
TEST(PaperExecution, UnmatchedOrdersCancelAndReleaseAllReserves) {
  auto spec = instrument();
  PaperExecution engine(
      d("1000"),
      {{{spec, costs()},
        {test::flat("2026-09-28", 100, "100"), test::flat("2026-09-28", 200, "101")}}},
      risk());
  engine.start();
  engine.advance();
  engine.submit(order("a", Side::buy, "1", "99"), Offset::open);
  engine.advance();
  EXPECT_TRUE(engine.snapshot()["fills"].empty());
  engine.cancel("a");
  EXPECT_EQ(engine.snapshot()["available"], "1000");
  EXPECT_EQ(engine.snapshot()["frozen"], "0");
}

namespace {
Json grant(const Json& input) {
  return {{"request_id", "grant.request"},
          {"action", "strategy_grant"},
          {"grant_id", "grant.one"},
          {"strategy_id", "sma.one"},
          {"stream_id", "history.one"},
          {"dataset_revision", input.at("contracts").at(0).at("dataset").at("revision")},
          {"max_quantity", "2"}};
}
Json target(const Json& g, std::string id, int sequence, std::string quantity) {
  auto t = g;
  t.erase("max_quantity");
  t["action"] = "strategy_target";
  t["request_id"] = id;
  t["sequence"] = sequence;
  t["timestamp_ns"] = std::to_string(sequence * 100);
  t["venue"] = "SHFE";
  t["symbol"] = "rb2610";
  t["target_quantity"] = quantity;
  return t;
}
} // namespace
TEST(StrategyTrading, RevocableGrantFencesManualOrdersAndReplayIsIdempotent) {
  Directory dir;
  const auto m = manifest();
  const auto g = grant(m);
  Json saved;
  const auto intent = target(g, "intent.one", 1, "2");
  const Json revoke{
      {"request_id", "revoke.one"}, {"action", "strategy_revoke"}, {"grant_id", "grant.one"}};
  {
    PaperSession session(dir.path, m);
    EXPECT_THROW(session.execute(intent), std::invalid_argument);
    session.execute(g);
    EXPECT_THROW(session.execute(submit("manual", "buy", "open", "100")), std::invalid_argument);
    session.execute(advance("tick.one"));
    session.execute(intent);
    EXPECT_EQ(session.snapshot().at("orders").size(), 1U);
    EXPECT_TRUE(session.snapshot().at("fills").empty()); // never fill on the source event
    session.execute(intent);
    EXPECT_EQ(session.snapshot().at("orders").size(), 1U);
    session.execute(advance("tick.two"));
    EXPECT_EQ(session.snapshot().at("fills").size(), 1U); // only one of two lots fills
    // The trading day ends with tick.two; its unfilled lot does not carry over.
    EXPECT_EQ(session.snapshot().at("orders")[0].at("state"), "cancelled");
    session.execute(revoke);
    EXPECT_EQ(session.snapshot().at("frozen"), "0");
    EXPECT_EQ(session.snapshot().at("positions").size(), 1U); // revoke does not liquidate
    EXPECT_THROW(session.execute(target(g, "late", 2, "0")), std::invalid_argument);
    session.execute(intent); // acknowledged old intent cannot execute again after revoke
    saved = session.snapshot();
    EXPECT_EQ(protocol::decode_snapshot(protocol::encode_snapshot(saved)).at("strategy"),
              saved.at("strategy"));
  }
  {
    PaperSession restored(dir.path);
    EXPECT_EQ(restored.snapshot(), saved);
    restored.execute(intent);
    restored.execute(revoke);
    EXPECT_EQ(restored.snapshot(), saved);
    auto conflict = intent;
    conflict["target_quantity"] = "1";
    EXPECT_THROW(restored.execute(conflict), std::invalid_argument);
    EXPECT_THROW(restored.execute(target(g, "late", 2, "0")), std::invalid_argument);
    // Manual operations resume after revoke; orders wait for the next trading day.
    EXPECT_THROW(restored.execute(submit("manual.close", "sell", "close_yesterday", "99")),
                 std::invalid_argument);
    restored.execute({{"request_id", "day0"}, {"action", "replay_settle"}, {"day_index", 0}});
    restored.execute(advance("tick.three"));
    EXPECT_EQ(restored.snapshot().at("positions").at(0).at("bucket"), "yesterday");
  }
}
TEST(StrategyTrading, ScopeSequenceAndRiskFailuresDoNotMutateAccountOrAuthorization) {
  Directory dir;
  const auto m = manifest();
  const auto g = grant(m);
  PaperSession session(dir.path, m);
  auto wrong = g;
  wrong["dataset_revision"] = "wrong";
  EXPECT_THROW(session.execute(wrong), std::invalid_argument);
  session.execute(g);
  session.execute(advance("one"));
  const auto before = session.snapshot();
  const auto good = target(g, "signal", 1, "1");
  for (const auto* field : {"grant_id", "strategy_id", "stream_id", "dataset_revision"}) {
    auto bad = good;
    bad[field] = "other";
    EXPECT_THROW(session.execute(bad), std::invalid_argument);
  }
  auto bad = good;
  bad["timestamp_ns"] = "0";
  EXPECT_THROW(session.execute(bad), std::invalid_argument);
  bad = good;
  bad["sequence"] = 2;
  EXPECT_THROW(session.execute(bad), std::invalid_argument);
  for (const auto* q : {"-1", "3", "0.5"}) {
    bad = good;
    bad["target_quantity"] = q;
    EXPECT_THROW(session.execute(bad), std::invalid_argument);
  }
  EXPECT_EQ(session.snapshot(), before);
  session.execute(good);
  const auto accepted = session.snapshot();
  auto duplicate = good;
  duplicate["request_id"] = "another.signal";
  EXPECT_THROW(session.execute(duplicate), std::invalid_argument);
  EXPECT_EQ(session.snapshot(), accepted);
  Directory poor;
  auto low = m;
  low["deposit"] = "101";
  PaperSession insufficient(poor.path, low);
  insufficient.execute(grant(low));
  insufficient.execute(advance("one"));
  const auto original = insufficient.snapshot();
  EXPECT_THROW(insufficient.execute(target(grant(low), "signal", 1, "1")), std::invalid_argument);
  EXPECT_EQ(insufficient.snapshot(), original);
}
TEST(StrategyTrading, GrantIdentitiesCannotBeReusedAndCommandsRoundTrip) {
  Directory dir;
  const auto g = grant(manifest());
  PaperSession session(dir.path, manifest());
  const Json revoke{
      {"request_id", "revoke"}, {"action", "strategy_revoke"}, {"grant_id", "grant.one"}};
  for (const auto& command : {g, revoke, target(g, "intent", 1, "1")})
    EXPECT_EQ(protocol::decode_command(protocol::encode_command(command)), command);
  session.execute(g);
  session.execute(revoke);
  auto reused = g;
  reused["request_id"] = "another.grant";
  EXPECT_THROW(session.execute(reused), std::invalid_argument);
  reused["grant_id"] = "grant.two";
  session.execute(reused);
  EXPECT_THROW(session.execute(target(g, "late", 1, "1")), std::invalid_argument);
  auto malformed = protocol::encode_command(target(reused, "intent", 1, "1"));
  malformed.mutable_strategy_target()->clear_target_quantity();
  EXPECT_THROW(protocol::decode_command(malformed), std::invalid_argument);
}

TEST(StrategyTrading, FailedCommitRestoresAllAuthorizationHistoryAndCommittedLedger) {
  for (const auto& action : {"advance", "strategy_target", "strategy_revoke"}) {
    SCOPED_TRACE(action);
    Directory dir;
    auto g = grant(manifest());
    Json committed;
    {
      PaperSession session(dir.path, manifest());
      session.execute(g);
      session.execute({{"request_id", "revoke.first"},
                       {"action", "strategy_revoke"},
                       {"grant_id", g.at("grant_id")}});
      g["grant_id"] = "grant.two";
      g["request_id"] = "grant.second";
      session.execute(g);
      if (std::string_view(action) != "advance")
        session.execute(advance("first.tick"));
      if (std::string_view(action) == "strategy_revoke")
        session.execute(target(g, "working.order", 1, "1"));
      committed = session.snapshot();
      Json command = advance("failed.command");
      if (std::string_view(action) == "strategy_target")
        command = target(g, "failed.command", 1, "1");
      else if (std::string_view(action) == "strategy_revoke")
        command = {
            {"request_id", "failed.command"}, {"action", action}, {"grant_id", g.at("grant_id")}};
      asterion::sqlite::fail_next_commits_for_testing(1);
      EXPECT_THROW(session.execute(command), std::runtime_error);
      auto shown = session.snapshot();
      EXPECT_EQ(shown.at("storage_state"), "recovery_required");
      shown["storage_state"] = "ready";
      EXPECT_EQ(shown, committed);
      EXPECT_THROW(session.execute(command), std::runtime_error);
      asterion::sqlite::fail_next_commits_for_testing(0);
    }
    PaperSession recovered(dir.path);
    EXPECT_EQ(recovered.snapshot(), committed);
    recovered.execute({{"request_id", "revoke.second"},
                       {"action", "strategy_revoke"},
                       {"grant_id", g.at("grant_id")}});
    auto reused = grant(manifest());
    reused["request_id"] = "reused.first";
    EXPECT_THROW(recovered.execute(reused), std::invalid_argument);
  }
}

TEST(PaperExecution, FailedTargetReplacementKeepsExistingOrdersAndReserves) {
  PaperExecution engine(
      d("200"),
      {{{instrument(), costs()},
        {test::flat("2026-09-28", 100, "100"), test::flat("2026-09-28", 200, "99")}}},
      risk());
  engine.start();
  engine.advance();
  engine.reconcile_long_target("one", instrument().id, d("1"), d("100"));
  const auto before = engine.snapshot();
  EXPECT_THROW(engine.reconcile_long_target("two", instrument().id, d("2"), d("100")),
               std::invalid_argument);
  EXPECT_EQ(engine.snapshot(), before);
  engine.advance();
  EXPECT_EQ(engine.snapshot().at("fills").size(), 1U);
}

TEST(StrategyTrading, InterruptedAuthorizationJournalIsNeverSilentlyOverwritten) {
  Directory dir;
  {
    PaperSession session(dir.path, manifest());
    session.execute(grant(manifest()));
  }
  {
    std::ofstream foreign(dir.path / "notes.txt");
    foreign << "partial authorization change";
  }
  EXPECT_THROW((PaperSession(dir.path)), std::invalid_argument);
  std::ifstream foreign(dir.path / "notes.txt");
  std::string bytes{std::istreambuf_iterator<char>(foreign), {}};
  EXPECT_EQ(bytes, "partial authorization change");
}

TEST(PreTradeRisk, ManualAndStrategyEnforcementPersistsAcrossRecovery) {
  auto m = manifest();
  m["risk"] = {{"max_order_quantity", "1"},
               {"max_gross_quantity", "1"},
               {"max_working_orders", std::uint64_t{1}}};
  auto too_large = submit("oversize", "buy", "open", "100");
  too_large["quantity"] = "2";
  Directory manual;
  Json saved;
  {
    PaperSession session(manual.path, m);
    session.execute(advance("one"));
    const auto before = session.snapshot();
    EXPECT_THROW(session.execute(too_large), std::invalid_argument);
    EXPECT_EQ(session.snapshot(), before);
    session.execute(submit("first", "buy", "open", "100"));
    saved = session.snapshot();
    EXPECT_THROW(session.execute(submit("second", "buy", "open", "100")), std::invalid_argument);
    EXPECT_EQ(session.snapshot(), saved);
    EXPECT_EQ(protocol::decode_snapshot(protocol::encode_snapshot(saved)).at("risk"), m.at("risk"));
  }
  {
    PaperSession restored(manual.path);
    EXPECT_EQ(restored.snapshot(), saved);
    EXPECT_THROW(restored.execute(too_large), std::invalid_argument);
    EXPECT_THROW(restored.execute(submit("second", "buy", "open", "100")), std::invalid_argument);
    EXPECT_EQ(restored.snapshot(), saved);
  }
  Directory automated;
  PaperSession strategy(automated.path, m);
  const auto g = grant(m);
  strategy.execute(g);
  strategy.execute(advance("one"));
  const auto before = strategy.snapshot();
  EXPECT_THROW(strategy.execute(target(g, "too.large", 1, "2")), std::invalid_argument);
  EXPECT_EQ(strategy.snapshot(), before);
  strategy.execute(target(g, "valid", 1, "1"));
  EXPECT_EQ(strategy.snapshot().at("orders").size(), 1U);
}
TEST(PreTradeRisk, MissingRiskIsRejectedInsteadOfDefaulted) {
  auto m = manifest();
  m.erase("risk");
  EXPECT_THROW(protocol::encode_input(m), std::exception);
  Directory dir;
  EXPECT_THROW((PaperSession(dir.path, m)), std::exception);
  auto noncanonical = manifest();
  noncanonical["risk"]["max_order_quantity"] = "100.0";
  EXPECT_THROW(protocol::encode_input(noncanonical), std::exception);
  auto wire = protocol::encode_input(manifest());
  wire.clear_risk();
  EXPECT_THROW(protocol::decode_input(wire), std::exception);
}

TEST(PreTradeRisk, UnavailablePluginAndRejectedTargetCannotMutateExecution) {
  const auto spec = instrument();
  auto policy = std::make_shared<OrderLimits>(OrderLimitsConfig{d("1"), d("1"), 1});
  PaperExecution engine(
      d("1000"),
      {{{spec, costs()},
        {test::flat("2026-09-28", 100, "100"), test::flat("2026-09-28", 200, "100")}}},
      policy);
  engine.start();
  engine.advance();
  const auto before = engine.snapshot();
  EXPECT_THROW(engine.submit(order("one", Side::buy, "1", "100"), Offset::open),
               std::invalid_argument);
  EXPECT_EQ(engine.snapshot(), before);
  policy->start();
  engine.submit(order("one", Side::buy, "1", "100"), Offset::open);
  const auto accepted = engine.snapshot();
  EXPECT_THROW(engine.reconcile_long_target("two", spec.id, d("2"), d("100")),
               std::invalid_argument);
  EXPECT_EQ(engine.snapshot(), accepted);
  policy->stop();
  EXPECT_THROW(engine.reconcile_long_target("three", spec.id, d("1"), d("100")),
               std::invalid_argument);
  EXPECT_EQ(engine.snapshot(), accepted);
}

namespace {
// Participation: volume 10 per available lot.
std::vector<MarketBar> settlement_bars() {
  return {test::flat("2026-09-25", 100, "100", "30"), test::flat("2026-09-25", 200, "99", "20"),
          test::flat("2026-09-28", 300, "106", "30"), test::flat("2026-09-28", 400, "105", "10"),
          test::flat("2026-09-28", 500, "107", "20"), test::flat("2026-09-28", 600, "108", "10"),
          test::flat("2026-09-28", 700, "109", "10")};
}
void mixed_longs(PaperExecution& engine) {
  engine.start();
  engine.advance();
  engine.submit(order("old.open", Side::buy, "2", "100"), Offset::open);
  engine.advance();
  engine.settle_day_end({d("105")});
  engine.advance();
  engine.submit(order("new.open", Side::buy, "1", "106"), Offset::open);
  engine.advance();
}
} // namespace
TEST(PaperExecution, ScheduledSettlementCarriesBasisAndClosesBucketsWithDistinctFees) {
  PaperExecution engine(d("1000"), {{{instrument(), costs()}, settlement_bars()}}, risk());
  mixed_longs(engine);
  const auto positions = engine.snapshot().at("positions");
  ASSERT_EQ(positions.size(), 2U);
  EXPECT_EQ(positions[0].at("bucket"), "yesterday");
  EXPECT_EQ(positions[0].at("basis"), "105");
  EXPECT_EQ(positions[1].at("bucket"), "today");
  engine.reconcile_long_target("close", instrument().id, d("0"), d("104"));
  auto state = engine.snapshot();
  EXPECT_EQ(state.at("orders")[2].at("id"), "close.yesterday");
  EXPECT_EQ(state.at("orders")[2].at("offset"), "close_yesterday");
  EXPECT_EQ(state.at("orders")[3].at("id"), "close.today");
  EXPECT_EQ(state.at("orders")[3].at("offset"), "close_today");
  EXPECT_EQ(state.at("frozen"), "11");
  engine.advance();
  EXPECT_EQ(engine.snapshot().at("orders")[3].at("filled"), "0");
  engine.advance();
  state = engine.snapshot();
  EXPECT_TRUE(state.at("positions").empty());
  EXPECT_EQ(state.at("balance"), "1173");
  EXPECT_EQ(state.at("fees"), "17");
  EXPECT_EQ(state.at("realized"), "190");
  EXPECT_EQ(state.at("unrealized"), "0");
}
TEST(PaperExecution, SecondBucketRiskRejectionPreservesOriginalOrdersAndAccount) {
  auto policy = std::make_shared<OrderLimits>(OrderLimitsConfig{d("100"), d("100"), 1});
  policy->start();
  PaperExecution engine(d("1000"), {{{instrument(), costs()}, settlement_bars()}}, policy);
  mixed_longs(engine);
  engine.submit(order("existing", Side::sell, "1", "200"), Offset::close_today);
  const auto before = engine.snapshot();
  EXPECT_THROW(engine.reconcile_long_target("split", instrument().id, d("0"), d("104")),
               std::invalid_argument);
  EXPECT_EQ(engine.snapshot(), before);
  // Only the yesterday bucket is needed: one order fits the same risk limit.
  engine.reconcile_long_target("reduce", instrument().id, d("1"), d("104"));
  EXPECT_EQ(engine.snapshot().at("orders").back().at("offset"), "close_yesterday");
  engine.advance();
  ASSERT_EQ(engine.snapshot().at("positions").size(), 1U);
  EXPECT_EQ(engine.snapshot().at("positions")[0].at("bucket"), "today");
}
TEST(PaperExecution, DayEndSettlementRejectsWrongPositionAndPendingOrdersAtomically) {
  PaperExecution engine(d("1000"), {{{instrument(), costs()}, settlement_bars()}}, risk());
  engine.start();
  const auto empty = engine.snapshot();
  EXPECT_THROW(engine.settle_day_end({d("105")}), std::invalid_argument);
  EXPECT_EQ(engine.snapshot(), empty);
  engine.advance();
  engine.submit(order("open", Side::buy, "1", "100"), Offset::open);
  const auto pending = engine.snapshot();
  // The first bar is not the last of its day.
  EXPECT_THROW(engine.settle_day_end({d("105")}), std::invalid_argument);
  EXPECT_EQ(engine.snapshot(), pending);
  engine.advance();
  const auto filled = engine.snapshot();
  EXPECT_THROW(engine.settle_day_end({d("105.5")}), std::invalid_argument);
  EXPECT_EQ(engine.snapshot(), filled);
  engine.settle_day_end({d("105")});
  const auto settled = engine.snapshot();
  EXPECT_THROW(engine.settle_day_end({d("106")}), std::invalid_argument) << "settled once per day";
  EXPECT_EQ(engine.snapshot(), settled);
  engine.advance();
  EXPECT_EQ(engine.snapshot().at("unrealized"), "10");
  for (int i = 0; i < 4; ++i)
    engine.advance();
  const auto finished = engine.snapshot();
  EXPECT_THROW(engine.settle_day_end({d("105")}), std::invalid_argument);
  EXPECT_EQ(engine.snapshot(), finished);
}

TEST(PaperExecution, ChildIdentityConflictCannotPartiallyReplaceMixedBucketOrders) {
  PaperExecution engine(d("1000"), {{{instrument(), costs()}, settlement_bars()}}, risk());
  mixed_longs(engine);
  engine.submit(order("split.today", Side::sell, "1", "200"), Offset::close_today);
  const auto before = engine.snapshot();
  EXPECT_THROW(engine.reconcile_long_target("", instrument().id, d("0"), d("104")),
               std::invalid_argument);
  EXPECT_EQ(engine.snapshot(), before);
  EXPECT_THROW(engine.reconcile_long_target("split", instrument().id, d("0"), d("104")),
               std::invalid_argument);
  EXPECT_EQ(engine.snapshot(), before);
}

namespace {
Json replay_settle_command(std::string id, int day) {
  return {{"request_id", id}, {"action", "replay_settle"}, {"day_index", day}};
}
} // namespace
TEST(PaperSession, DayEndSettlementFencesAdvanceAndSurvivesRestartExactlyOnce) {
  Directory dir;
  Json before;
  {
    PaperSession session(dir.path, manifest());
    EXPECT_THROW(session.execute(replay_settle_command("early", 0)), std::invalid_argument);
    session.execute(advance("a1"));
    auto order = submit("open", "buy", "open", "100");
    order["quantity"] = "2";
    session.execute(order);
    EXPECT_THROW(session.execute(replay_settle_command("early", 0)), std::invalid_argument);
    session.execute(advance("a2"));
    before = session.snapshot();
    EXPECT_EQ(before.at("orders").at(0).at("state"), "cancelled");
    EXPECT_THROW(session.execute(advance("a3")), std::invalid_argument);
    EXPECT_THROW(session.execute(submit("cross", "buy", "open", "99")), std::invalid_argument);
    EXPECT_THROW(
        session.execute(Json({{"request_id", "manual"}, {"action", "settle"}, {"price", "105"}})),
        std::invalid_argument);
    EXPECT_THROW(session.execute(replay_settle_command("wrong", 1)), std::invalid_argument);
    EXPECT_EQ(session.snapshot(), before);
    const auto settle = replay_settle_command("day0", 0);
    EXPECT_EQ(protocol::decode_command(protocol::encode_command(settle)), settle);
    session.execute(settle);
    before = session.snapshot();
    session.execute(settle);
    EXPECT_EQ(session.snapshot(), before);
    EXPECT_EQ(before.at("balance"), "1058");
    EXPECT_EQ(before.at("positions").at(0).at("bucket"), "yesterday");
    EXPECT_EQ(protocol::decode_snapshot(protocol::encode_snapshot(before)).at("replay"),
              before.at("replay"));
  }
  PaperSession recovered(dir.path);
  EXPECT_EQ(recovered.snapshot(), before);
  recovered.execute(replay_settle_command("day0", 0));
  EXPECT_EQ(recovered.snapshot(), before);
  EXPECT_THROW(recovered.execute(replay_settle_command("duplicate-day", 0)), std::invalid_argument);
  recovered.execute(advance("a3"));
  recovered.execute(replay_settle_command("day1", 1));
  const auto final = recovered.snapshot();
  EXPECT_EQ(final.at("balance"), "1208");
  EXPECT_EQ(final.at("replay").at("settled_days"), 2);
  EXPECT_EQ(final.at("positions").at(0).at("basis"), "120");
  recovered.execute(replay_settle_command("day1", 1));
  EXPECT_EQ(recovered.snapshot(), final);
  EXPECT_THROW(recovered.execute(replay_settle_command("extra", 2)), std::invalid_argument);
  EXPECT_EQ(recovered.snapshot(), final);
}
TEST(PaperSession, ScheduledStrategyCannotPlaceOrdersAcrossSessionOrSkipSettlement) {
  Directory dir;
  PaperSession session(dir.path, manifest());
  const auto revision = manifest().at("contracts").at(0).at("dataset").at("revision");
  session.execute({{"request_id", "grant"},
                   {"action", "strategy_grant"},
                   {"grant_id", "g"},
                   {"strategy_id", "s"},
                   {"stream_id", "stream"},
                   {"dataset_revision", revision},
                   {"max_quantity", "2"}});
  session.execute(advance("a1"));
  session.execute(advance("a2"));
  session.execute({{"request_id", "intent"},
                   {"action", "strategy_target"},
                   {"grant_id", "g"},
                   {"strategy_id", "s"},
                   {"stream_id", "stream"},
                   {"dataset_revision", revision},
                   {"sequence", 2},
                   {"timestamp_ns", "200"},
                   {"venue", "SHFE"},
                   {"symbol", "rb2610"},
                   {"target_quantity", "1"}});
  EXPECT_TRUE(session.snapshot().at("orders").empty());
  EXPECT_EQ(session.snapshot().at("strategy").at("last_sequence"), 2);
  EXPECT_THROW(session.execute(advance("a3")), std::invalid_argument);
  session.execute(replay_settle_command("day0", 0));
  session.execute(advance("a3"));
  session.execute(replay_settle_command("day1", 1));
  EXPECT_EQ(session.snapshot().at("replay").at("settled_days"), 2);
}
TEST(FuturesAccount, TypedQueriesMatchSnapshotAndRejectedFillsLeaveLedgerUntouched) {
  FuturesAccount account(d("100000"), {{instrument(), costs()}});
  account.mark(instrument().id, d("100"));
  account.submit(order("o1", Side::buy, "2", "100"), Offset::open);
  ASSERT_TRUE(account.fill({"e1", "o1", d("1"), d("100")}));
  account.mark(instrument().id, d("103"));
  const auto before = account.snapshot();
  EXPECT_EQ(before.at("available"), account.available().str());
  EXPECT_EQ(before.at("frozen"), account.frozen().str());
  EXPECT_EQ(before.at("margin"), account.margin().str());
  EXPECT_EQ(before.at("unrealized"), account.unrealized().str());
  EXPECT_EQ(before.at("marks").at(0).at("mark"), account.last_mark(instrument().id).str());
  EXPECT_TRUE(account.has_working_orders());
  // Limit violation, conflicting duplicate and unknown order all fail atomically.
  EXPECT_THROW(account.fill({"e2", "o1", d("1"), d("101")}), std::invalid_argument);
  EXPECT_THROW(account.fill({"e1", "o1", d("1"), d("99")}), std::invalid_argument);
  EXPECT_THROW(account.fill({"e3", "missing", d("1"), d("100")}), std::invalid_argument);
  EXPECT_THROW(account.submit(order("o1", Side::buy, "1", "100"), Offset::open),
               std::invalid_argument);
  EXPECT_EQ(account.snapshot(), before);
  EXPECT_FALSE(account.fill({"e1", "o1", d("1"), d("100")}));
}
TEST(PaperExecution, RestingOrdersDoNotMakeReplayQuadratic) {
  // Smaller under sanitizers: the point is linear scaling, not absolute size.
#ifdef ASTERION_SANITIZED
  constexpr std::size_t events = 2000, resting = 400;
#else
  constexpr std::size_t events = 10000, resting = 2000;
#endif
  std::vector<MarketBar> ticks;
  for (std::size_t i = 0; i < events; ++i)
    ticks.push_back(test::flat("2026-09-28", static_cast<std::int64_t>(i + 1), "100"));
  auto limits = std::make_shared<OrderLimits>(OrderLimitsConfig{d("10"), d("100000"), resting});
  limits->start();
  PaperExecution execution(d("100000000"), {{{instrument(), costs()}, std::move(ticks)}}, limits);
  execution.start();
  execution.advance();
  for (std::size_t i = 0; i < resting; ++i)
    execution.submit(order("rest." + std::to_string(i), Side::buy, "1", "90"), Offset::open);
  const auto started = std::chrono::steady_clock::now();
  while (execution.cursor() < execution.size())
    execution.advance();
  // Previously every event copied the full ledger (O(events x orders)).
  EXPECT_LT(std::chrono::steady_clock::now() - started,
            asterion::testing_support::bound(std::chrono::seconds(10)));
  EXPECT_EQ(execution.account().fills().size(), 0U);
  EXPECT_EQ(execution.account().orders().size(), resting);
}
namespace {
void recorded_session(const std::filesystem::path& path) {
  PaperSession session(path, manifest());
  session.execute(advance("tick1"));
  session.execute(submit("buy", "buy", "open", "100"));
  session.execute(advance("tick2"));
}
} // namespace
TEST(PaperSession, JournalHeaderPinsFormatAndEngineSemantics) {
  Directory directory;
  recorded_session(directory.path);
  const auto header_file = test::journal_record(directory.path, 0);
  const auto header = read_record(header_file);
  EXPECT_EQ(header.at("format"), 5);
  EXPECT_EQ(header.at("manifest"), manifest());
  auto foreign = header;
  foreign["engine"] = "asterion.paper-futures.v0";
  write_record(header_file, foreign);
  EXPECT_THROW(PaperSession{directory.path}, std::invalid_argument);
  // A header without the format and engine fields is refused and left as is.
  write_record(header_file, manifest());
  EXPECT_THROW(PaperSession{directory.path}, std::invalid_argument);
  EXPECT_EQ(read_record(header_file), manifest());
  write_record(header_file, header);
  EXPECT_NO_THROW(PaperSession{directory.path});
}
TEST(PaperSession, ReplayDivergingFromRecordedOutcomeIsRefused) {
  Directory directory;
  recorded_session(directory.path);
  const auto file = test::journal_record(directory.path, 3);
  auto record = read_record(file);
  ASSERT_EQ(record.at("outcome").at("fills"), 1);
  auto tampered = record;
  tampered["outcome"]["balance"] = "999999";
  write_record(file, tampered);
  EXPECT_THROW(PaperSession{directory.path}, std::invalid_argument);
  write_record(file, record);
  PaperSession recovered(directory.path);
  EXPECT_EQ(recovered.snapshot().at("fills").size(), 1U);
}
TEST(PaperSession, FailedCommitAfterInPlaceMutationRestoresCommittedState) {
  Directory directory;
  {
    PaperSession session(directory.path, manifest());
    session.execute(advance("tick1"));
    session.execute(submit("rest", "buy", "open", "1"));
    const auto committed = session.snapshot();
    ASSERT_EQ(committed.at("orders").at(0).at("state"), "accepted");
    // The cancel is applied in place, then the commit fails: the published
    // state must be the committed one, not the unjournaled cancel.
    asterion::sqlite::fail_next_commits_for_testing(1);
    const Json cancel{{"request_id", "cancel"}, {"action", "cancel"}, {"order_id", "order.rest"}};
    EXPECT_THROW(session.execute(cancel), std::runtime_error);
    auto shown = session.snapshot();
    EXPECT_EQ(shown.at("storage_state"), "recovery_required");
    shown["storage_state"] = committed.at("storage_state");
    EXPECT_EQ(shown, committed);
    asterion::sqlite::fail_next_commits_for_testing(0);
  }
  PaperSession recovered(directory.path);
  EXPECT_EQ(recovered.snapshot().at("orders").at(0).at("state"), "accepted");
  recovered.execute({{"request_id", "cancel"}, {"action", "cancel"}, {"order_id", "order.rest"}});
  EXPECT_EQ(recovered.snapshot().at("orders").at(0).at("state"), "cancelled");
}
TEST(PaperSession, RejectedCommandLeavesStateAndIdempotencyUntouched) {
  Directory directory;
  PaperSession session(directory.path, manifest());
  session.execute(advance("tick1"));
  const auto before = session.snapshot();
  EXPECT_THROW(session.execute(submit("bad", "buy", "close_today", "100")), std::invalid_argument);
  EXPECT_EQ(session.snapshot(), before);
  // A rejected request identity was never committed and may be reused.
  session.execute(submit("bad", "buy", "open", "100"));
  EXPECT_EQ(session.snapshot().at("orders").size(), 1U);
}
namespace {
Instrument venue_instrument(const char* venue, const char* symbol) {
  return {{venue, symbol}, AssetClass::futures, "CNY", d("1"), d("1"), d("10")};
}
LimitOrder venue_order(const Instrument& spec, std::string id, Side side, const char* quantity,
                       const char* price) {
  return {std::move(id), spec.id, side, d(quantity), d(price)};
}
} // namespace
TEST(FuturesCosts, NotionalRatesRoundHalfUpToTheCentAndMarginFollowsTheMark) {
  // Per-lot 2 + 0.0023% of notional to open; 12% margin rate, no per-lot margin.
  FuturesCosts costs{d("0"), d("2"), d("0"), d("0"), d("0.12"), d("0.000023"), d("0"), d("0")};
  EXPECT_EQ(costs.fee(Offset::open, d("1"), d("3510.5"), d("10")).str(), "2.81"); // 2 + 0.807415
  EXPECT_EQ(costs.margin(d("2"), d("3510.5"), d("10")).str(), "8425.2");
  EXPECT_THROW(costs.fee(Offset::close, d("1"), d("1"), d("10")), std::invalid_argument);
  EXPECT_THROW((FuturesCosts{d("0"), d("0"), d("0"), d("0")}.validate()), std::invalid_argument)
      << "some margin is required";
  EXPECT_THROW((FuturesCosts{d("0"), d("0"), d("0"), d("0"), d("1")}.validate()),
               std::invalid_argument)
      << "rates are below 1";
  FuturesAccount account(d("100000"), {{venue_instrument("SHFE", "rb2610"), costs}});
  account.mark(venue_instrument("SHFE", "rb2610").id, d("3500"));
  account.submit(venue_order(account.contracts().front().instrument, "o", Side::buy, "1", "3500"),
                 Offset::open);
  ASSERT_TRUE(account.fill({"e", "o", d("1"), d("3500")}));
  EXPECT_EQ(account.fees().str(), "2.81"); // 2 + 0.805 rounds half-up
  EXPECT_EQ(account.margin().str(), "4200");
  account.mark(venue_instrument("SHFE", "rb2610").id, d("3600"));
  EXPECT_EQ(account.margin().str(), "4320") << "position margin follows the mark";
}
TEST(FuturesAccount, ExchangeClosePoliciesAssignBucketsAndFees) {
  const FuturesCosts costs{d("100"), d("1"), d("5"), d("2")};
  EXPECT_EQ(close_policy("SHFE"), ClosePolicy::explicit_buckets);
  EXPECT_EQ(close_policy("INE"), ClosePolicy::explicit_buckets);
  EXPECT_EQ(close_policy("CFFEX"), ClosePolicy::today_first);
  EXPECT_EQ(close_policy("DCE"), ClosePolicy::yesterday_first);
  EXPECT_EQ(close_policy("CZCE"), ClosePolicy::yesterday_first);
  EXPECT_EQ(close_policy("GFEX"), ClosePolicy::yesterday_first);
  EXPECT_EQ(close_policy("UNKNOWN"), ClosePolicy::explicit_buckets);
  for (const auto [venue, today_first] : {std::pair{"DCE", false}, std::pair{"CFFEX", true}}) {
    FuturesAccount account(d("100000"), {{venue_instrument(venue, "x2609"), costs}});
    const auto& spec = account.contracts().front().instrument;
    account.mark(venue_instrument(venue, "x2609").id, d("100"));
    account.submit(venue_order(spec, "y", Side::buy, "2", "100"), Offset::open);
    ASSERT_TRUE(account.fill({"ey", "y", d("2"), d("100")}));
    account.settle({d("100")}); // two yesterday lots
    account.submit(venue_order(spec, "t", Side::buy, "2", "100"), Offset::open);
    ASSERT_TRUE(account.fill({"et", "t", d("2"), d("100")})); // two today lots
    EXPECT_THROW(
        account.submit(venue_order(spec, "bad", Side::sell, "1", "100"), Offset::close_today),
        std::invalid_argument)
        << venue << " assigns buckets itself";
    EXPECT_THROW(account.submit(venue_order(spec, "big", Side::sell, "5", "100"), Offset::close),
                 std::invalid_argument);
    const auto before = account.fees();
    account.submit(venue_order(spec, "c", Side::sell, "3", "100"), Offset::close);
    ASSERT_TRUE(account.fill({"ec", "c", d("3"), d("100")}));
    // First bucket fully (2 lots), second bucket 1 lot; each at its own fee.
    const auto expected = today_first ? d("2") * d("5") + d("1") * d("2")  // 12
                                      : d("2") * d("2") + d("1") * d("5"); // 9
    EXPECT_EQ((account.fees() - before).str(), expected.str()) << venue;
    ASSERT_EQ(account.positions().size(), 1U);
    EXPECT_EQ(account.positions()[0].today, !today_first) << venue;
  }
  FuturesAccount shfe(d("100000"), {{venue_instrument("SHFE", "rb2610"), costs}});
  shfe.mark(venue_instrument("SHFE", "rb2610").id, d("100"));
  shfe.submit(venue_order(shfe.contracts().front().instrument, "o", Side::buy, "1", "100"),
              Offset::open);
  ASSERT_TRUE(shfe.fill({"e", "o", d("1"), d("100")}));
  EXPECT_THROW(
      shfe.submit(venue_order(shfe.contracts().front().instrument, "c", Side::sell, "1", "100"),
                  Offset::close),
      std::invalid_argument)
      << "SHFE requires an explicit bucket";
}
TEST(PaperExecution, NextBarFillsAreConservativeAndCappedByParticipation) {
  // Buy limit 101: a gap down fills at the open; later at the limit itself.
  // Sell limit 105 fills only when the high reaches it. 10% of bar volume.
  PaperExecution execution(d("100000"),
                           {{{instrument(), costs()},
                             {test::flat("2026-09-28", 1, "100", "100"),
                              test::bar("2026-09-28", 2, "99", "102", "98", "101", "30"),
                              test::bar("2026-09-28", 3, "103", "104", "100", "103", "50"),
                              test::bar("2026-09-28", 4, "103", "104", "102", "104", "50"),
                              test::bar("2026-09-28", 5, "104", "106", "103", "105", "50")}}},
                           risk());
  execution.start();
  execution.advance();
  execution.submit(order("buy", Side::buy, "5", "101"), Offset::open);
  EXPECT_TRUE(execution.account().fills().empty()) << "never on the bar it was placed after";
  execution.advance(); // participation 3 lots at min(open 99, limit 101)
  ASSERT_EQ(execution.account().fills().size(), 1U);
  EXPECT_EQ(execution.account().fills()[0].quantity.str(), "3");
  EXPECT_EQ(execution.account().fills()[0].price.str(), "99");
  execution.advance(); // low 100 reaches 101: the rest at the limit, not the better open
  ASSERT_EQ(execution.account().fills().size(), 2U);
  EXPECT_EQ(execution.account().fills()[1].quantity.str(), "2");
  EXPECT_EQ(execution.account().fills()[1].price.str(), "101");
  execution.submit(order("sell", Side::sell, "5", "105"), Offset::close_today);
  execution.advance(); // high 104 never reaches 105
  EXPECT_EQ(execution.account().fills().size(), 2U);
  execution.advance(); // high 106 reaches it: max(open 104, limit 105)
  ASSERT_EQ(execution.account().fills().size(), 3U);
  EXPECT_EQ(execution.account().fills()[2].price.str(), "105");
  EXPECT_TRUE(execution.account().positions().empty());
}

TEST(SqliteJournal, OnlyDeclaredRealSidecarDirectoriesAreAccepted) {
  Directory directory;
  std::filesystem::create_directory(directory.path / "plugins");
  {
    SqliteJournal plain(directory.path);
    EXPECT_THROW(plain.start(), std::invalid_argument);
  }
  {
    SqliteJournal allowed(directory.path, {"plugins"});
    allowed.start();
    allowed.append({{"test", 1}});
  }
  std::filesystem::rename(directory.path / "plugins", directory.path / "elsewhere");
  std::filesystem::create_directory_symlink(directory.path / "elsewhere",
                                            directory.path / "plugins");
  {
    SqliteJournal links(directory.path, {"plugins", "elsewhere"});
    EXPECT_THROW(links.start(), std::invalid_argument);
  }
  std::filesystem::remove(directory.path / "plugins");
  std::ofstream(directory.path / "plugins") << "not a directory";
  {
    SqliteJournal files(directory.path, {"plugins", "elsewhere"});
    EXPECT_THROW(files.start(), std::invalid_argument);
  }
  EXPECT_THROW((SqliteJournal(directory.path, {"../outside"})), std::invalid_argument);
  EXPECT_THROW((SqliteJournal(directory.path, {"journal.sqlite"})), std::invalid_argument);
}
TEST(PaperSession, RecoveryRequiresOriginalRiskArtifactAndPreservesLedgerOnFailure) {
  Directory directory;
  recorded_session(directory.path);
  const auto header_file = test::journal_record(directory.path, 0);
  const auto header = read_record(header_file);
  const auto plugin = risk_providers::Module::filename(directory.path / "plugins");
  ASSERT_TRUE(std::filesystem::is_regular_file(plugin));
  ASSERT_EQ(header.at("risk_artifact").get<std::string>().size(), 64U);
  // Never modify an inode which may still be mapped by the dynamic loader.
  const auto saved = plugin.parent_path() / "original";
  std::filesystem::rename(plugin, saved);
  EXPECT_THROW(PaperSession{directory.path}, std::exception);
  EXPECT_EQ(read_record(header_file), header);
  std::ofstream(plugin) << "corrupt plugin fixture";
  EXPECT_THROW(PaperSession{directory.path}, std::exception);
  EXPECT_EQ(read_record(header_file), header);
  EXPECT_EQ(std::filesystem::file_size(plugin), 22U);
  std::filesystem::remove(plugin);
  std::filesystem::create_symlink(saved, plugin);
  EXPECT_THROW(PaperSession{directory.path}, std::exception);
  std::filesystem::remove(plugin);
  std::filesystem::rename(saved, plugin);
  auto wrong = header;
  wrong["risk_artifact"] = std::string(64, '0');
  write_record(header_file, wrong);
  EXPECT_THROW(PaperSession{directory.path}, std::exception);
  EXPECT_EQ(read_record(header_file), wrong);
  write_record(header_file, header);
  PaperSession recovered(directory.path);
  EXPECT_EQ(recovered.snapshot().at("fills").size(), 1U);
}

TEST(PaperSession, RecoveryUsesOwnedRiskEvenWhenDefaultPluginIsUnavailable) {
  Directory ledger, empty_plugins, new_ledger;
  recorded_session(ledger.path);
  struct RestorePlugins {
    std::filesystem::path original = native_plugin_directory();
    ~RestorePlugins() { configure_native_plugins(original); }
  } restore;
  configure_native_plugins(empty_plugins.path);
  EXPECT_THROW((PaperSession(new_ledger.path, manifest())), std::exception);
  EXPECT_EQ(test::journal_size(new_ledger.path), 0U);
  PaperSession recovered(ledger.path);
  EXPECT_EQ(recovered.snapshot().at("fills").size(), 1U);
}
TEST(PaperSession, ResearchSizedDatasetsAreRefusedBeforeWriting) {
  std::vector<MarketBar> bars;
  for (std::int64_t i = 0; i < 20001; ++i)
    bars.push_back(test::flat("2026-09-25", 1790384400000000000LL + i * 60000000000LL, "100"));
  auto large = manifest();
  large["contracts"][0]["dataset"] = test::dataset_json(bars);
  Directory directory;
  try {
    PaperSession session(directory.path, large);
    FAIL() << "a dataset above the session limit was accepted";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("at most 20000 bars"), std::string::npos);
  }
  EXPECT_EQ(test::journal_size(directory.path), 0U);
}
TEST(BarDataset, ResearchLimitIsTwoHundredThousandBars) {
  std::vector<MarketBar> bars;
  for (std::int64_t i = 0; i < 200000; ++i)
    bars.push_back(test::flat("2026-09-25", 1790384400000000000LL + i * 60000000000LL, "100"));
  auto dataset = test::dataset(bars);
  EXPECT_EQ(dataset.bars_size(), 200000);
  *dataset.add_bars() = protocol::encode_bar(
      test::flat("2026-09-25", 1790384400000000000LL + 200000 * 60000000000LL, "100"));
  dataset.set_revision(protocol::bar_dataset_revision(dataset));
  EXPECT_THROW(protocol::validate_bar_dataset(dataset), std::invalid_argument);
}
namespace {
// Two contracts over the same two trading days; their bars interleave by time.
Json portfolio_manifest(std::vector<MarketBar> second_bars) {
  auto value = manifest();
  const auto hc = test::contract("SHFE", "hc2610", "hc", "2026-10");
  value["contracts"].push_back(
      {{"dataset", protocol::decode_bar_dataset(test::dataset(
                       second_bars, {{"2026-09-25", d("205")}, {"2026-09-28", d("215")}}, hc))},
       {"costs", cost_json()}});
  return value;
}
std::vector<MarketBar> hc_bars() {
  return {test::flat("2026-09-25", 150, "200"), test::flat("2026-09-25", 250, "201"),
          test::flat("2026-09-28", 350, "210")};
}
Json submit_hc(std::string id, std::string price) {
  auto command = submit(std::move(id), "buy", "open", std::move(price));
  command["symbol"] = "hc2610";
  return command;
}
Json settle_day(std::string id, int day) {
  return {{"request_id", std::move(id)}, {"action", "replay_settle"}, {"day_index", day}};
}
} // namespace
TEST(PaperPortfolio, ContractsFillOnlyOnTheirOwnBarsAndSettleTogether) {
  Directory dir;
  Json saved;
  {
    PaperSession session(dir.path, portfolio_manifest(hc_bars()));
    EXPECT_EQ(session.snapshot().at("contracts").size(), 2U);
    session.execute(advance("rb.100"));
    session.execute(submit("rb", "buy", "open", "100"));
    auto unknown = submit_hc("unknown", "200");
    unknown["symbol"] = "ag2612";
    EXPECT_THROW(session.execute(unknown), std::invalid_argument);
    session.execute(advance("hc.150")); // an hc bar never fills the rb order
    EXPECT_TRUE(session.snapshot().at("fills").empty());
    session.execute(submit_hc("hc", "201"));
    session.execute(advance("rb.200"));
    ASSERT_EQ(session.snapshot().at("fills").size(), 1U);
    EXPECT_EQ(session.snapshot().at("fills")[0].at("symbol"), "rb2610");
    EXPECT_EQ(session.snapshot().at("fills")[0].at("price"), "99");
    session.execute(advance("hc.250")); // last bar of the first day across both contracts
    ASSERT_EQ(session.snapshot().at("fills").size(), 2U);
    EXPECT_EQ(session.snapshot().at("fills")[1].at("symbol"), "hc2610");
    EXPECT_THROW(session.execute(advance("early")), std::invalid_argument);
    session.execute(settle_day("day0", 0));
    // 1000 - 4 fees + rb (105 - 99) x 10 + hc (205 - 201) x 10
    EXPECT_EQ(session.snapshot().at("balance"), "1096");
    EXPECT_EQ(session.snapshot().at("margin"), "200");
    saved = session.snapshot();
  }
  PaperSession restored(dir.path);
  EXPECT_EQ(restored.snapshot(), saved);
  restored.execute(advance("rb.300"));
  restored.execute(advance("hc.350"));
  restored.execute(settle_day("day1", 1));
  // rb (120 - 105) x 10 + hc (215 - 205) x 10
  EXPECT_EQ(restored.snapshot().at("balance"), "1346");
  EXPECT_EQ(restored.snapshot().at("replay").at("settled_days"), 2);
  const auto contracts = restored.snapshot().at("contracts");
  EXPECT_EQ(contracts[0].at("mark"), "120");
  EXPECT_EQ(contracts[1].at("mark"), "215");
}
TEST(PaperPortfolio, ContractsOnDifferentTradingDaysAreRefusedBeforeWriting) {
  Directory dir;
  auto bars = hc_bars();
  bars.pop_back(); // hc lacks the second trading day
  EXPECT_THROW(PaperSession(dir.path, portfolio_manifest(bars)), std::invalid_argument);
  EXPECT_EQ(test::journal_size(dir.path), 0U);
  auto duplicate = manifest();
  duplicate["contracts"].push_back(duplicate["contracts"][0]);
  EXPECT_THROW(PaperSession(dir.path, duplicate), std::invalid_argument);
  EXPECT_EQ(test::journal_size(dir.path), 0U);
}
