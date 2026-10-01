#pragma once
#include <cstdint>
#include <filesystem>
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
  explicit Database(const std::filesystem::path& file);
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
void fail_next_commits_for_testing(int count);
} // namespace asterion::sqlite
