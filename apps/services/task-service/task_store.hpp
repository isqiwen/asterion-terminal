#pragma once
#include <asterion/foundation/time.hpp>
#include <asterion/protocol/task.hpp>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>
#include <string_view>
namespace asterion::tasks {
struct Identity {
  std::string instance, data_instance;
};
// Serialized application-level durable task state. Agent owns OS processes;
// attempt tokens fence late worker reports after retry or service restart.
class Store {
  struct Impl;

public:
  explicit Store(std::filesystem::path directory, Identity identity,
                 std::shared_ptr<const Clock> clock = std::make_shared<SystemClock>());
  ~Store();
  // State decisions and their persistence have separate owners. The host keeps
  // at most one unconfirmed change per task and serializes database access.
  class Change {
  public:
    Change(Change&&) noexcept;
    Change& operator=(Change&&) noexcept;
    ~Change();
    bool needs_write() const;
    const task::v1::Task& task() const;
    const std::string& token() const;
    void persist(); // Journal thread only; never changes the active state.

  private:
    friend class Store;
    friend struct Impl;
    struct State;
    explicit Change(std::unique_ptr<State>);
    std::unique_ptr<State> state_;
  };
  void confirm(Change&); // State owner only, after successful persistence.
  void persistence_failed();
  bool is_active(const std::string&) const;
  // Synchronous composition for stopped-service operations and storage tests.
  // The running host uses persist/confirm separately.
  Change commit(Change);
  class Submission {
  public:
    Submission(Submission&&) noexcept = default;
    Submission& operator=(Submission&&) noexcept = default;
    Submission(const Submission&) = delete;
    // File worker only: owns its definition and an optional existing-input snapshot.
    void prepare_files();

  private:
    friend class Store;
    Submission(Impl*, std::filesystem::path, task::v1::Task);
    Impl* owner_;
    std::filesystem::path root_;
    task::v1::Task task_;
    std::optional<task::v1::StoredTaskRecord> existing_;
    std::string digest_;
    std::uint64_t bytes_ = 0;
    bool admitted_ = false, created_ = false, prepared_ = false;
  };
  // Definition validation may be expensive; construct submissions on the file worker.
  Submission submission(const std::string&, factor::v1::DailyFactorInput) const;
  Submission submission(const std::string&, backtest::v1::BacktestInput) const;
  Submission submission(const std::string&, factor::v1::FactorInput) const;
  Submission submission(const data::v1::DownloadAuthorization&) const;
  void admit_submission(Submission&);
  [[nodiscard]] Change register_submission(Submission&);
  // Release an unconfirmed reservation, retaining any created directory.
  // True means file preparation failed after creating evidence and recovery is required.
  bool abandon_submission(Submission&);
  bool has_submission() const;
  // Immutable input I/O runs without the task-state lock. Confirmation returns
  // current metadata so a concurrent cancellation is never overwritten.
  class InputRead {
  public:
    void load();
    void load_for_claim();

  private:
    friend class Store;
    InputRead(task::v1::Task, std::filesystem::path, std::string, std::uint64_t);
    task::v1::Task task_;
    std::filesystem::path path_;
    std::string digest_;
    std::uint64_t bytes_;
    bool loaded_ = false;
    task::v1::TaskExecution execution_;
  };
  InputRead prepare_input(const std::string&) const;
  task::v1::Task confirm_input(InputRead) const;
  task::v1::TaskAttempt confirm_attempt(InputRead) const;
  // Capture metadata under the host lock, verify owned input/result files outside
  // it, then confirm identity under the lock. Reads never execute an algorithm.
  class ResultRead {
  public:
    void verify();

  private:
    friend class Store;
    ResultRead(task::v1::Task, std::filesystem::path, std::string, std::uint64_t,
               bool verify_content, bool include_input);
    task::v1::Task task_;
    std::filesystem::path directory_;
    std::string input_digest_;
    std::uint64_t input_bytes_;
    bool verify_content_, include_input_, verified_ = false;
    task::v1::TaskResponse response_;
  };
  ResultRead prepare_result(const std::string&) const;
  task::v1::TaskResponse confirm_result(ResultRead) const;
  // Owns an immutable attempt/input/result snapshot, never a reference to Store.
  // prepare_finish/finish require host serialization. prepare_payload verifies
  // and durably writes the attempt file outside it; only finish decides success.
  class Completion {
  public:
    void prepare_payload();
    data::v1::DownloadPreparation download_preparation() const;

