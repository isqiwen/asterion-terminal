#include "journal_fixture.hpp"
#include "timing.hpp"
#include <gtest/gtest.h>
#include <asterion/domain/account.hpp>
#include <asterion/kernel/process/child.hpp>
#include "paper_execution.hpp"
#include "order_limits.hpp"
#include "sqlite_journal.hpp"
#include <asterion/protocol/data.hpp>
#include "bar_fixture.hpp"
#include <chrono>
#include <fstream>
using namespace asterion;
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
  engine.reconcile_long_target("one", instrument().id, d("1"), d("100"));
  const auto before = engine.snapshot();
  EXPECT_THROW(engine.reconcile_long_target("two", instrument().id, d("2"), d("100")),
               std::invalid_argument);
  EXPECT_EQ(engine.snapshot(), before);
  engine.advance();
  EXPECT_EQ(engine.snapshot().at("fills").size(), 1U);
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
