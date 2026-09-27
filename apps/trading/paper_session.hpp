#pragma once
#include "file_journal.hpp"
#include "paper_execution.hpp"
#include "replay_schedule.hpp"
#include <map>
#include <vector>
namespace asterion::trading {
class PaperSession {
public:
  // create_manifest absent means recover an existing session without rewriting
  // it.
  PaperSession(std::filesystem::path directory, const Json& create_manifest = nullptr);
  ~PaperSession();
  void execute(const Json& command);
  Json snapshot() const;
  bool recovery_required() const noexcept { return failed_; }

private:
  static std::unique_ptr<PaperExecution> build(const Json& manifest);
  // Rebuilds in-memory state from the manifest and the committed commands.
  // Used only when a failed command or commit left the engine modified.
  void restore();
  void apply(PaperExecution& engine, Json& authorization, Json& replay,
             std::shared_ptr<const PaperReplaySchedule>& schedule, const Json& command) const;
  FileJournal journal_;
  std::unique_ptr<PaperExecution> engine_;
  Json manifest_;
  Json authorization_ = nullptr;
  Json replay_ = nullptr;
  std::shared_ptr<const PaperReplaySchedule> schedule_;
  std::string dataset_revision_;
  InstrumentId instrument_;
  std::map<std::string, Json> commands_;
  // Committed commands in journal order; points into commands_ nodes.
  std::vector<const Json*> sequence_;
  bool failed_ = false;
};
} // namespace asterion::trading
