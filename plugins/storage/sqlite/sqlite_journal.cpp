#include "sqlite_journal.hpp"
#include "sqlite_database.hpp"
#include <stdexcept>
namespace asterion {
namespace {
constexpr auto file_name = "journal.sqlite";
constexpr auto format = "asterion.journal.v1";
// A replay bar can produce an advance, a strategy target and a settlement.
// Leave room for 20000 of each, 10000 orders and their cancellations, and
// control commands. This is a record budget, independent of the bar budget.
constexpr std::size_t max_records = 100001;
// The first record may carry a whole input dataset (up to 200000 bars).
constexpr std::uintmax_t max_first = 128ULL * 1024 * 1024, max_record = 65536,
                         max_total = 256ULL * 1024 * 1024;
std::uintmax_t limit(std::size_t index) {
  return index == 0 ? max_first : max_record;
}
} // namespace
SqliteJournal::SqliteJournal(std::filesystem::path directory, std::set<std::string> sidecars)
    : directory_(std::move(directory)), sidecar_directories_(std::move(sidecars)) {
  for (const auto& name : sidecar_directories_)
    if (name.empty() || name == "." || name == ".." ||
        std::filesystem::path(name).filename() != std::filesystem::path(name) ||
        name.starts_with(file_name))
      throw std::invalid_argument("invalid journal sidecar directory");
  if (!directory_.is_absolute())
    throw std::invalid_argument("trading record directory must be an absolute path");
}
SqliteJournal::~SqliteJournal() {
  stop();
}
PluginDescriptor SqliteJournal::descriptor() const {
  return {"asterion.storage.sqlite-journal", PluginKind::storage, plugin_contract_version, {}};
}
void check_journal_directory(const std::filesystem::path& directory,
                             const std::set<std::string>& sidecars) {
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    const auto name = entry.path().filename().string();
    if (sidecars.contains(name)) {
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
  auto database = std::make_unique<sqlite::Database>(directory_ / file_name);
  sqlite::Database::Transaction schema(*database);
  database->execute("CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT NOT NULL)"
                    " STRICT");
  database->execute("CREATE TABLE IF NOT EXISTS records(sequence INTEGER PRIMARY KEY,"
                    " body TEXT NOT NULL) STRICT");
  database->execute(std::string("INSERT OR IGNORE INTO meta VALUES('format','") + format + "')");
  schema.commit();
  sqlite::Database::Statement stored(*database, "SELECT value FROM meta WHERE key='format'");
  if (!stored.step() || stored.text(0) != format)
    throw std::invalid_argument("unsupported journal format");
  database_ = std::move(database);
  try {
    poisoned_ = false;
    const auto records = read();
    count_ = records.size();
    bytes_ = 0;
    for (const auto& record : records)
      bytes_ += record.dump().size();
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
  sqlite::Database::Statement stored(database, "SELECT value FROM meta WHERE key='format'");
  if (!stored.step() || stored.text(0) != format)
    throw std::invalid_argument("unsupported journal format");
  // Bound bytes before copying the body into process memory, including UTF-8.
  sqlite::Database::Statement query(
      database, "SELECT sequence, length(CAST(body AS BLOB)), "
                "CASE WHEN length(CAST(body AS BLOB)) <= 134217728 THEN body END "
                "FROM records ORDER BY sequence LIMIT 1");
  if (!query.step() || query.integer(0) != 0 || query.integer(1) <= 0 ||
      query.integer(1) > static_cast<std::int64_t>(max_first))
    throw std::invalid_argument("invalid trading journal header");
  return parse_json(query.text(2), max_first);
}
void SqliteJournal::stop() noexcept {
  database_.reset();
}
std::vector<Json> SqliteJournal::read() const {
  if (!database_)
    throw std::logic_error("storage plugin is not started");
  sqlite::Database::Statement query(*database_,
                                    "SELECT sequence, body FROM records ORDER BY sequence");
  std::vector<Json> result;
  std::uintmax_t total = 0;
  while (query.step()) {
    const auto body = query.text(1);
    total += body.size();
    if (query.integer(0) != static_cast<std::int64_t>(result.size()) ||
        result.size() >= max_records || body.size() > limit(result.size()) || total > max_total)
      throw std::invalid_argument("trading journal has a gap or an oversized record");
    result.push_back(parse_json(body, max_first));
  }
  return result;
}
void SqliteJournal::append(const Json& record) {
  if (!database_ || poisoned_)
    throw std::runtime_error("trading storage is not writable; close and reopen the session");
  if (count_ >= max_records)
    throw std::invalid_argument("trading journal capacity reached");
  const auto data = record.dump();
  if (data.size() > limit(count_) || bytes_ + data.size() > max_total)
    throw std::invalid_argument("trading journal record too large");
  try {
    sqlite::Database::Transaction commit(*database_);
    sqlite::Database::Statement insert(*database_, "INSERT INTO records VALUES(?, ?)");
    insert.bind(1, static_cast<std::int64_t>(count_)).bind(2, data).step();
    commit.commit();
    ++count_;
    bytes_ += data.size();
  } catch (...) {
    // A failed commit may still have reached disk; recover by reopening.
    poisoned_ = true;
    throw;
  }
}
} // namespace asterion
