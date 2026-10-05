#pragma once
#include <asterion/kernel/journal_port.hpp>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
namespace asterion {
// Test-only rotation threshold; zero restores the production budget.
void journal_segment_records_for_testing(std::size_t records);
namespace sqlite {
class Database;
}
// Rejects a directory that is not a dedicated journal directory, without
// creating or changing anything; the same check start() applies.
void check_journal_directory(const std::filesystem::path& directory,
                             const std::set<std::string>& sidecar_directories);
// Reads only the first committed record through SQLite's normal locking.
// Does not create a database, replay commands or write records. SQLite may
// create WAL coordination files; an exclusive writer causes this read to fail.
Json read_journal_header(const std::filesystem::path& directory,
                         const std::set<std::string>& sidecar_directories = {});
// Stable header, command index and active events live in journal.sqlite;
// immutable event segments live in archives/, alongside declared sidecars.
class SqliteJournal final : public JournalPort {
public:
  struct Capacity {
    std::uint64_t records_used, records_limit, bytes_used, bytes_limit;
    std::uint64_t total_records = 0, segment_count = 0;
  };
  struct Archive {
    std::string name, digest;
    std::uint64_t records, bytes, first;
  };
  struct OrderIndex {
    std::string order_id, trading_day, broker_key;
    std::uint64_t sequence = 0, excluded_by = 0;
  };
  explicit SqliteJournal(std::filesystem::path directory,
                         std::set<std::string> sidecar_directories = {},
                         std::function<void(const Json&)> validate_header = {});
  ~SqliteJournal() override;
  PluginDescriptor descriptor() const override;
  void start() override;
  void stop() noexcept override;
  std::vector<Json> read(std::uint64_t first) const override;
  void append(const Json& record) override;
  // An identified record and its lifetime deduplication entry commit together.
  // A submission also claims its lifetime order identity in the same transaction.
  // Automatic segmentation never retires either identity.
  void append(const Json& record, std::string_view command_id, std::string_view order_id = {},
              std::string_view excluded_order = {});
  std::optional<std::uint64_t> command_sequence(std::string_view id) const;
  std::optional<std::uint64_t> order_sequence(std::string_view id) const;
  std::optional<OrderIndex> order_index(std::string_view id) const;
  std::uint64_t order_identity_count() const;
  std::vector<OrderIndex> order_identities(std::string_view day, std::uint64_t after) const;
  Json record(std::uint64_t sequence) const;
  Json command(std::string_view id) const;
  Json order(std::string_view id) const;
  Capacity capacity() const;
  static void verify_archive(const std::filesystem::path& directory, const Archive& archive);

private:
  std::filesystem::path directory_;
  std::set<std::string> sidecar_directories_;
  std::function<void(const Json&)> validate_header_;
  std::unique_ptr<sqlite::Database> database_;
  Archive seal() const;
  std::vector<Json> read_records(std::uint64_t first, std::size_t count) const;
  bool poisoned_ = false;
  std::size_t count_ = 0;
  std::uintmax_t bytes_ = 0, header_bytes_ = 0;
  std::uint64_t first_ = 1, segments_ = 0;
};
} // namespace asterion
