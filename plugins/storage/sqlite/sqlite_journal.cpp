#include "sqlite_journal.hpp"
#include "sqlite_database.hpp"
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/child.hpp>
#include <stdexcept>
#include <atomic>
#include <optional>
namespace asterion {
namespace {
constexpr auto file_name = "journal.sqlite";
constexpr auto format = "asterion.journal.v5";
// Physical event-file budgets; command identities have an independent lifetime.
constexpr std::size_t max_records = 100001;
std::atomic<std::size_t> test_segment_records{0};

constexpr std::uintmax_t max_record = 65536, max_total = 256ULL * 1024 * 1024;
void validate_format(sqlite::Database& database, bool archive = false) {
  sqlite::Database::Statement tables(
      database, "SELECT COUNT(*) FROM sqlite_schema WHERE "
                "type='table' AND name IN ('meta','records','commands','segments')");
  if (!tables.step() || tables.integer(0) != 4)
    throw std::invalid_argument("unsupported journal format");
  sqlite::Database::Statement stored(database, "SELECT value FROM meta WHERE key='format'");
  if (!stored.step() || stored.text(0) != format)
    throw std::invalid_argument("unsupported journal format");
  sqlite::Database::Statement sealed(database, "SELECT value FROM meta WHERE key='sealed'");
  const bool is_sealed = sealed.step();
  if (archive ? !is_sealed || sealed.text(0) != "1" : is_sealed)
    throw std::invalid_argument("sealed trading archives cannot be used as active records");
}
void initialize(sqlite::Database& database) {
  database.execute("CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT NOT NULL) STRICT");
  database.execute("CREATE TABLE records(sequence INTEGER PRIMARY KEY, body TEXT NOT NULL) STRICT");
  database.execute(
      "CREATE TABLE commands(id TEXT PRIMARY KEY, sequence INTEGER NOT NULL UNIQUE "
      "CHECK(sequence > 0), order_id TEXT UNIQUE, trading_day TEXT, broker_key TEXT, "
      "excluded_by INTEGER CHECK(excluded_by IS NULL OR excluded_by > sequence)) STRICT");
  database.execute("CREATE INDEX order_day ON commands(trading_day, sequence)");
  database.execute(
      "CREATE INDEX order_broker_identity ON commands(trading_day, broker_key, sequence) "
      "WHERE excluded_by IS NULL");
  database.execute(
      "CREATE TABLE segments(first INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE, "
      "digest TEXT NOT NULL, records INTEGER NOT NULL, bytes INTEGER NOT NULL) STRICT");
  database.execute(std::string("INSERT INTO meta VALUES('format','") + format + "')");
}
SqliteJournal::Archive archive_rows(sqlite::Database& source, sqlite::Database* destination,
                                    std::uint64_t first) {
  SqliteJournal::Archive result{"", sha256_bytes("asterion.journal-segment.v1"), 0, 0, first};
  sqlite::Database::Statement rows(source,
                                   "SELECT sequence, length(CAST(body AS BLOB)), "
                                   "CASE WHEN length(CAST(body AS BLOB)) <= 65536 THEN body END "
                                   "FROM records WHERE sequence > 0 ORDER BY sequence");
  while (rows.step()) {
    const auto bytes = rows.integer(1);
    if (rows.integer(0) != static_cast<std::int64_t>(first + result.records) ||
        result.records >= max_records || bytes <= 0 ||
        static_cast<std::uint64_t>(bytes) > max_record ||
        result.bytes + static_cast<std::uint64_t>(bytes) > max_total)
      throw std::invalid_argument("trading archive has a gap or an oversized record");
    const auto body = rows.text(2);
    result.digest = sha256_bytes(result.digest + ":" + std::to_string(first + result.records) +
                                 ":" + std::to_string(bytes) + ":" + sha256_bytes(body));
    if (destination) {
      sqlite::Database::Statement insert(*destination, "INSERT INTO records VALUES(?, ?)");
      insert.bind(1, static_cast<std::int64_t>(first + result.records)).bind(2, body).step();
    }
    ++result.records;
    result.bytes += static_cast<std::uint64_t>(bytes);
  }
  if (!result.records)
    throw std::invalid_argument("trading archive is empty");
  return result;
}
Json read_header(sqlite::Database& database) {
  validate_format(database);
  sqlite::Database::Statement query(database,
                                    "SELECT sequence, length(CAST(body AS BLOB)), "
                                    "CASE WHEN length(CAST(body AS BLOB)) <= 65536 THEN body END "
                                    "FROM records ORDER BY sequence LIMIT 1");
  if (!query.step() || query.integer(0) != 0 || query.integer(1) <= 0 ||
      query.integer(1) > static_cast<std::int64_t>(max_record))
    throw std::invalid_argument("invalid trading journal header");
  return parse_json(query.text(2), max_record);
}
} // namespace
void journal_segment_records_for_testing(std::size_t records) {
  test_segment_records = records;
}
SqliteJournal::SqliteJournal(std::filesystem::path directory, std::set<std::string> sidecars,
                             std::function<void(const Json&)> validate_header)
    : directory_(std::move(directory)), sidecar_directories_(std::move(sidecars)),
      validate_header_(std::move(validate_header)) {
  for (const auto& name : sidecar_directories_)
    if (name.empty() || name == "." || name == ".." ||
        std::filesystem::path(name).filename() != std::filesystem::path(name) ||
        name.starts_with(file_name))
      throw std::invalid_argument("invalid journal sidecar directory");
  sidecar_directories_.insert("archives");
  if (!directory_.is_absolute())
    throw std::invalid_argument("trading record directory must be an absolute path");
}
SqliteJournal::~SqliteJournal() {
  stop();
}
void check_journal_directory(const std::filesystem::path& directory,
                             const std::set<std::string>& sidecars) {
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    const auto name = entry.path().filename().string();
    if (name == "archives" || sidecars.contains(name)) {
      if (!entry.is_directory() || entry.is_symlink())
        throw std::invalid_argument("invalid journal sidecar directory");
      continue;
    }
    if (name == file_name || name == std::string(file_name) + "-wal" ||
        name == std::string(file_name) + "-shm" || name == std::string(file_name) + "-journal") {
      if (!entry.is_regular_file() || entry.is_symlink())
        throw std::invalid_argument("invalid internal file in trading directory");
      continue;
    }
    throw std::invalid_argument(
        "trading directory contains unknown files; use a dedicated directory");
  }
}
void SqliteJournal::start() {
  if (database_)
    throw std::logic_error("storage plugin already started");
  if (!std::filesystem::is_directory(directory_) || std::filesystem::is_symlink(directory_))
    throw std::invalid_argument("choose an existing trading record directory");
  check_journal_directory(directory_, sidecar_directories_);
  const bool existing = std::filesystem::exists(directory_ / file_name);
  const auto validate = [&](sqlite::Database& database) {
    validate_format(database);
    if (validate_header_)
      validate_header_(read_header(database));
  };
  if (existing) {
    sqlite::Database inspect(directory_ / file_name, sqlite::Database::Access::read_only);
    validate(inspect);
  }
  auto database = std::make_unique<sqlite::Database>(
      directory_ / file_name, sqlite::Database::Access::writer,
      existing ? std::function<void(sqlite::Database&)>(validate) : nullptr);
  if (!existing) {
    sqlite::Database::Transaction schema(*database);
    initialize(*database);
    schema.commit();
  }
  database_ = std::move(database);
  try {
    poisoned_ = false;
    first_ = 1;
    segments_ = 0;
    sqlite::Database::Statement segments(
        *database_, "SELECT first,name,digest,records,bytes FROM segments ORDER BY first");
    while (segments.step()) {
      Archive archive{segments.text(1), segments.text(2),
                      static_cast<std::uint64_t>(segments.integer(3)),
                      static_cast<std::uint64_t>(segments.integer(4)),
                      static_cast<std::uint64_t>(segments.integer(0))};
      if (archive.first != first_)
        throw std::invalid_argument("trading journal has a gap or an oversized record");
      verify_archive(directory_, archive);
      first_ += archive.records;
      ++segments_;
    }
    sqlite::Database::Statement usage(
        *database_, "SELECT COUNT(*), COALESCE(MIN(sequence),0), COALESCE(MAX(sequence),-1), "
                    "COALESCE(SUM(length(CAST(body AS BLOB))),0), "
                    "COALESCE(MAX(length(CAST(body AS BLOB))),0) FROM records WHERE sequence != 0");
    usage.step();
    const auto events = static_cast<std::uint64_t>(usage.integer(0));
    if (events >= max_records ||
        (events && (usage.integer(1) != static_cast<std::int64_t>(first_) ||
                    usage.integer(2) != static_cast<std::int64_t>(first_ + events - 1))) ||
        usage.integer(3) > static_cast<std::int64_t>(max_total) ||
        usage.integer(4) > static_cast<std::int64_t>(max_record))
      throw std::invalid_argument("trading journal has a gap or an oversized record");
    sqlite::Database::Statement header(
        *database_, "SELECT length(CAST(body AS BLOB)) FROM records WHERE sequence=0");
    header_bytes_ = header.step() ? static_cast<std::uintmax_t>(header.integer(0)) : 0;
    if ((!header_bytes_ && (events || segments_)) || header_bytes_ > max_record ||
        header_bytes_ + usage.integer(3) > max_total)
      throw std::invalid_argument("invalid trading journal header");
    count_ = header_bytes_ ? first_ + events : 0;
    bytes_ = header_bytes_ + usage.integer(3);
  } catch (...) {
    stop();
    throw;
  }
}
Json read_journal_header(const std::filesystem::path& directory,
                         const std::set<std::string>& sidecars) {
  if (!directory.is_absolute() || !std::filesystem::is_directory(directory) ||
      std::filesystem::is_symlink(directory))
    throw std::invalid_argument("choose an existing trading record directory");
  check_journal_directory(directory, sidecars);
  sqlite::Database database(directory / file_name, sqlite::Database::Access::read_only);
  database.execute("BEGIN");
  return read_header(database);
}
void SqliteJournal::stop() noexcept {
  database_.reset();
}
SqliteJournal::Capacity SqliteJournal::capacity() const {
  if (!database_)
    throw std::logic_error("storage plugin is not started");
  return {count_ ? count_ - first_ + 1 : 0, max_records, bytes_, max_total, count_, segments_};
}
void SqliteJournal::verify_archive(const std::filesystem::path& directory, const Archive& archive) {
  if (archive.name.empty() || archive.name.size() > 128 ||
      std::filesystem::path(archive.name).filename() != std::filesystem::path(archive.name) ||
      !archive.name.starts_with("record.") || !archive.name.ends_with(".sqlite") ||
      archive.digest.size() != 64 || !archive.records || archive.records >= max_records ||
      !archive.first || archive.first > 9007199254740991ULL - archive.records ||
      archive.bytes > max_total)
    throw std::invalid_argument("invalid trading archive reference");
  const auto parent = directory / "archives";
  const auto file = parent / archive.name;
  if (std::filesystem::is_symlink(parent) || !std::filesystem::is_directory(parent) ||
      std::filesystem::is_symlink(file) || !std::filesystem::is_regular_file(file) ||
      std::filesystem::file_size(file) > 512ULL * 1024 * 1024)
    throw std::invalid_argument("trading archive is missing or invalid");
  sqlite::Database database(file, sqlite::Database::Access::read_only);
  database.execute("BEGIN");
  validate_format(database, true);
  const auto actual = archive_rows(database, nullptr, archive.first);
  if (actual.digest != archive.digest || actual.records != archive.records ||
      actual.bytes != archive.bytes)
    throw std::invalid_argument("trading archive contents changed");
}
SqliteJournal::Archive SqliteJournal::seal() const {
  const auto parent = directory_ / "archives";
  create_directories_durably(parent);
  std::filesystem::permissions(parent, std::filesystem::perms::owner_all);
  const auto identity = unique_process_id();
  const auto pending = parent / ("pending." + identity + ".sqlite");
  Archive archive;
  {
    // One transaction preserves exact serialized rows with a bounded working
    // set. Each archive is independent, sealed and never reused or overwritten.
    sqlite::Database destination(pending);
    sqlite::Database::Transaction transaction(destination);
    initialize(destination);
    destination.execute("INSERT INTO meta VALUES('sealed','1')");
    archive = archive_rows(*database_, &destination, first_);
    transaction.commit();
    // A sealed archive is one standalone file; later read-only validation
    // must not create WAL coordination files beside it.
    destination.execute("PRAGMA wal_checkpoint(TRUNCATE)");
    destination.execute("PRAGMA journal_mode=DELETE");
  }
  if (archive.records != count_ - first_ || archive.bytes != bytes_ - header_bytes_)
    throw std::runtime_error("trading journal changed during archival");
  archive.name = "record." + identity + ".sqlite";
  publish_file_durably(pending, parent / archive.name);
  verify_archive(directory_, archive);
  return archive;
}
std::vector<Json> SqliteJournal::read(std::uint64_t first) const {
  return read_records(first, page_size);
}
std::vector<Json> SqliteJournal::read_records(std::uint64_t first, std::size_t count) const {
  if (!database_)
    throw std::logic_error("storage plugin is not started");
  if (first >= count_)
    return {};
  if (first == 0)
    return {read_header(*database_)};
  std::unique_ptr<sqlite::Database> archive;
  auto* source = database_.get();
  if (first < first_) {
    sqlite::Database::Statement segment(
        *database_, "SELECT name FROM segments WHERE first <= ? ORDER BY first DESC LIMIT 1");
    segment.bind(1, static_cast<std::int64_t>(first));
    segment.step();
    archive = std::make_unique<sqlite::Database>(directory_ / "archives" / segment.text(0),
                                                 sqlite::Database::Access::read_only);
    source = archive.get();
  }
  sqlite::Database::Statement query(
      *source, "SELECT body FROM records WHERE sequence >= ? ORDER BY sequence LIMIT ?");
  query.bind(1, static_cast<std::int64_t>(first)).bind(2, static_cast<std::int64_t>(count));
  std::vector<Json> result;
  while (query.step())
    result.push_back(parse_json(query.text(0), max_record));
  return result;
}
std::optional<std::uint64_t> SqliteJournal::command_sequence(std::string_view id) const {
  if (!database_)
    throw std::logic_error("storage plugin is not started");
  sqlite::Database::Statement query(*database_, "SELECT sequence FROM commands WHERE id=?");
  query.bind(1, id);
  if (!query.step())
    return std::nullopt;
  const auto sequence = query.integer(0);
  if (sequence <= 0 || static_cast<std::uint64_t>(sequence) >= count_)
    throw std::invalid_argument("invalid persisted trading command");
  return static_cast<std::uint64_t>(sequence);
}
Json SqliteJournal::command(std::string_view id) const {
  const auto sequence = command_sequence(id);
  if (!sequence)
    return nullptr;
  return read_records(*sequence, 1).at(0);
}
std::optional<std::uint64_t> SqliteJournal::order_sequence(std::string_view id) const {
  const auto index = order_index(id);
  return index ? std::optional(index->sequence) : std::nullopt;
}
std::optional<SqliteJournal::OrderIndex> SqliteJournal::order_index(std::string_view id) const {
  if (!database_)
    throw std::logic_error("storage plugin is not started");
  sqlite::Database::Statement query(
      *database_, "SELECT sequence,trading_day,broker_key,COALESCE(excluded_by,0) FROM commands "
                  "WHERE order_id=?");
  query.bind(1, id);
  if (!query.step())
    return std::nullopt;
  const auto sequence = query.integer(0);
  if (sequence <= 0 || static_cast<std::uint64_t>(sequence) >= count_)
    throw std::invalid_argument("invalid persisted trading command");
  const auto excluded = static_cast<std::uint64_t>(query.integer(3));
  if (excluded && (excluded <= static_cast<std::uint64_t>(sequence) || excluded >= count_))
    throw std::invalid_argument("trading order index does not match its journal record");
  return OrderIndex{std::string(id), query.text(1), query.text(2),
                    static_cast<std::uint64_t>(sequence), excluded};
}
std::vector<SqliteJournal::OrderIndex> SqliteJournal::order_identities(std::string_view day,
                                                                       std::uint64_t after) const {
  if (!database_)
    throw std::logic_error("storage plugin is not started");
  sqlite::Database::Statement query(
      *database_, "SELECT o.order_id,o.broker_key,o.sequence FROM commands o "
                  "WHERE o.trading_day=? AND o.sequence>? AND o.excluded_by IS NULL AND NOT EXISTS "
                  "(SELECT 1 FROM commands newer WHERE newer.trading_day=o.trading_day "
                  "AND newer.broker_key=o.broker_key AND newer.sequence>o.sequence "
                  "AND newer.excluded_by IS NULL) ORDER BY o.sequence LIMIT ?");
  query.bind(1, day)
      .bind(2, static_cast<std::int64_t>(after))
      .bind(3, static_cast<std::int64_t>(SqliteJournal::page_size));
  std::vector<OrderIndex> result;
  while (query.step())
    result.push_back({query.text(0), std::string(day), query.text(1),
                      static_cast<std::uint64_t>(query.integer(2)), 0});
  return result;
}
Json SqliteJournal::record(std::uint64_t sequence) const {
  return read_records(sequence, 1).at(0);
}
std::uint64_t SqliteJournal::order_identity_count() const {
  if (!database_)
    throw std::logic_error("storage plugin is not started");
  sqlite::Database::Statement query(*database_,
                                    "SELECT COUNT(*) FROM commands WHERE order_id IS NOT NULL");
  query.step();
  return static_cast<std::uint64_t>(query.integer(0));
}
Json SqliteJournal::order(std::string_view id) const {
  const auto sequence = order_sequence(id);
  if (!sequence)
    return nullptr;
  return read_records(*sequence, 1).at(0);
}
void SqliteJournal::append(const Json& record) {
  append(record, {});
}
void SqliteJournal::append(const Json& record, std::string_view command_id,
                           std::string_view order_id, std::string_view excluded_order) {
  if (!database_ || poisoned_)
    throw std::runtime_error("trading storage is not writable; close and reopen the session");
  if (count_ >= 9007199254740991ULL)
    throw std::invalid_argument("trading journal capacity reached");
  const auto data = record.dump();
  if (data.size() > max_record)
    throw std::invalid_argument("trading journal record too large");
  try {
    const auto testing = test_segment_records.load();
    const auto threshold = testing ? testing : max_records - 1;
    const bool rotate =
        count_ > first_ && (count_ - first_ >= threshold || bytes_ + data.size() > max_total);
    std::optional<Archive> archive;
    if (rotate)
      archive = seal();
    sqlite::Database::Transaction commit(*database_);
    if (archive) {
      sqlite::Database::Statement segment(*database_, "INSERT INTO segments VALUES(?,?,?,?,?)");
      segment.bind(1, static_cast<std::int64_t>(archive->first))
          .bind(2, archive->name)
          .bind(3, archive->digest)
          .bind(4, static_cast<std::int64_t>(archive->records))
          .bind(5, static_cast<std::int64_t>(archive->bytes))
          .step();
      database_->execute("DELETE FROM records WHERE sequence > 0");
    }
    sqlite::Database::Statement insert(*database_, "INSERT INTO records VALUES(?, ?)");
    insert.bind(1, static_cast<std::int64_t>(count_)).bind(2, data).step();
    if (!command_id.empty()) {
      sqlite::Database::Statement command(
          *database_,
          "INSERT INTO commands VALUES(?, ?, NULLIF(?, ''), NULLIF(?, ''), NULLIF(?, ''), NULL)");
      command.bind(1, command_id)
          .bind(2, static_cast<std::int64_t>(count_))
          .bind(3, order_id)
          .bind(4, order_id.empty() ? std::string{} : record.at("trading_day").get<std::string>())
          .bind(5, order_id.empty() ? std::string{} : record.at("broker_key").get<std::string>())
          .step();
    }
    if (!excluded_order.empty()) {
      sqlite::Database::Statement exclude(*database_,
                                          "UPDATE commands SET excluded_by=? WHERE order_id=? AND "
                                          "excluded_by IS NULL RETURNING id");
      exclude.bind(1, static_cast<std::int64_t>(count_)).bind(2, excluded_order);
      if (!exclude.step())
        throw std::invalid_argument("trading order index does not match its journal record");
    }
    commit.commit();
    if (archive) {
      first_ = count_;
      ++segments_;
      bytes_ = header_bytes_;
    }
    if (!count_)
      header_bytes_ = data.size();
    ++count_;
    bytes_ += data.size();
  } catch (...) {
    // A failed commit may still have reached disk; recover by reopening.
    poisoned_ = true;
    throw;
  }
}
} // namespace asterion
