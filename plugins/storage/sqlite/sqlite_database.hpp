#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
struct sqlite3;
struct sqlite3_stmt;
namespace asterion::sqlite {
// One owned connection with full durability: every committed transaction is
// synced before it returns. Not shared between threads; the owner serializes.
class Database {
public:
  enum class Access { writer, read_only };
  // Existing-store validation runs before enabling writer pragmas or creating
  // a journal. Callers also perform read-only preflight before taking ownership.
  explicit Database(const std::filesystem::path& file, Access access = Access::writer,
                    std::function<void(Database&)> before_write = {});
  ~Database();
  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;
  void execute(std::string_view sql);
  class Statement {
  public:
    Statement(Database& database, std::string_view sql);
    ~Statement();
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    Statement& bind(int index, std::int64_t value);
    Statement& bind(int index, std::string_view text);
    // True while a row is available.
    Statement& bind_blob(int index, std::string_view bytes);
    std::string blob(int column) const;
    bool step();
    std::int64_t integer(int column) const;
    std::string text(int column) const;

  private:
    Database& database_;
    sqlite3_stmt* statement_ = nullptr;
  };
  // Commits on commit(); rolls back when destroyed uncommitted.
  class Transaction {
  public:
    explicit Transaction(Database& database);
    ~Transaction();
    void commit();

  private:
    Database& database_;
    bool open_ = true;
  };

private:
  friend class Statement;
  [[noreturn]] void fail(std::string_view operation) const;
  sqlite3* handle_ = nullptr;
};
// Test hook: the next `count` transaction commits in this process fail and roll
// back, as an I/O error before acknowledgement would.
// Deterministic durability barrier used by account-thread integration tests.
void hold_commits_for_testing(bool hold);
bool commit_waiting_for_testing();
void fail_next_commits_for_testing(int count);
} // namespace asterion::sqlite