  private:
    friend class Store;
    Completion(Identity identity, task::v1::Task task, task::v1::TaskFinish result,
               std::filesystem::path directory, std::string input_digest,
               std::uint64_t input_bytes);
    task::v1::Task task_;
    std::filesystem::path directory_;
    std::string input_digest_;
    std::uint64_t input_bytes_;
    task::v1::TaskFinish result_;
    Identity identity_;
    std::string result_digest_;
    bool prepared_ = false;
  };
  task::v1::Task submit(const std::string&, factor::v1::DailyFactorInput);
  factor::v1::DailyFactorResult daily_factor_result(const std::string&) const;
  Completion prepare_finish(const task::v1::TaskFinish& result);
  [[nodiscard]] Change finish(Completion completion);
  // Cancellation and publication are ordered by the same durable task writer.
  // A confirmed PUBLISHING change transfers custody; the worker lease no longer applies.
  [[nodiscard]] Change prepare_publication(Completion, const data::v1::PreparedDownload&);
  std::vector<data::v1::DownloadPublication> pending_publications() const;
  [[nodiscard]] Change confirm_publication(const data::v1::PublishedDownload&);
  task::v1::Task submit(const std::string& id, backtest::v1::BacktestInput input);
  task::v1::Task submit(const std::string& id, factor::v1::FactorInput input);
  task::v1::Task submit(const data::v1::DownloadAuthorization&);
  data::v1::DailyDownloadResult daily_result(const std::string&) const;
  data::v1::MinuteDownloadResult minute_result(const std::string&) const;
  // Full immutable input is loaded and integrity-checked only for explicit reads.
  task::v1::Task get(const std::string& id) const;
  task::v1::Task describe(const std::string& id) const;
  task::v1::TaskList list(unsigned limit = 200, unsigned before_sequence = 0) const;
  std::vector<task::v1::Task> active_tasks() const;
  class HistoryRead {
  public:
    void load_page(); // File worker; consumes only the captured metadata page.
    data::v1::HistoryUsage take() { return std::move(result_); }

  private:
    friend class Store;
    HistoryRead(std::filesystem::path root, std::string dataset, std::uint32_t sequence)
        : root_(std::move(root)), through_(sequence) {
      result_.set_dataset_id(std::move(dataset));
    }
    std::filesystem::path root_;
    std::uint32_t through_, after_ = 0;
    std::vector<task::v1::StoredTaskRecord> page_;
    data::v1::HistoryUsage result_;
  };
  HistoryRead prepare_history_usage(const std::string&) const;
  bool next_history_page(HistoryRead&) const; // State owner; at most 200 metadata rows.
  data::v1::HistoryUsage history_usage(const std::string& dataset_id) const;
  static data::v1::HistoryUsage inspect_history_usage(const std::filesystem::path& directory,
                                                      Identity identity,
                                                      const std::string& dataset_id);
  task::v1::TaskLaunches dispatch(const task::v1::TaskDispatch& processes,
                                  bool data_available = true) const;
  [[nodiscard]] Change claim(const std::string& id);
  [[nodiscard]] Change claim(const InputRead&);
  [[nodiscard]] Change progress(const std::string& id, const std::string& token,
                                unsigned completed);
  // State operations return metadata; use get() when the input is required.
  [[nodiscard]] Change cancel(const std::string& id);
  [[nodiscard]] Change retry(const std::string& id);
  [[nodiscard]] Change finish(const std::string& id, const std::string& token,
                              const backtest::v1::BacktestResult& result);
  [[nodiscard]] Change finish(const std::string& id, const std::string& token,
                              const factor::v1::FactorResult& result);
  [[nodiscard]] Change fail(const std::string& id, const std::string& token,
                            const std::string& error);
  [[nodiscard]] Change interrupt(const std::string& id, const std::string& token,
                                 const std::string& error);
  [[nodiscard]] Change acknowledge_cancel(const std::string& id, const std::string& token);
  backtest::v1::BacktestResult result(const std::string& id) const;

  factor::v1::FactorResult factor_result(const std::string& id) const;

private:
  Store(std::filesystem::path, Identity, std::shared_ptr<const Clock>, bool read_only);
  task::v1::Task submit_prepared(Submission);
  void validate_input(const InputRead&) const;
  std::unique_ptr<Impl> impl_;
};
} // namespace asterion::tasks
