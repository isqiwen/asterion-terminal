#pragma once
#include "task_domain.hpp"
#include <asterion/base/time.hpp>
#include <asterion/protocol/task.hpp>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>
#include <set>
#include <string_view>
#include <functional>
#include <stop_token>

namespace asterion::sqlite {

class Database;

} // namespace asterion::sqlite

namespace asterion::tasks {

struct Identity
{
    std::string instance;
};

class Kinds;

// Serialized durable task state owned by one service. Attempt tokens fence late
// execution reports after cancellation, retry or service restart. The store is
// a mechanism: what each task kind means comes from the service's TaskDomain.
class Store
{
    struct Impl;

public:
    Store(
        const TaskDomain& domain,
        std::filesystem::path directory,
        Identity identity,
        std::shared_ptr<const Clock> clock = std::make_shared<SystemClock>());
    ~Store();

    struct BackupCatalogue
    {
        std::vector<std::filesystem::path> files;
        std::vector<std::filesystem::path> directories;
    };

    using BackupVisitor = std::function<
        void(const task::proto::StoredTaskRecord&, const task::proto::TaskExecution&)>;
    // The caller retains its domain cold lock after this read-only inspection.
    static BackupCatalogue inspect_backup(
        const TaskDomain&,
        const std::filesystem::path&,
        Identity,
        const BackupVisitor&,
        const BackupVisitor&,
        const std::function<
            void(const task::proto::PreparedTaskSubmission&, const resource::proto::Object&)>&,
        std::stop_token = {});

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

    task::proto::TaskRetirement retirement(const std::string& id, const std::string& node_id) const;
    [[nodiscard]] Change retire(const resource::proto::Object&);
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
        // File-worker projection of the exact prepared calculation input.
        task::proto::TaskExecution execution() const;

        const task::proto::Task& task() const
        {
            return task_;
        }

        void set_derivation(task::proto::TaskDerivation);
        void set_reuse_digest(std::string);
        // The owning service completes its admitted identity before admission.
        // The task id, kind and definition stay as submitted.
        void amend(const std::function<void(task::proto::Task&)>&);

    private:
        friend class Store;
        Submission(Impl*, std::filesystem::path, task::proto::Task);
        Impl* owner_;
        std::filesystem::path root_;
        task::proto::Task task_;
        std::optional<task::proto::StoredTaskRecord> existing_;
        std::optional<task::proto::StoredTaskRecord> reusable_;
        std::string digest_, reuse_digest_;
        std::uint64_t bytes_ = 0;
        bool admitted_ = false, created_ = false, prepared_ = false, resumed_ = false;
    };

    // File worker only. Explicitly resume an owned current preparation; never adopt an unknown directory.
    task::proto::PreparedTaskSubmission prepared_submission_record(const std::string& id) const;
    Submission resume_submission(const std::string& id) const;
    // A durable legal decision for an unadmitted preparation; accepted tasks cannot use it.
    Change
    abandon_preparation(const task::proto::PreparedTaskSubmission&, const resource::proto::Object&);
    std::optional<resource::proto::Object> abandoned_preparation(const std::string& id) const;
    task::proto::PreparedTaskSubmission abandoned_preparation_record(const std::string& id) const;
    // Owning services construct their typed submissions (see research_submission.hpp and
    // data_submission.hpp) on a file worker; definition validation may be expensive.
    Submission submission(task::proto::Task) const;
    const std::string& instance() const;
    [[nodiscard]] Change edit_metadata(const task::proto::TaskMetadataEdit&);
    void admit_submission(Submission&);
    [[nodiscard]] Change register_submission(Submission&);
    // Release an unconfirmed reservation, retaining any created directory.
    // True means file preparation failed after creating evidence and recovery is required.
    bool abandon_submission(Submission&);
    // A fully durable preparation can stop before registration without a failed write.
    // The owner immediately constructs its existing abandon_preparation decision,
    // without awaiting other work. Files and the legal release proof are retained.
    void release_prepared_reservation(Submission&);
    bool has_submission() const;

    // Immutable input I/O runs without the task-state lock. Confirmation returns
    // current metadata so a concurrent cancellation is never overwritten.
    class InputRead
    {
    public:
        void load();
        void load_for_claim();

        // After load_for_claim(), source metadata can be resolved before a durable claim.
        const task::proto::TaskExecution& execution() const
        {
            return execution_;
        }

