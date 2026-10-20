#pragma once
#include <asterion/foundation/time.hpp>
#include <asterion/protocol/task.hpp>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>
#include <string_view>

namespace asterion::tasks {

struct Identity
{
    std::string instance, data_instance;
};

// Serialized application-level durable task state. Agent owns OS processes;
// attempt tokens fence late worker reports after retry or service restart.
class Store
{
    struct Impl;

public:
    explicit Store(
        std::filesystem::path directory,
        Identity identity,
        std::shared_ptr<const Clock> clock = std::make_shared<SystemClock>());
    ~Store();

    // State decisions and their persistence have separate owners. The host keeps
    // at most one unconfirmed change per task and serializes database access.
    class Change
    {
    public:
        Change(Change&&) noexcept;
        Change& operator=(Change&&) noexcept;
        ~Change();
        bool needs_write() const;
        const task::proto::Task& task() const;
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
    // Persists and confirms on the calling thread. The running host uses this
    // only for a change that needs no write; otherwise it persists on its journal
    // thread and confirms on its state owner.
    Change commit(Change);

    class Submission
    {
    public:
        Submission(Submission&&) noexcept = default;
        Submission& operator=(Submission&&) noexcept = default;
        Submission(const Submission&) = delete;
        // File worker only: owns its definition and an optional existing-input snapshot.
        void prepare_files();
        void set_derivation(task::proto::TaskDerivation);

    private:
        friend class Store;
        Submission(Impl*, std::filesystem::path, task::proto::Task);
        Impl* owner_;
        std::filesystem::path root_;
        task::proto::Task task_;
        std::optional<task::proto::StoredTaskRecord> existing_;
        std::string digest_;
        std::uint64_t bytes_ = 0;
        bool admitted_ = false, created_ = false, prepared_ = false;
    };

    // Definition validation may be expensive; construct submissions on the file worker.
    Submission submission(const std::string&, backtest::proto::BacktestInput) const;
    Submission submission(const std::string&, factor::proto::FactorInput) const;
    Submission submission(const data::proto::DownloadAuthorization&) const;
    [[nodiscard]] Change edit_metadata(const task::proto::TaskMetadataEdit&);
    void admit_submission(Submission&);
    [[nodiscard]] Change register_submission(Submission&);
    // Release an unconfirmed reservation, retaining any created directory.
    // True means file preparation failed after creating evidence and recovery is required.
    bool abandon_submission(Submission&);
    bool has_submission() const;

    // Immutable input I/O runs without the task-state lock. Confirmation returns
    // current metadata so a concurrent cancellation is never overwritten.
    class InputRead
    {
    public:
        void load();
        void load_for_claim();

    private:
        friend class Store;
        InputRead(task::proto::Task, std::filesystem::path, std::string, std::uint64_t);
        task::proto::Task task_;
        std::filesystem::path path_;
        std::string digest_;
        std::uint64_t bytes_;
        bool loaded_ = false;
        task::proto::TaskExecution execution_;
        // Why the fixed input cannot execute; empty when valid.
        std::string refusal_;
    };

    InputRead prepare_input(const std::string&) const;
    task::proto::Task confirm_input(InputRead) const;
    task::proto::TaskAttempt confirm_attempt(InputRead) const;

    // Capture metadata under the host lock, verify owned input/result files outside
    // it, then confirm identity under the lock. Reads never execute an algorithm.
    class ResultRead
    {
    public:
        void verify();

    private:
        friend class Store;
        ResultRead(
            Identity,
            task::proto::Task,
            std::filesystem::path,
            std::string,
            std::uint64_t,
            bool verify_content,
            bool include_input);
        task::proto::Task task_;
        std::filesystem::path directory_;
        std::string input_digest_;
        std::uint64_t input_bytes_;
        Identity identity_;
        bool verify_content_, include_input_, verified_ = false;
        task::proto::TaskResponse response_;
    };

