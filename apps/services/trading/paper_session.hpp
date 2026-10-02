#pragma once
#include "sqlite_journal.hpp"
#include <asterion/v1/trading.pb.h>
#include "risk_module.hpp"
#include <optional>
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
  protocol::v1::PaperHistoryUsage history_usage(const std::string& dataset_id) const;
  bool recovery_required() const noexcept { return failed_; }

private:
  std::unique_ptr<PaperExecution> build(const Json& manifest);
  // Rebuilds in-memory state from the manifest and the committed commands.
  // Used only when a failed command or commit left the engine modified.
  void restore();
  // Admission against committed history; replay must not validate against its future.
  void validate_history(const Json& command) const;
  void apply(PaperExecution& engine, Json& authorization, Json& replay, const Json& command) const;
  SqliteJournal journal_;
  std::optional<risk_providers::Module> risk_module_;
  std::unique_ptr<PaperExecution> engine_;
  Json manifest_;
  Json authorization_ = nullptr;
  Json replay_ = nullptr;
  // Day-end settlement of every contract, fixed by the manifest.
  std::shared_ptr<const PaperReplaySchedule> schedule_;
  std::string dataset_revision_;
  std::map<std::string, std::pair<bool, bool>> history_roles_;
  std::map<std::string, Json> commands_;
  // Committed commands in journal order; points into commands_ nodes.
  std::vector<const Json*> sequence_;
  bool failed_ = false;
};
} // namespace asterion::trading