    private:
        friend class Store;
        InputRead(
            std::shared_ptr<const Kinds>,
            task::proto::Task,
            std::filesystem::path,
            std::string,
            std::uint64_t);
        std::shared_ptr<const Kinds> kinds_;
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

    // Capture metadata under the host lock, read owned input/result files outside
    // it, then confirm identity under the lock. Results were validated when the
    // task finished; reads only parse them and never execute an algorithm.
    class ResultRead
    {
    public:
        void verify();

        const std::string& input_digest() const
        {
            return input_digest_;
        }

        std::uint64_t input_bytes() const
        {
            return input_bytes_;
        }

    private:
        friend class Store;
        ResultRead(
            std::shared_ptr<const Kinds>,
            task::proto::Task,
            std::filesystem::path,
            std::string,
            std::uint64_t,
            bool include_input);
        std::shared_ptr<const Kinds> kinds_;
        task::proto::Task task_;
        std::filesystem::path directory_;
        std::string input_digest_;
        std::uint64_t input_bytes_;
        bool include_input_, verified_ = false;
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

        // The fixed input, loaded by prepare_payload(), and the reported result.
        const task::proto::Task& task() const
        {
            return task_;
        }

        const task::proto::TaskFinish& result() const
        {
            return result_;
        }

    private:
        friend class Store;
        Completion(
            std::shared_ptr<const Kinds>,
            Identity identity,
            task::proto::Task task,
            task::proto::TaskFinish result,
            std::filesystem::path directory,
            std::string input_digest,
            std::uint64_t input_bytes);
        std::shared_ptr<const Kinds> kinds_;
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
    // For publishing kinds. Cancellation and publication are ordered by the same
    // durable task writer; a confirmed PUBLISHING change transfers custody to the
    // service's publication step, which has verified the candidate.
    [[nodiscard]] Change prepare_publication(
        Completion,
        const data::proto::DownloadIdentity&,
        std::string candidate_digest,
        std::string dataset_id);
    std::vector<data::proto::DownloadPublication> pending_publications() const;
    [[nodiscard]] Change confirm_publication(const data::proto::DownloadPublication&);
    // Full immutable input is loaded and integrity-checked only for explicit reads.
    task::proto::Task get(const std::string& id) const;
    // describe() and other lookups throw Error(not_found) for an unknown id.
    task::proto::Task describe(const std::string& id) const;
    std::optional<task::proto::Task> find_task(const std::string& id) const;
    task::proto::TaskList list(unsigned limit = 200, unsigned before_sequence = 0) const;
    // Every record after `after_id` in id order, until the visitor returns false.
    void visit(
        const std::string& after_id,
        const std::function<bool(const task::proto::StoredTaskRecord&)>&) const;
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

        HistoryRead(
            std::shared_ptr<const Kinds> kinds,
            std::filesystem::path root,
            std::string dataset,
            std::uint32_t sequence)
            : kinds_(std::move(kinds)),
              root_(std::move(root)),
              through_(sequence)
        {
            result_.set_dataset_id(std::move(dataset));
        }

        std::shared_ptr<const Kinds> kinds_;
        std::filesystem::path root_;
        std::uint32_t through_, after_ = 0;
        bool active_only_ = false;
        std::vector<task::proto::StoredTaskRecord> page_;
        data::proto::HistoryUsage result_;
    };

    HistoryRead prepare_history_usage(const std::string&) const;
    bool next_history_page(HistoryRead&) const; // State owner; at most 200 metadata rows.
    data::proto::HistoryUsage
    history_usage(const std::string& dataset_id, bool active_only = false) const;
    static data::proto::HistoryUsage inspect_history_usage(
        const TaskDomain&,
        const std::filesystem::path& directory,
        Identity identity,
        const std::string& dataset_id);
    [[nodiscard]] Change fail_start(
        const std::string& id,
        unsigned attempt,
        const std::string& error,
        const std::string& code);
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

    // The owning service's own metadata shares this connection and its serialized
    // journal. Never expose it through a business RPC.
    sqlite::Database& database(std::string_view service, bool writer);

private:
    Store(
        const TaskDomain&,
        std::filesystem::path,
        Identity,
        std::shared_ptr<const Clock>,
        bool read_only,
        bool bounded_inspection = true,
        bool cold_inspection = false,
        std::stop_token = {});
    void validate_input(const InputRead&) const;
    std::unique_ptr<Impl> impl_;
};

} // namespace asterion::tasks