    ResultRead prepare_result(const std::string&) const;
    task::proto::TaskResponse confirm_result(ResultRead) const;

    // Owns an immutable attempt/input/result snapshot, never a reference to Store.
    // prepare_finish/finish require host serialization. prepare_payload verifies
    // and durably writes the attempt file outside it; only finish decides success.
    class Completion
    {
    public:
        void prepare_payload();
        data::proto::DownloadPreparation download_preparation() const;

    private:
        friend class Store;
        Completion(
            Identity identity,
            task::proto::Task task,
            task::proto::TaskFinish result,
            std::filesystem::path directory,
            std::string input_digest,
            std::uint64_t input_bytes);
        task::proto::Task task_;
        std::filesystem::path directory_;
        std::string input_digest_;
        std::uint64_t input_bytes_;
        task::proto::TaskFinish result_;
        Identity identity_;
        std::string result_digest_;
        bool prepared_ = false;
    };

    Completion prepare_finish(const task::proto::TaskFinish& result);
    [[nodiscard]] Change finish(Completion completion);
    // Cancellation and publication are ordered by the same durable task writer.
    // A confirmed PUBLISHING change transfers custody; the worker lease no longer applies.
    [[nodiscard]] Change prepare_publication(Completion, const data::proto::PreparedDownload&);
    std::vector<data::proto::DownloadPublication> pending_publications() const;
    [[nodiscard]] Change confirm_publication(const data::proto::PublishedDownload&);
    // Full immutable input is loaded and integrity-checked only for explicit reads.
    task::proto::Task get(const std::string& id) const;
    task::proto::Task describe(const std::string& id) const;
    task::proto::TaskList list(unsigned limit = 200, unsigned before_sequence = 0) const;
    std::vector<task::proto::Task> active_tasks() const;

    class HistoryRead
    {
    public:
        void load_page(); // File worker; consumes only the captured metadata page.

        data::proto::HistoryUsage take()
        {
            return std::move(result_);
        }

    private:
        friend class Store;

        HistoryRead(std::filesystem::path root, std::string dataset, std::uint32_t sequence)
            : root_(std::move(root)),
              through_(sequence)
        {
            result_.set_dataset_id(std::move(dataset));
        }

        std::filesystem::path root_;
        std::uint32_t through_, after_ = 0;
        std::vector<task::proto::StoredTaskRecord> page_;
        data::proto::HistoryUsage result_;
    };

    HistoryRead prepare_history_usage(const std::string&) const;
    bool next_history_page(HistoryRead&) const; // State owner; at most 200 metadata rows.
    data::proto::HistoryUsage history_usage(const std::string& dataset_id) const;
    static data::proto::HistoryUsage inspect_history_usage(
        const std::filesystem::path& directory,
        Identity identity,
        const std::string& dataset_id);
    task::proto::TaskLaunches
    dispatch(const task::proto::TaskDispatch& processes, bool data_available = true) const;
    [[nodiscard]] Change claim(const std::string& id);
    [[nodiscard]] Change claim(const InputRead&);
    [[nodiscard]] Change
    progress(const std::string& id, const std::string& token, unsigned completed);
    // State operations return metadata; use get() when the input is required.
    [[nodiscard]] Change cancel(const std::string& id);
    [[nodiscard]] Change retry(const std::string& id);
    [[nodiscard]] Change fail(
        const std::string& id,
        const std::string& token,
        const std::string& error,
        const std::string& error_code = {});
    [[nodiscard]] Change
    interrupt(const std::string& id, const std::string& token, const std::string& error);
    [[nodiscard]] Change acknowledge_cancel(const std::string& id, const std::string& token);

private:
    Store(std::filesystem::path, Identity, std::shared_ptr<const Clock>, bool read_only);
    void validate_input(const InputRead&) const;
    std::unique_ptr<Impl> impl_;
};

} // namespace asterion::tasks
