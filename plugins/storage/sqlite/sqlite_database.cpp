#include "sqlite_database.hpp"
#include <sqlite3.h>
#include <atomic>
#include <stdexcept>
namespace asterion::sqlite {
namespace {
std::atomic<int> injected_failures{0};
}
void fail_next_commits_for_testing(int count) {
  injected_failures = count;
}
Database::Database(const std::filesystem::path& file, Access access) {
  if (!file.is_absolute() || std::filesystem::is_symlink(file))
    throw std::invalid_argument("database requires an absolute regular file path");
  // No shared cache, no URI parsing and no extension loading: the path is data.
  if (access == Access::read_only && !std::filesystem::is_regular_file(file))
    throw std::invalid_argument("database requires an existing regular file");
  const auto flags = (access == Access::read_only ? SQLITE_OPEN_READONLY
                                                  : SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE) |
                     SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_PRIVATECACHE;
  if (sqlite3_open_v2(file.string().c_str(), &handle_, flags, nullptr) != SQLITE_OK) {
    sqlite3_close(handle_);
    handle_ = nullptr;
    throw std::runtime_error("cannot open database");
  }
  try {
    sqlite3_extended_result_codes(handle_, 1);
    sqlite3_busy_timeout(handle_, 0);
    execute("PRAGMA trusted_schema=OFF");
    if (access == Access::read_only) {
      execute("PRAGMA query_only=ON");
      return;
    }
    // One writer owns the file; WAL with exclusive locking keeps no shared
    // memory file, and FULL sync makes each commit durable before returning.
    execute("PRAGMA locking_mode=EXCLUSIVE");
    execute("PRAGMA journal_mode=WAL");
    execute("PRAGMA synchronous=FULL");
    execute("PRAGMA foreign_keys=ON");
    // Take the exclusive lock now so a second writer fails at open.
    execute("BEGIN EXCLUSIVE");
    execute("COMMIT");
  } catch (...) {
    sqlite3_close(handle_);
    handle_ = nullptr;
    throw;
  }
}
Database::~Database() {
  sqlite3_close(handle_);
}
void Database::fail(std::string_view operation) const {
  const auto code = sqlite3_extended_errcode(handle_);
  if ((code & 0xff) == SQLITE_BUSY || (code & 0xff) == SQLITE_LOCKED)
    throw std::runtime_error("database is in use by another writer");
  if ((code & 0xff) == SQLITE_CORRUPT || (code & 0xff) == SQLITE_NOTADB)
    throw std::invalid_argument("database file is corrupted");
  throw std::runtime_error(std::string("database ") + std::string(operation) + " failed");
}
void Database::execute(std::string_view sql) {
  Statement statement(*this, sql);
  while (statement.step()) {
  }
}
Database::Statement::Statement(Database& database, std::string_view sql) : database_(database) {
  if (sqlite3_prepare_v2(database_.handle_, sql.data(), static_cast<int>(sql.size()), &statement_,
                         nullptr) != SQLITE_OK)
    database_.fail("prepare");
}
Database::Statement::~Statement() {
  sqlite3_finalize(statement_);
}
Database::Statement& Database::Statement::bind(int index, std::int64_t value) {
  if (sqlite3_bind_int64(statement_, index, value) != SQLITE_OK)
    database_.fail("bind");
  return *this;
}
Database::Statement& Database::Statement::bind(int index, std::string_view text) {
  if (sqlite3_bind_text64(statement_, index, text.data(), text.size(), SQLITE_TRANSIENT,
                          SQLITE_UTF8) != SQLITE_OK)
    database_.fail("bind");
  return *this;
}
bool Database::Statement::step() {
  const auto status = sqlite3_step(statement_);
  if (status == SQLITE_ROW)
    return true;
  if (status == SQLITE_DONE)
    return false;
  database_.fail("statement");
}
std::int64_t Database::Statement::integer(int column) const {
  return sqlite3_column_int64(statement_, column);
}
std::string Database::Statement::text(int column) const {
  const auto* data = reinterpret_cast<const char*>(sqlite3_column_text(statement_, column));
  return data
             ? std::string(data, static_cast<std::size_t>(sqlite3_column_bytes(statement_, column)))
             : std::string();
}
Database::Transaction::Transaction(Database& database) : database_(database) {
  database_.execute("BEGIN IMMEDIATE");
}
Database::Transaction::~Transaction() {
  if (open_)
    sqlite3_exec(database_.handle_, "ROLLBACK", nullptr, nullptr, nullptr);
}
void Database::Transaction::commit() {
  if (injected_failures.load() > 0 && injected_failures.fetch_sub(1) > 0)
    throw std::runtime_error("database commit failed");
  database_.execute("COMMIT");
  open_ = false;
}
} // namespace asterion::sqlite
