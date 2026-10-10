#include "trading/journal_fixture.hpp"
#include "support/timing.hpp"
#include <gtest/gtest.h>
#include <asterion/domain/account.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/durable_file.hpp>
#include "paper_execution.hpp"
#include "order_limits.hpp"
#include "sqlite_journal.hpp"
#include <asterion/protocol/data.hpp>
#include "data/bar_fixture.hpp"
#include <chrono>
#include <fstream>
using namespace asterion;
namespace {
Decimal d(const char* value) {
  return Decimal::parse(value);
}
Instrument instrument() {
  return {{"SHFE", "rb2610"}, "CNY", d("1"), d("1"), d("10")};
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
TEST(SqliteJournal, RecoveryReadsConsecutiveBoundedPagesAndRejectsGaps) {
  Directory directory;
  const auto count = SqliteJournal::page_size * 2 + 3;
  {
    SqliteJournal journal(directory.path);
    journal.start();
    journal.append({{"sequence", 0}});
    for (std::size_t i = 1; i < count; ++i)
      journal.append({{"sequence", i},
                      {"trading_day", "20260928"},
                      {"broker_key", "1:1:" + std::to_string(i)}},
                     "command." + std::to_string(i), "order." + std::to_string(i));
  }
  {
    SqliteJournal recovered(directory.path);
    recovered.start();
    std::size_t next = 0;
    while (next < count) {
      const auto page = recovered.read(next);
      ASSERT_FALSE(page.empty());
      EXPECT_LE(page.size(), SqliteJournal::page_size);
      for (const auto& record : page)
        EXPECT_EQ(record.at("sequence"), next++);
    }
    EXPECT_TRUE(recovered.read(next).empty());
    std::uint64_t after = 0;
    while (after < count - 1) {
      const auto identities = recovered.order_identities("20260928", after);
      ASSERT_FALSE(identities.empty());
      EXPECT_LE(identities.size(), SqliteJournal::page_size);
      for (const auto& order : identities) {
        EXPECT_EQ(order.sequence, ++after);
        EXPECT_EQ(order.order_id, "order." + std::to_string(after));
      }
    }
    EXPECT_TRUE(recovered.order_identities("20260928", after).empty());
    EXPECT_TRUE(recovered.order_identities("20260929", 0).empty());
  }
  {
    sqlite::Database database(directory.path / "journal.sqlite");
    database.execute("DELETE FROM records WHERE sequence=64");
  }
  SqliteJournal broken(directory.path);
  EXPECT_THROW(broken.start(), std::invalid_argument);
}
TEST(SqliteJournal, AutomaticSegmentsPreserveOrderedHistoryAndCommandIdentity) {
  Directory directory;
  test::SmallJournalSegments budget(2);
  const Json header{{"account", "one"}};
  {
    SqliteJournal journal(directory.path);
    journal.start();
    journal.append(header);
    for (int i = 1; i <= 5; ++i)
      journal.append(
          {{"event", i}, {"trading_day", "20260928"}, {"broker_key", "1:1:" + std::to_string(i)}},
          "command." + std::to_string(i), "order." + std::to_string(i));
    EXPECT_EQ(journal.capacity().segment_count, 2U);
    EXPECT_EQ(journal.capacity().records_used, 2U);
    EXPECT_EQ(journal.capacity().total_records, 6U);
  }
  SqliteJournal recovered(directory.path);
  recovered.start();
  EXPECT_EQ(recovered.read(0), std::vector<Json>{header});
  std::uint64_t sequence = 1;
  while (sequence < recovered.capacity().total_records)
    for (const auto& event : recovered.read(sequence)) {
      EXPECT_EQ(event.at("event"), sequence);
      EXPECT_EQ(recovered.command("command." + std::to_string(sequence)), event);
      EXPECT_EQ(recovered.order_sequence("order." + std::to_string(sequence)), sequence);
      EXPECT_EQ(recovered.order("order." + std::to_string(sequence)), event);
      ++sequence;
    }
  EXPECT_TRUE(recovered.read(sequence).empty());
  recovered.append({{"order_not_sent", "order.1"}, {"error_code", -1005}}, {}, {}, "order.1");
  EXPECT_EQ(recovered.order_identities("20260928", 0).size(), 4U);
  ASSERT_TRUE(recovered.order_index("order.1"));
  EXPECT_EQ(recovered.order_index("order.1")->excluded_by, 6U);
  EXPECT_THROW(
      recovered.append({{"event", 6}, {"trading_day", "20260928"}, {"broker_key", "1:1:6"}},
                       "command.6", "order.1"),
      std::runtime_error);
  EXPECT_FALSE(recovered.command_sequence("command.6"));
  EXPECT_EQ(recovered.capacity().total_records, 7U);
}
TEST(SqliteJournal, FailedSegmentSwitchPreservesOriginalEventsAndUnreferencedExports) {
  Directory directory;
  test::SmallJournalSegments budget(1);
  const Json event{{"event", 1}};
  {
    SqliteJournal journal(directory.path);
    journal.start();
    journal.append({{"account", "one"}});
    journal.append(event, "one");
    // Export succeeds; duplicate identity rejects the transaction that registers
    // the segment, retires active rows and appends the new command.
    EXPECT_THROW(journal.append({{"event", 2}}, "one"), std::runtime_error);
    EXPECT_EQ(journal.capacity().segment_count, 0U);
    EXPECT_EQ(journal.read(1), std::vector<Json>{event});
    EXPECT_EQ(journal.command("one"), event);
    EXPECT_THROW(journal.append({{"event", 3}}, "three"), std::runtime_error);
  }
  EXPECT_FALSE(std::filesystem::is_empty(directory.path / "archives"));
  SqliteJournal recovered(directory.path);
  recovered.start();
  EXPECT_EQ(recovered.capacity().total_records, 2U);
  EXPECT_EQ(recovered.command("one"), event);
  recovered.append({{"event", 3}}, "three");
  EXPECT_EQ(recovered.capacity().segment_count, 1U);
  EXPECT_EQ(recovered.capacity().total_records, 3U);
}
TEST(SqliteJournal, MissingOrModifiedSegmentRejectsRecovery) {
  Directory directory;
  test::SmallJournalSegments budget(1);
  {
    SqliteJournal journal(directory.path);
    journal.start();
    journal.append({{"account", "one"}});
    journal.append({{"event", 1}});
    journal.append({{"event", 2}});
  }
  const auto segment = std::filesystem::directory_iterator(directory.path / "archives")->path();
  const auto held = segment.string() + ".held";
  const auto active_digest = sha256_file(directory.path / "journal.sqlite");
  std::filesystem::rename(segment, held);
  {
    SqliteJournal missing(directory.path);
    EXPECT_THROW(missing.start(), std::invalid_argument);
    EXPECT_EQ(sha256_file(directory.path / "journal.sqlite"), active_digest);
  }
  std::filesystem::rename(held, segment);
  {
    sqlite::Database database(segment);
    database.execute("UPDATE records SET body='{}'");
  }
  SqliteJournal changed(directory.path);
  EXPECT_THROW(changed.start(), std::invalid_argument);
  EXPECT_EQ(sha256_file(directory.path / "journal.sqlite"), active_digest);
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
    EXPECT_EQ(journal.capacity().total_records, 2U);
  }
  {
    SqliteJournal recovered(directory.path);
    recovered.start();
    EXPECT_EQ(recovered.capacity().total_records, 2U);
    EXPECT_EQ(recovered.read(1).back(), (Json{{"test", 2}}));
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

TEST(PaperExecution, FailedTargetReplacementKeepsExistingOrdersAndReserves) {
  PaperExecution engine(
      d("200"),
      {{{instrument(), costs()},
        {test::flat("2026-09-28", 100, "100"), test::flat("2026-09-28", 200, "99")}}},
      risk());
  engine.start();
  engine.advance();
  engine.reconcile_target("one", instrument().id, d("1"), d("100"));
  const auto before = engine.snapshot();
  EXPECT_THROW(engine.reconcile_target("two", instrument().id, d("2"), d("100")),
               std::invalid_argument);
  EXPECT_EQ(engine.snapshot(), before);
  engine.advance();
  EXPECT_EQ(engine.snapshot().at("fills").size(), 1U);
}

TEST(PaperExecution, TargetsHoldShortsAndReverseByClosingBeforeOpening) {
  PaperExecution engine(
      d("1000"),
      {{{instrument(), costs()},
        {test::flat("2026-09-28", 100, "100", "30"), test::flat("2026-09-28", 200, "100", "30"),
         test::flat("2026-09-28", 300, "96", "30"), test::flat("2026-09-28", 400, "96", "30"),
         test::flat("2026-09-28", 500, "98", "30")}}},
      risk());
  engine.start();
  engine.advance();
  engine.reconcile_target("short", instrument().id, d("-2"), d("100"));
  auto state = engine.snapshot();
  EXPECT_EQ(state.at("orders")[0].at("side"), "sell");
  EXPECT_EQ(state.at("orders")[0].at("offset"), "open");
  engine.advance();
  engine.advance();
  state = engine.snapshot();
  ASSERT_EQ(state.at("positions").size(), 1U);
  EXPECT_EQ(state.at("positions")[0].at("side"), "sell");
  EXPECT_EQ(state.at("positions")[0].at("quantity"), "2");
  // Two lots sold at 100 and marked at 96, ten per point.
  EXPECT_EQ(state.at("unrealized"), "80");
  EXPECT_EQ(state.at("margin"), "200");
  // A long target first only buys the short back.
  engine.reconcile_target("flip", instrument().id, d("1"), d("96"));
  state = engine.snapshot();
  ASSERT_EQ(state.at("orders").size(), 2U);
  EXPECT_EQ(state.at("orders")[1].at("id"), "flip.today");
  EXPECT_EQ(state.at("orders")[1].at("side"), "buy");
  EXPECT_EQ(state.at("orders")[1].at("offset"), "close_today");
  EXPECT_EQ(state.at("orders")[1].at("quantity"), "2");
  engine.advance();
  state = engine.snapshot();
  EXPECT_TRUE(state.at("positions").empty());
  EXPECT_EQ(state.at("realized"), "80");
  EXPECT_EQ(state.at("fees"), "10"); // Two opens at 2 and two closes of today's lots at 3.
  // Flat now: the same target opens its own side.
  engine.reconcile_target("long", instrument().id, d("1"), d("98"));
  engine.advance();
  state = engine.snapshot();
  ASSERT_EQ(state.at("positions").size(), 1U);
  EXPECT_EQ(state.at("positions")[0].at("side"), "buy");
  EXPECT_EQ(state.at("positions")[0].at("quantity"), "1");
}
TEST(PaperExecution, TargetRefusesAContractHeldOnBothSides) {
  PaperExecution engine(
      d("1000"),
      {{{instrument(), costs()},
        {test::flat("2026-09-28", 100, "100", "30"), test::flat("2026-09-28", 200, "100", "30"),
         test::flat("2026-09-28", 300, "100", "30")}}},
      risk());
  engine.start();
  engine.advance();
  engine.submit(order("long", Side::buy, "1", "100"), Offset::open);
  engine.submit(order("short", Side::sell, "1", "100"), Offset::open);
  engine.advance();
  const auto before = engine.snapshot();
  ASSERT_EQ(before.at("positions").size(), 2U);
  EXPECT_THROW(engine.reconcile_target("net", instrument().id, d("1"), d("100")),
               std::invalid_argument);
  EXPECT_EQ(engine.snapshot(), before);
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
  EXPECT_THROW(engine.reconcile_target("two", spec.id, d("2"), d("100")), std::invalid_argument);
  EXPECT_EQ(engine.snapshot(), accepted);
  policy->stop();
  EXPECT_THROW(engine.reconcile_target("three", spec.id, d("1"), d("100")), std::invalid_argument);
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
  engine.settle_scheduled({d("105")}, false);
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
  engine.reconcile_target("close", instrument().id, d("0"), d("104"));
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
  EXPECT_THROW(engine.reconcile_target("split", instrument().id, d("0"), d("104")),
               std::invalid_argument);
  EXPECT_EQ(engine.snapshot(), before);
  // Only the yesterday bucket is needed: one order fits the same risk limit.
  engine.reconcile_target("reduce", instrument().id, d("1"), d("104"));
  EXPECT_EQ(engine.snapshot().at("orders").back().at("offset"), "close_yesterday");
  engine.advance();
  ASSERT_EQ(engine.snapshot().at("positions").size(), 1U);
  EXPECT_EQ(engine.snapshot().at("positions")[0].at("bucket"), "today");
}
TEST(PaperExecution, DayEndSettlementRejectsWrongPositionAndPendingOrdersAtomically) {
  PaperExecution engine(d("1000"), {{{instrument(), costs()}, settlement_bars()}}, risk());
  engine.start();
  const auto empty = engine.snapshot();
  EXPECT_THROW(engine.settle_scheduled({d("105")}, false), std::invalid_argument);
  EXPECT_EQ(engine.snapshot(), empty);
  engine.advance();
  engine.submit(order("open", Side::buy, "1", "100"), Offset::open);
  const auto pending = engine.snapshot();
  // The first bar is not the last of its day.
  EXPECT_THROW(engine.settle_scheduled({d("105")}, false), std::invalid_argument);
  EXPECT_EQ(engine.snapshot(), pending);
  engine.advance();
  const auto filled = engine.snapshot();
  EXPECT_THROW(engine.settle_scheduled({d("105.5")}, false), std::invalid_argument);
  EXPECT_EQ(engine.snapshot(), filled);
  engine.settle_scheduled({d("105")}, false);
  const auto settled = engine.snapshot();
  EXPECT_THROW(engine.settle_scheduled({d("106")}, false), std::invalid_argument)
      << "settled once per day";
  EXPECT_EQ(engine.snapshot(), settled);
  engine.advance();
  EXPECT_EQ(engine.snapshot().at("unrealized"), "10");
  for (int i = 0; i < 4; ++i)
    engine.advance();
  const auto finished = engine.snapshot();
  EXPECT_THROW(engine.settle_scheduled({d("105")}, false), std::invalid_argument);
  EXPECT_EQ(engine.snapshot(), finished);
}

TEST(PaperExecution, ChildIdentityConflictCannotPartiallyReplaceMixedBucketOrders) {
  PaperExecution engine(d("1000"), {{{instrument(), costs()}, settlement_bars()}}, risk());
  mixed_longs(engine);
  engine.submit(order("split.today", Side::sell, "1", "200"), Offset::close_today);
  const auto before = engine.snapshot();
  EXPECT_THROW(engine.reconcile_target("", instrument().id, d("0"), d("104")),
               std::invalid_argument);
  EXPECT_EQ(engine.snapshot(), before);
  EXPECT_THROW(engine.reconcile_target("split", instrument().id, d("0"), d("104")),
               std::invalid_argument);
  EXPECT_EQ(engine.snapshot(), before);
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
TEST(FuturesAccount, WorkingIndexesSurviveCopiesPartialFillsAndRejectedMutations) {
  FuturesAccount account(d("10000"), {{instrument(), costs()}});
  account.mark(instrument().id, d("100"));
  account.submit(order("first", Side::buy, "2", "100"), Offset::open);
  account.submit(order("second", Side::buy, "1", "100"), Offset::open);
  EXPECT_EQ(account.working_orders(), (std::set<std::size_t>{0, 1}));
  const auto before = account.snapshot();
  EXPECT_THROW(account.submit(order("first", Side::buy, "1", "100"), Offset::open),
               std::invalid_argument);
  EXPECT_THROW(account.fill({"oversized", "first", d("3"), d("100")}), std::invalid_argument);
  EXPECT_EQ(account.snapshot(), before);
  EXPECT_EQ(account.working_orders(), (std::set<std::size_t>{0, 1}));
  auto copy = account;
  copy.cancel("first");
  EXPECT_EQ(copy.working_orders(), (std::set<std::size_t>{1}));
  EXPECT_EQ(account.working_orders(), (std::set<std::size_t>{0, 1}));
  const Fill partial{"partial", "first", d("1"), d("100")};
  EXPECT_TRUE(account.fill(partial));
  EXPECT_FALSE(account.fill(partial));
  EXPECT_EQ(account.working_orders(), (std::set<std::size_t>{0, 1}));
  account.fill({"complete", "first", d("1"), d("100")});
  EXPECT_EQ(account.working_orders(), (std::set<std::size_t>{1}));
  account.cancel("second");
  EXPECT_TRUE(account.working_orders().empty());
  EXPECT_FALSE(account.has_working_orders());
  EXPECT_EQ(account.frozen(), Decimal{});
  EXPECT_THROW(account.fill({"late", "second", d("1"), d("100")}), std::logic_error);
  EXPECT_TRUE(account.working_orders().empty());
  EXPECT_EQ(account.orders().size(), 2U);
}
TEST(PaperExecution, CompletedOrderHistoryDoesNotParticipateInLaterBars) {
  std::vector<MarketBar> bars;
  for (std::int64_t i = 1; i <= 10000; ++i)
    bars.push_back(test::flat("2026-09-28", i, "100", "10"));
  PaperExecution execution(d("10000"), {{{instrument(), costs()}, std::move(bars)}}, risk());
  execution.start();
  execution.advance();
  for (std::size_t i = 0; i < 8000; ++i) {
    const auto id = "cancelled." + std::to_string(i);
    execution.submit(order(id, Side::buy, "1", "90"), Offset::open);
    execution.cancel(id);
  }
  execution.submit(order("partial", Side::buy, "2", "100"), Offset::open);
  const auto started = std::chrono::steady_clock::now();
  execution.advance();
  EXPECT_EQ(execution.account().working_orders(), (std::set<std::size_t>{8000}));
  execution.advance();
  EXPECT_TRUE(execution.account().working_orders().empty());
  while (execution.cursor() < execution.size())
    execution.advance();
  const auto elapsed = std::chrono::steady_clock::now() - started;
  RecordProperty(
      "completed_8000_orders_10000_bars_us",
      std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()));
  EXPECT_LT(elapsed, testing_support::bound(std::chrono::seconds(10)));
  EXPECT_EQ(execution.account().orders().size(), 8001U);
  EXPECT_EQ(execution.account().fills().size(), 2U);
  EXPECT_EQ(execution.account().orders().back().order.state(), OrderState::filled);
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
  // Previously every event copied the full ledger (O(events x orders)). The
  // bound catches that kind of blowup, not small slowdowns: about 5 s alone,
  // with headroom for machines shared with other tests.
  EXPECT_LT(std::chrono::steady_clock::now() - started,
            asterion::testing_support::bound(std::chrono::seconds(30)));
  EXPECT_EQ(execution.account().fills().size(), 0U);
  EXPECT_EQ(execution.account().orders().size(), resting);
}
namespace {
Instrument venue_instrument(const char* venue, const char* symbol) {
  return {{venue, symbol}, "CNY", d("1"), d("1"), d("10")};
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
TEST(PaperExecution, NextBarFillsRespectPriceParticipationAndArrivalOrder) {
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
  EXPECT_THROW(execution.advance(), std::logic_error);
  execution.start();
  execution.advance();
  execution.submit(order("first", Side::buy, "2", "101"), Offset::open);
  execution.submit(order("second", Side::buy, "3", "101"), Offset::open);
  EXPECT_TRUE(execution.account().fills().empty()) << "never on the bar it was placed after";
  execution.advance(); // participation 3 lots at min(open 99, limit 101)
  ASSERT_EQ(execution.account().fills().size(), 2U);
  EXPECT_EQ(execution.account().fills()[0].order_id, "first");
  EXPECT_EQ(execution.account().fills()[0].quantity.str(), "2");
  EXPECT_EQ(execution.account().fills()[0].price.str(), "99");
  EXPECT_EQ(execution.account().fills()[1].order_id, "second");
  EXPECT_EQ(execution.account().fills()[1].quantity.str(), "1");
  EXPECT_EQ(execution.account().fills()[1].price.str(), "99");
  execution.advance(); // low 100 reaches 101: the rest at the limit, not the better open
  ASSERT_EQ(execution.account().fills().size(), 3U);
  EXPECT_EQ(execution.account().fills()[2].order_id, "second");
  EXPECT_EQ(execution.account().fills()[2].quantity.str(), "2");
  EXPECT_EQ(execution.account().fills()[2].price.str(), "101");
  execution.submit(order("sell", Side::sell, "5", "105"), Offset::close_today);
  execution.advance(); // high 104 never reaches 105
  EXPECT_EQ(execution.account().fills().size(), 3U);
  execution.advance(); // high 106 reaches it: max(open 104, limit 105)
  ASSERT_EQ(execution.account().fills().size(), 4U);
  EXPECT_EQ(execution.account().fills()[3].price.str(), "105");
  EXPECT_TRUE(execution.account().positions().empty());
  const auto state = execution.snapshot();
  EXPECT_THROW(execution.advance(), std::invalid_argument);
  EXPECT_EQ(execution.snapshot(), state);
}

TEST(PaperExecution, SlippageMovesEveryFillAgainstTheOrderAndNotWhetherItFills) {
  // Two price increments given up on every fill of this contract.
  const std::vector<MarketBar> bars{test::flat("2026-09-28", 1, "100", "100"),
                                    test::bar("2026-09-28", 2, "101", "102", "101", "102", "100"),
                                    test::bar("2026-09-28", 3, "101", "103", "100", "102", "100"),
                                    test::bar("2026-09-28", 4, "99", "104", "98", "103", "100"),
                                    test::bar("2026-09-28", 5, "104", "105", "101", "102", "100"),
                                    test::bar("2026-09-28", 6, "107", "108", "106", "107", "100")};
  PaperExecution execution(d("100000"), {{{instrument(), costs()}, bars, {}, d("2")}}, risk());
  execution.start();
  execution.advance();
  // A target decided at 100 is placed at 102, the worst price it accepts.
  execution.reconcile_target("long", instrument().id, d("1"), d("100"));
  EXPECT_EQ(execution.snapshot().at("orders")[0].at("limit_price"), "102");
  // A low of 101 is inside that limit and still does not reach 100: no fill.
  execution.advance();
  EXPECT_TRUE(execution.account().fills().empty());
  // A low of 100 reaches it: filled at 100 and two more, not at the open.
  execution.advance();
  ASSERT_EQ(execution.account().fills().size(), 1U);
  EXPECT_EQ(execution.account().fills()[0].price.str(), "102");
  // A second lot decided at 102: the bar opens below it, at 99, and the fill
  // gives two up from there.
  execution.reconcile_target("more", instrument().id, d("2"), d("102"));
  execution.advance();
  ASSERT_EQ(execution.account().fills().size(), 2U);
  EXPECT_EQ(execution.account().fills()[1].price.str(), "101");
  // Selling both at a price of 103: placed at 101, filled once the high
  // reaches 103, at max(open 104, 103) less two.
  execution.reconcile_target("flat", instrument().id, d("0"), d("103"));
  EXPECT_EQ(execution.snapshot().at("orders").back().at("limit_price"), "101");
  execution.advance();
  ASSERT_EQ(execution.account().fills().size(), 3U);
  EXPECT_EQ(execution.account().fills()[2].price.str(), "102");
  EXPECT_TRUE(execution.account().positions().empty());
  // Bought at 102 and 101, sold at 102: one point over two lots at ten a
  // point, where without slippage 100, 99 and 104 would have made 90.
  EXPECT_EQ(execution.account().realized().str(), "10");

  // An order placed by hand keeps its limit: with slippage it needs the bar
  // to reach two inside it.
  execution.submit(order("manual", Side::sell, "1", "106"), Offset::open);
  execution.advance(); // high 108 reaches 108 = 106 + 2: max(open 107, 108) - 2
  ASSERT_EQ(execution.account().fills().size(), 4U);
  EXPECT_EQ(execution.account().fills()[3].price.str(), "106");

  // Slippage is a whole number of increments and leaves a sale a price.
  const auto with = [&](const char* slippage) {
    return PaperExecution(d("100000"), {{{instrument(), costs()}, bars, {}, d(slippage)}}, risk());
  };
  EXPECT_THROW(with("0.5"), std::invalid_argument);
  EXPECT_THROW(with("-1"), std::invalid_argument);
  EXPECT_THROW(with("98"), std::invalid_argument);
  EXPECT_NO_THROW(with("97"));
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

TEST(BarDataset, RevisionRetainsExactCanonicalBytesAndExcludesProvenance) {
  std::vector<MarketBar> bars{test::flat("2026-09-25", 1790384400000000000LL, "100", "0"),
                              test::flat("2026-09-28", 1790643600000000000LL, "105", "7")};
  auto dataset = test::dataset(bars);
  auto canonical = Json{{"version", 1},
                        {"interval_minutes", dataset.interval_minutes()},
                        {"contract", protocol::decode_contract(dataset.contract())},
                        {"bars", Json::array()},
                        {"days", Json::array()}};
  for (const auto& bar : bars)
    canonical["bars"].push_back({{"trading_day", bar.trading_day},
                                 {"timestamp_ns", std::to_string(bar.timestamp_ns)},
                                 {"open", bar.open.str()},
                                 {"high", bar.high.str()},
                                 {"low", bar.low.str()},
                                 {"close", bar.close.str()},
                                 {"volume", bar.volume.str()}});
  for (const auto& day : dataset.days())
    canonical["days"].push_back(
        {{"trading_day", day.trading_day()},
         {"settlement_price", Decimal::from_raw(day.settlement_price().units()).str()}});
  EXPECT_EQ(protocol::bar_dataset_revision(dataset), sha256_bytes(canonical.dump()));
  dataset.set_source("another-provider");
  dataset.set_source_dataset_ids(0, std::string(64, 'a'));
  EXPECT_EQ(protocol::bar_dataset_revision(dataset), sha256_bytes(canonical.dump()));
  // Canonical identity is also used while constructing a dataset, before
  // semantic validation. Escapes and exact signed decimals remain unambiguous.
  dataset.mutable_bars(0)->set_trading_day("quote\"\\\n中");
  dataset.mutable_bars(0)->mutable_close()->set_units(-123456789);
  canonical["bars"][0]["trading_day"] = "quote\"\\\n中";
  canonical["bars"][0]["close"] = Decimal::from_raw(-123456789).str();
  EXPECT_EQ(protocol::bar_dataset_revision(dataset), sha256_bytes(canonical.dump()));
}
TEST(BarDataset, FixedAvailabilityEvidenceIsRequiredButSeparateFromContentIdentity) {
  auto dataset = test::dataset(test::series(4));
  const auto original = dataset.SerializeAsString();
  const auto revision = dataset.revision();
  const auto decoded = protocol::decode_bar_dataset(dataset);
  EXPECT_EQ(protocol::encode_bar_dataset(decoded).SerializeAsString(), original);
  EXPECT_EQ(decoded.at("history_evidence")[0].at("source_availability"), "unknown");
  dataset.mutable_history_evidence(0)->set_acquired_at_ns(1800000000000000000);
  EXPECT_NO_THROW(protocol::validate_bar_dataset(dataset));
  EXPECT_EQ(protocol::bar_dataset_revision(dataset), revision);
  EXPECT_NE(dataset.SerializeAsString(), original);
  dataset.mutable_history_evidence(0)->set_source_availability(
      data::v1::SOURCE_AVAILABILITY_UNSPECIFIED);
  EXPECT_THROW(protocol::validate_bar_dataset(dataset), std::invalid_argument);
  dataset = protocol::encode_bar_dataset(decoded);
  dataset.mutable_history_evidence(0)->set_dataset_id(std::string(64, 'c'));
  EXPECT_THROW(protocol::validate_bar_dataset(dataset), std::invalid_argument);
  dataset.set_version(1);
  dataset.clear_history_evidence();
  EXPECT_THROW(protocol::validate_bar_dataset(dataset), std::invalid_argument);
}
TEST(BarDataset, TaskLimitIsTwoHundredThousandBars) {
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

TEST(FuturesCosts, DatedSchedulesRejectAmbiguityAndMissingCoverage) {
  std::vector<FuturesCostVersion> schedule{{"2026-09-25", "published A", costs()},
                                           {"2026-09-28", "published B", costs()}};
  EXPECT_NO_THROW(validate_cost_schedule(schedule));
  EXPECT_EQ(costs_on(schedule, "2026-09-27").source, "published A");
  EXPECT_EQ(costs_on(schedule, "2026-09-28").source, "published B");
  EXPECT_THROW(costs_on(schedule, "2026-09-24"), std::invalid_argument);
  EXPECT_THROW(validate_cost_schedule({}), std::invalid_argument);
  for (const auto day : {"2026-09-25", "2026-09-24", "2026-02-30"}) {
    auto bad = schedule;
    bad[1].effective_from = day;
    EXPECT_THROW(validate_cost_schedule(bad), std::invalid_argument);
  }
  schedule[1].source = " \n";
  EXPECT_THROW(validate_cost_schedule(schedule), std::invalid_argument);
}
TEST(FuturesAccount, CostChangePreservesLedgerAndRejectsWorkingOrdersAtomically) {
  FuturesAccount account(d("1000"), {{instrument(), costs()}});
  account.mark(instrument().id, d("100"));
  account.submit(order("open", Side::buy, "2", "100"), Offset::open);
  account.fill({"fill", "open", d("1"), d("100")});
  auto changed = costs();
  changed.margin_per_lot = d("2000");
  auto before = account.snapshot();
  EXPECT_THROW(account.update_costs({changed}), std::invalid_argument);
  EXPECT_EQ(account.snapshot(), before);
  account.cancel("open");
  account.update_costs({changed});
  EXPECT_EQ(account.balance(), d("998"));
  EXPECT_EQ(account.fees(), d("2"));
  EXPECT_EQ(account.available(), d("-1002"));
  EXPECT_EQ(account.positions().size(), 1U);
  before = account.snapshot();
  auto invalid = changed;
  invalid.open_fee = d("-1");
  EXPECT_THROW(account.update_costs({invalid}), std::invalid_argument);
  EXPECT_EQ(account.snapshot(), before);
  EXPECT_EQ(account.contracts()[0].costs.margin_per_lot, d("2000"));
}

TEST(FuturesAccount, TransactionFailureRestoresCostsFundsHistoryAndReusableIdentities) {
  FuturesAccount account(d("100000"), {{instrument(), costs()}});
  account.mark(instrument().id, d("100"));
  account.submit(order("old", Side::buy, "1", "100"), Offset::open);
  account.fill({"old-fill", "old", d("1"), d("100")});
  const auto before = account.snapshot();
  const auto original = account.contracts()[0].costs;
  EXPECT_THROW(
      {
        auto batch = account.transaction();
        auto changed = costs();
        changed.open_fee = d("7");
        account.update_costs({changed});
        account.submit(order("new", Side::buy, "2", "100"), Offset::open);
        account.fill({"new-fill", "new", d("1"), d("100")});
        account.cancel("new");
        account.mark(instrument().id, d("90000000000"));
        batch.commit();
      },
      std::overflow_error);
  EXPECT_EQ(account.snapshot(), before);
  EXPECT_EQ(account.contracts()[0].costs.open_fee, original.open_fee);
  EXPECT_EQ(account.contracts()[0].costs.margin_per_lot, original.margin_per_lot);
  EXPECT_TRUE(account.working_orders().empty());
  account.submit(order("new", Side::buy, "2", "100"), Offset::open);
  EXPECT_TRUE(account.fill({"new-fill", "new", d("1"), d("100")}));
  EXPECT_EQ(account.working_orders(), (std::set<std::size_t>{1}));
  EXPECT_EQ(account.fees(), d("4"));
  const auto after = account.snapshot();
  EXPECT_FALSE(account.fill({"new-fill", "new", d("1"), d("100")}));
  EXPECT_EQ(account.snapshot(), after);
}
TEST(FuturesAccount, CostArithmeticFailureRestoresAllPreviousRates) {
  FuturesAccount account(d("100000"), {{instrument(), costs()}});
  account.mark(instrument().id, d("100"));
  account.submit(order("open", Side::buy, "2", "100"), Offset::open);
  account.fill({"fill", "open", d("2"), d("100")});
  const auto before = account.snapshot();
  auto invalid = costs();
  invalid.margin_per_lot = d("90000000000");
  EXPECT_THROW(account.update_costs({invalid}), std::overflow_error);
  EXPECT_EQ(account.snapshot(), before);
  EXPECT_EQ(account.contracts()[0].costs.margin_per_lot, costs().margin_per_lot);
}
TEST(PaperExecution, LateMarkFailureRollsBackBothFillsAndExecutionSequence) {
  auto high = test::flat("2026-09-28", 2, "100", "20");
  high.high = high.close = d("90000000000");
  std::vector<MarketBar> bars{test::flat("2026-09-28", 1, "100", "20"), high,
                              test::flat("2026-09-28", 3, "100", "20")};
  PaperExecution execution(d("100000"), {{{instrument(), costs()}, std::move(bars)}}, risk());
  execution.start();
  execution.advance();
  execution.submit(order("first", Side::buy, "1", "100"), Offset::open);
  execution.submit(order("second", Side::buy, "1", "100"), Offset::open);
  const auto before = execution.snapshot();
  const auto revision = execution.revision();
  EXPECT_THROW(execution.advance(), std::overflow_error);
  EXPECT_EQ(execution.snapshot(), before);
  EXPECT_EQ(execution.revision(), revision);
  EXPECT_EQ(execution.account().working_orders(), (std::set<std::size_t>{0, 1}));
  execution.cancel_open_orders();
  execution.advance();
  execution.submit(order("after", Side::buy, "1", "100"), Offset::open);
  execution.advance();
  ASSERT_EQ(execution.account().fills().size(), 1U);
  EXPECT_EQ(execution.account().fills()[0].execution_id, "paper.fill.1");
  EXPECT_EQ(execution.account().balance(), d("99998"));
}
TEST(PaperExecution, FrequentFillsWithRetainedHistoryWorkSample) {
  std::vector<MarketBar> bars;
  for (std::int64_t i = 1; i <= 2001; ++i)
    bars.push_back(test::flat("2026-09-28", i, "100", "10"));
  PaperExecution execution(d("100000"), {{{instrument(), costs()}, std::move(bars)}}, risk());
  execution.start();
  execution.advance();
  for (unsigned i = 0; i < 8000; ++i) {
    const auto id = "cancelled." + std::to_string(i);
    execution.submit(order(id, Side::buy, "1", "90"), Offset::open);
    execution.cancel(id);
  }
  const auto started = std::chrono::steady_clock::now();
  for (unsigned i = 0; i < 2000; ++i) {
    const bool open = i % 2 == 0;
    execution.submit(order("fill." + std::to_string(i), open ? Side::buy : Side::sell, "1", "100"),
                     open ? Offset::open : Offset::close_today);
    execution.advance();
  }
  const auto elapsed = std::chrono::steady_clock::now() - started;
  RecordProperty(
      "frequent_2000_fills_with_8000_prior_orders_ms",
      std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()));
  EXPECT_LT(elapsed, testing_support::bound(std::chrono::seconds(5)));
  EXPECT_EQ(execution.account().orders().size(), 10000U);
  EXPECT_EQ(execution.account().fills().size(), 2000U);
  EXPECT_TRUE(execution.account().positions().empty());
  EXPECT_TRUE(execution.account().working_orders().empty());
  EXPECT_EQ(execution.account().balance(), d("95000"));
}
