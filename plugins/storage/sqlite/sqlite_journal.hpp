#pragma once
#include <asterion/kernel/journal_port.hpp>
#include <filesystem>
#include <memory>
#include <set>
namespace asterion {
namespace sqlite {
class Database;
}
// Rejects a directory that is not a dedicated journal directory, without
// creating or changing anything; the same check start() applies.
void check_journal_directory(const std::filesystem::path& directory,
                             const std::set<std::string>& sidecar_directories);
// Ordered journal in a dedicated directory, stored as journal.sqlite. The
// directory may also hold the declared sidecar directories and nothing else.
class SqliteJournal final : public JournalPort {
public:
  explicit SqliteJournal(std::filesystem::path directory,
                         std::set<std::string> sidecar_directories = {});
  ~SqliteJournal() override;
  PluginDescriptor descriptor() const override;
  void start() override;
  void stop() noexcept override;
  std::vector<Json> read() const override;
  void append(const Json& record) override;

private:
  std::filesystem::path directory_;
  std::set<std::string> sidecar_directories_;
  std::unique_ptr<sqlite::Database> database_;
  bool poisoned_ = false;
  std::size_t count_ = 0;
  std::uintmax_t bytes_ = 0;
};
} // namespace asterion
