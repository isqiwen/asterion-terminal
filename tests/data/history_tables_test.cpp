#include "history_tables.hpp"
#include "sqlite_database.hpp"
#include "table_parquet.hpp"
#include <asterion/kernel/process/child.hpp>
#include <fstream>
#include <gtest/gtest.h>
using namespace asterion;
using history_files::TableSeries;
using history_files::TableStore;
namespace {
struct Folder {
  std::filesystem::path path =
      std::filesystem::temp_directory_path() / ("asterion-tables-" + unique_process_id());
  Folder() { std::filesystem::create_directory(path); }
  ~Folder() { std::filesystem::remove_all(path); }
};
const HistoryTable& daily = history_table("futures.daily");
// A daily row with only the cells a test cares about; the rest are missing.
TableSourceRow row(const char* contract, const char* close, const char* volume,
                   const char* amount) {
  TableSourceRow cells(daily.columns.size());
  cells[0] = contract;
  if (close)
    cells[4] = close;
  if (volume)
    cells[8] = volume;
  if (amount)
    cells[9] = amount;
  return cells;
}
std::string rejection(const std::vector<TableSourceRow>& rows, const char* exchange = "SHFE") {
  try {
    (void)table_partition(daily, exchange, "2025-07-25", rows);
  } catch (const std::invalid_argument& error) {
    return error.what();
  }
  return {};
}
const TableSeries series{"futures.daily", 1, "fixture.daily.v1"};
const std::string artifact(64, 'a');
} // namespace
TEST(HistoryTables, KeepWhatTheSourcePublishedExactly) {
  // A day's turnover above what an eight-place int64 holds, and a contract
  // that did not trade: both are ordinary rows of a real exchange day.
  const auto partition =
      table_partition(daily, "SHFE", "2025-07-25",
                      {row("SHFE/rb/2025-10", "3356", "3501234.0", "117501413040.50"),
                       row("SHFE/rb/2025-06", nullptr, "0", "0")});
  ASSERT_EQ(partition.rows.size(), 2U);
  EXPECT_EQ(std::get<std::string>(partition.rows[0][0]), "SHFE/rb/2025-06") << "ordered by key";
  EXPECT_TRUE(std::holds_alternative<std::monostate>(partition.rows[0][4]));
  EXPECT_EQ(table_cell_text(daily.columns[4], partition.rows[0][4]), std::nullopt);
  EXPECT_EQ(std::get<std::int64_t>(partition.rows[1][8]), 3501234);
  EXPECT_EQ(std::get<std::int64_t>(partition.rows[1][9]), 11750141304050);
  EXPECT_EQ(table_cell_text(daily.columns[9], partition.rows[1][9]), "117501413040.5");
  EXPECT_EQ(table_cell_text(daily.columns[4], partition.rows[1][4]), "3356");
  // The digest names the rows, not the order they arrived in.
  EXPECT_EQ(partition.content,
            table_partition(daily, "SHFE", "2025-07-25",
                            {row("SHFE/rb/2025-06", nullptr, "0.00", "0"),
                             row("SHFE/rb/2025-10", "3356.0", "3501234", "117501413040.5")})
                .content);
  EXPECT_NE(partition.content,
            table_partition(daily, "SHFE", "2025-07-28",
                            {row("SHFE/rb/2025-10", "3356", "3501234.0", "117501413040.50"),
                             row("SHFE/rb/2025-06", nullptr, "0", "0")})
                .content);
}
TEST(HistoryTables, RejectionsNameTheRowAndColumn) {
  EXPECT_EQ(rejection({row("SHFE/rb/2025-10", "3356", "1", "1"),
                       row("SHFE/cu/2025-10", "78000.123456789", "1", "1")}),
            "futures.daily SHFE 2025-07-25: row 2 \"SHFE/cu/2025-10\" column close: "
            "\"78000.123456789\" has more than 8 decimal places");
  EXPECT_EQ(rejection({row("SHFE/rb/2025-10", "3356", "1.5", "1")}),
            "futures.daily SHFE 2025-07-25: row 1 \"SHFE/rb/2025-10\" column volume: "
            "\"1.5\" has more than 0 decimal places");
  EXPECT_EQ(rejection({row("SHFE/rb/2025-10", "3.356e3", "1", "1")}),
            "futures.daily SHFE 2025-07-25: row 1 \"SHFE/rb/2025-10\" column close: "
            "\"3.356e3\" is not a plain decimal number");
  EXPECT_EQ(rejection({row("SHFE/rb/2025-10", "1", "1", "12345678901234567.89")}),
            "futures.daily SHFE 2025-07-25: row 1 \"SHFE/rb/2025-10\" column amount: "
            "\"12345678901234567.89\" is out of range");
  EXPECT_EQ(rejection({row("DCE/i/2025-09", "1", "1", "1")}),
            "futures.daily SHFE 2025-07-25: row 1 \"DCE/i/2025-09\" column contract: "
            "\"DCE/i/2025-09\" is not a contract of SHFE");
  EXPECT_EQ(
      rejection({row("SHFE/rb/2025-10", "1", "1", "1"), row("SHFE/rb/2025-10", "2", "1", "1")}),
      "futures.daily SHFE 2025-07-25: key \"SHFE/rb/2025-10\" appears more than once");
  TableSourceRow keyless(daily.columns.size());
  EXPECT_EQ(rejection({keyless}),
            "futures.daily SHFE 2025-07-25: row 1 column contract is missing");
  EXPECT_EQ(rejection({{std::string("SHFE/rb/2025-10")}}),
            "futures.daily SHFE 2025-07-25: row 1 \"SHFE/rb/2025-10\" has 1 cells, expected 13");
  EXPECT_THROW((void)table_partition(daily, "NYMEX", "2025-07-25", {}), std::invalid_argument);
  EXPECT_THROW((void)table_partition(daily, "SHFE", "2025", {}), std::invalid_argument);
  // A year of the calendar holds that year's days only.
  const auto& calendar = history_table("futures.calendar");
  EXPECT_NO_THROW((void)table_partition(
      calendar, "SHFE", "2026",
      {{std::string("2026-10-08"), std::string("1"), std::string("2026-09-30")}}));
  EXPECT_THROW((void)table_partition(calendar, "SHFE", "2026",
                                     {{std::string("2025-12-31"), std::string("1"), std::nullopt}}),
               std::invalid_argument);
  EXPECT_THROW(
      (void)table_partition(calendar, "SHFE", "2026",
                            {{std::string("2026-10-08"), std::string("yes"), std::nullopt}}),
      std::invalid_argument);
}
TEST(HistoryTables, APartitionReadsBackFromItsFileAsItWasWritten) {
  Folder folder;
  const auto& contracts = history_table("futures.contracts");
  TableSourceRow listing(contracts.columns.size());
  listing[0] = "CFFEX/if/2026-11";
  listing[1] = "IF2611.CFX";
  listing[2] = "IF2611";
  listing[3] = "2026-09-21";
  listing[4] = "2026-11-20";
  listing[6] = "300";
  listing[10] = "0.2指数点";
  const auto partition = table_partition(contracts, "CFFEX", "all", {listing});
  const auto file = folder.path / "contracts.parquet";
  parquet::write_table(file, partition);
  EXPECT_EQ(parquet::read_table(file, contracts), partition.rows);
  const auto bars = table_partition(daily, "SHFE", "2025-07-25",
                                    {row("SHFE/rb/2025-10", "3356.5", "3501234", "117501413040.5"),
                                     row("SHFE/rb/2025-06", nullptr, nullptr, nullptr)});
  parquet::write_table(folder.path / "daily.parquet", bars);
  EXPECT_EQ(parquet::read_table(folder.path / "daily.parquet", daily), bars.rows);
  EXPECT_THROW(parquet::read_table(file, daily), std::invalid_argument) << "another table's file";
}
TEST(HistoryTableStore, ARevisionNeverReplacesWhatAnEarlierSnapshotHeld) {
  Folder folder;
  const auto first = table_partition(daily, "SHFE", "2025-07-25",
                                     {row("SHFE/rb/2025-10", "3356", "100", "3356000")});
  const auto corrected = table_partition(daily, "SHFE", "2025-07-25",
                                         {row("SHFE/rb/2025-10", "3357", "100", "3357000")});
  const auto next = table_partition(daily, "SHFE", "2025-07-28",
                                    {row("SHFE/rb/2025-10", "3360", "90", "3024000")});
  std::string snapshot;
  history_files::TableRevision original;
  {
    TableStore store(folder.path / "tables");
    EXPECT_EQ(store.head(), "");
    EXPECT_FALSE(store.current(series, "SHFE", "2025-07-25"));
    const auto recorded = store.record(first, series, store.write(first), artifact, 1000);
    EXPECT_FALSE(recorded.unchanged);
    EXPECT_EQ(recorded.revision.sequence, 1U);
    original = recorded.revision;
    snapshot = store.head();
    EXPECT_EQ(snapshot, original.snapshot);
    // The same content again is the same revision: nothing is appended.
    const auto again = store.record(first, series, store.write(first), artifact, 2000);
    EXPECT_TRUE(again.unchanged);
    EXPECT_EQ(again.revision.sequence, 1U);
    EXPECT_EQ(again.revision.acquired_at_ns, 1000);
    EXPECT_EQ(store.head(), snapshot);
    EXPECT_EQ(store.record(next, series, store.write(next), artifact, 3000).revision.sequence, 2U);
    const auto revised = store.record(corrected, series, store.write(corrected), artifact, 4000);
    EXPECT_FALSE(revised.unchanged);
    EXPECT_EQ(revised.revision.sequence, 3U);
    EXPECT_NE(store.head(), snapshot);
  }
  // Another process later: the ledger and every revision's rows are still there.
  TableStore store(folder.path / "tables");
  const auto current = store.current(series, "SHFE", "2025-07-25");
  ASSERT_TRUE(current);
  EXPECT_EQ(current->sequence, 3U);
  EXPECT_EQ(store.rows(*current), corrected.rows);
  EXPECT_EQ(store.rows(original), first.rows) << "the superseded revision stays readable";
  EXPECT_EQ(store.periods(series, "SHFE", "2025-07-01", "2025-07-31"),
            (std::vector<std::string>{"2025-07-25", "2025-07-28"}));
  EXPECT_TRUE(store.periods(series, "DCE", "2025-07-01", "2025-07-31").empty());
  const auto coverage = store.coverage();
  ASSERT_EQ(coverage.size(), 1U);
  EXPECT_EQ(coverage[0].series, series);
  EXPECT_EQ(coverage[0].exchange, "SHFE");
  EXPECT_EQ(coverage[0].partitions, 2U);
  EXPECT_EQ(coverage[0].rows, 2U);
  EXPECT_EQ(coverage[0].first_period, "2025-07-25");
  EXPECT_EQ(coverage[0].last_period, "2025-07-28");
  EXPECT_EQ(coverage[0].acquired_at_ns, 4000);
  // A file that no longer has the recorded bytes is refused, not read.
  const auto object = folder.path / "tables" / "objects" / current->object.substr(0, 2) /
                      (current->object + ".parquet");
  std::ofstream(object, std::ios::app) << "x";
  EXPECT_THROW(store.rows(*current), std::invalid_argument);
  EXPECT_THROW(store.current({"futures.daily", 2, "fixture.daily.v1"}, "SHFE", "2025-07-25"),
               std::invalid_argument);
}
TEST(HistoryTableStore, RefusesALedgerItDidNotWrite) {
  Folder folder;
  std::filesystem::create_directories(folder.path / "tables");
  {
    sqlite::Database foreign(folder.path / "tables" / "ledger.sqlite");
    foreign.execute("CREATE TABLE something(value INTEGER)");
  }
  EXPECT_THROW(TableStore(folder.path / "tables"), std::invalid_argument);
  EXPECT_THROW(TableStore("relative"), std::invalid_argument);
}
