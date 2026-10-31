#pragma once
#include <set>
#include <asterion/database/sqlite/sqlite_journal.hpp>
#include "account_policy.hpp"
#include "account_records.hpp"
#include "ctp_order_identity.hpp"
#include <asterion/foundation/bounded_queue.hpp>
#include <coroutine>
#include <future>
#include <thread>
#include <asterion/kernel/progress.hpp>

namespace asterion::trading {

// Runtime storage belongs to this serial writer. Completions are
// delivered to the account loop; it alone resumes and adopts command results.
class AccountJournal
{
public:
    using Post = std::function<void(std::function<void()>)>;
    // Physical database/file publication ownership; distinct from account execution binding.
    static constexpr auto ownership_file = "account.lock";
    // Directories beside the journal files: the CTP flow cache,
    // sealed segments and owner/, which holds ownership_file alone.
    static inline const std::set<std::string> sidecars{"ctp-flow", "archives", "owner"};

    struct Result
    {
        SqliteJournal::Capacity capacity{};
        // find_command: the record that first used the request ID.
        std::optional<CommandRecord> command;
        // find_order: the order ID belongs to a recorded submission.
        bool order_known = false;
        ctp::KnownOrders orders{};
        std::uint64_t order_cursor = 0;
    };

    AccountJournal(
        std::filesystem::path directory,
        std::function<void(const Json&)> validate,
        Post post,
        Progress& progress);
    ~AccountJournal();
    // Caller owns the stopped physical directory lease. This shares replay's
    // typed/index verification without creating a writer, account or SDK.
    static std::vector<std::filesystem::path> inspect_backup(
        const std::filesystem::path&,
        const std::function<void(const Json&)>& header,
        const std::function<void(const JournalRecord&)>& apply,
        std::stop_token = {});
    // Initialization only: storage reads bounded pages; the caller applies them on
    // the unpublished account owner, never on the writer thread.
    void replay(
        const std::function<void(const Json&)>& header,
        const std::function<void(const JournalRecord&)>& apply);

    SqliteJournal::Capacity initial_capacity() const
    {
        return initial_capacity_;
    }

    // Initial creation is not published to clients until its header is durable.
    SqliteJournal::Capacity initialize(Json header);

    struct Write
    {
        AccountJournal& owner;
        std::function<Result(SqliteJournal&)> operation;
        Result result;
        std::exception_ptr error;

        bool await_ready() const noexcept
        {
            return false;
        }

        void await_suspend(std::coroutine_handle<> command);
        Result await_resume();
    };

    Write append(JournalEntry entry);
    Write find_command(std::string id);
    Write find_order(std::string id);
    Write restore_orders(std::string day, std::uint64_t after);

private:
    using Job = std::function<void(SqliteJournal&)>;
    std::filesystem::path directory_;
    Progress& progress_;
    Post post_;
    BoundedQueue<Job> jobs_{1};
    SqliteJournal::Capacity initial_capacity_{};
    std::jthread thread_;
};

} // namespace asterion::trading
