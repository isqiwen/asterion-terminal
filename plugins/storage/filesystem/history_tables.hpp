#pragma once
#include <asterion/domain/history_tables.hpp>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
namespace asterion::sqlite {
class Database;
}
namespace asterion::history_files {
// One table from one source under one column definition. Another source or
// another definition of the same table is another series: they never mix.
struct TableSeries {
  std::string table;
  unsigned schema = 0;
  std::string source; // the source's normalization identity
  auto operator<=>(const TableSeries&) const = default;
};
// One entry of the ledger: the content a partition held from this sequence on.
struct TableRevision {
  std::uint64_t sequence = 0;
  TableSeries series;
  std::string exchange, period;
  std::string content, object; // SHA-256 of the typed rows, and of the file
  std::uint64_t rows = 0, bytes = 0;
  std::int64_t acquired_at_ns = 0;
  std::string provider_artifact;
  std::string snapshot; // identifies the ledger up to and including this entry
};
struct TableCoverage {
  TableSeries series;
  std::string exchange, first_period, last_period;
  std::uint64_t partitions = 0, rows = 0;
  std::int64_t acquired_at_ns = 0; // the most recent
};
// Market-wide tables as immutable partition files and an append-only ledger:
//
//   <root>/ledger.sqlite
//   <root>/objects/<first two digits>/<object>.parquet
//
// A partition is one exchange and one period of a series. Recording appends a
// revision; earlier revisions and their files stay. A snapshot is a prefix of
// the ledger, so a reader that keeps a snapshot keeps exactly what it read.
// One process owns the ledger; calls may come from any of its threads.
class TableStore final {
public:
  explicit TableStore(std::filesystem::path root);
  ~TableStore();
  // The revision a partition currently holds.
  std::optional<TableRevision> current(const TableSeries&, const std::string& exchange,
                                       const std::string& period) const;
  struct Object {
    std::string sha256;
    std::uint64_t bytes = 0;
  };
  // Writes a partition's file. Nothing reads it until record() names it.
  Object write(const TablePartition&) const;
  struct Recorded {
    TableRevision revision;
    bool unchanged = false; // the partition already held this content
  };
  Recorded record(const TablePartition&, const TableSeries&, const Object&,
                  const std::string& provider_artifact, std::int64_t acquired_at_ns);
  // The snapshot after the last entry; empty for an empty ledger.
  std::string head() const;
  std::vector<TableCoverage> coverage() const;
  // Periods of one exchange that hold a revision, ascending, within [first, last].
  std::vector<std::string> periods(const TableSeries&, const std::string& exchange,
                                   const std::string& first, const std::string& last) const;
  // Where a snapshot ends in the ledger; nothing for a snapshot it never had.
  std::optional<std::uint64_t> sequence(const std::string& snapshot) const;
  // What a snapshot holds of one exchange within [first, last]: for each period
  // the newest revision up to the snapshot's last entry, ascending by period.
  std::vector<TableRevision> revisions(const TableSeries&, const std::string& exchange,
                                       const std::string& first, const std::string& last,
                                       std::uint64_t until) const;
  // The typed rows of a revision, after its file's digest has been checked.
  std::vector<std::vector<TableCell>> rows(const TableRevision&) const;
  // Revisions of one series read together, after every file's digest has been
  // checked: of each row whose first column starts with one of `prefixes`
  // (every row when there are none), the cells of `columns`, which index the
  // table's columns. `visit` is given the row's revision as an index into
  // `revisions`; rows come in no particular order.
  void scan(std::span<const TableRevision> revisions, std::span<const std::size_t> columns,
            std::span<const std::string> prefixes,
            const std::function<void(std::size_t, std::vector<TableCell>&)>& visit) const;

private:
  std::filesystem::path object_path(const std::string& object) const;
  std::filesystem::path root_;
  mutable std::mutex mutex_;
  std::unique_ptr<sqlite::Database> ledger_;
};
} // namespace asterion::history_files
