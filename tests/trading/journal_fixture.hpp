#pragma once
// Test-only access to a closed SqliteJournal: read, count and tamper records.
#include "sqlite_database.hpp"
#include "sqlite_journal.hpp"
#include <asterion/foundation/serialization.hpp>
#include <filesystem>
namespace asterion::test {
struct SmallJournalSegments {
  explicit SmallJournalSegments(std::size_t records) {
    journal_segment_records_for_testing(records);
  }
  ~SmallJournalSegments() { journal_segment_records_for_testing(0); }
};
struct JournalRecord {
  std::filesystem::path directory;
  std::int64_t index;
};
inline JournalRecord journal_record(const std::filesystem::path& directory, std::int64_t index) {
  return {directory, index};
}
inline Json read_record(const JournalRecord& record) {
  sqlite::Database database(record.directory / "journal.sqlite");
  sqlite::Database::Statement query(database, "SELECT body FROM records WHERE sequence=?");
  query.bind(1, record.index);
  if (!query.step())
    throw std::invalid_argument("journal record fixture is missing");
  return Json::parse(query.text(0));
}
inline void write_record(const JournalRecord& record, const Json& value) {
  sqlite::Database database(record.directory / "journal.sqlite");
  sqlite::Database::Statement update(database, "UPDATE records SET body=? WHERE sequence=?");
  update.bind(1, value.dump()).bind(2, record.index).step();
}
// Committed records; zero when no journal was created.
inline std::size_t journal_size(const std::filesystem::path& directory) {
  if (!std::filesystem::exists(directory / "journal.sqlite"))
    return 0;
  sqlite::Database database(directory / "journal.sqlite");
  sqlite::Database::Statement query(database, "SELECT COUNT(*) FROM records");
  query.step();
  return static_cast<std::size_t>(query.integer(0));
}
} // namespace asterion::test
