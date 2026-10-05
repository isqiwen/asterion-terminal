#pragma once
#include "sqlite_journal.hpp"
#include "account_policy.hpp"
#include "ctp_order_identity.hpp"
#include <asterion/foundation/bounded_queue.hpp>
#include <coroutine>
#include <future>
#include <thread>
#include <asterion/kernel/progress.hpp>
namespace asterion::trading {
// Runtime storage and policy preparation belong to this serial writer. Completions are
// delivered to the account loop; it alone resumes and adopts command results.
class AccountJournal {
public:
  using Post = std::function<void(std::function<void()>)>;
  struct Result {
    SqliteJournal::Capacity capacity{};
    Json record;
    std::unique_ptr<const AccountPolicy> policy;
    ctp::KnownOrders orders{};
    std::uint64_t order_cursor = 0;
  };
  AccountJournal(std::filesystem::path directory, std::function<void(const Json&)> validate,
                 Post post, Progress& progress);
  ~AccountJournal();
  // Initialization only: storage reads bounded pages; the caller applies them on
  // the unpublished account owner, never on the writer thread.
  void replay(const std::function<void(std::uint64_t, const Json&)>& apply);
  SqliteJournal::Capacity initial_capacity() const { return initial_capacity_; }
  // Initial creation is not published to clients until its header is durable.
  SqliteJournal::Capacity initialize(Json header);
  struct Write {
    AccountJournal& owner;
    std::function<Result(SqliteJournal&)> operation;
    Result result;
    std::exception_ptr error;
    std::unique_ptr<const AccountPolicy> retiring;
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> command);
    Result await_resume();
  };
  Write append(Json record);
  Write find_command(std::string id);
  Write find_order(std::string id);
  Write restore_orders(std::string day, std::uint64_t after);
  Write prepare_policy(std::filesystem::path plugins, Json definition, std::string revision,
                       std::string requested_artifact, std::string current_artifact);
  Write retire_policy(std::unique_ptr<const AccountPolicy> policy);

private:
  using Job = std::function<void(SqliteJournal&)>;
  Progress& progress_;
  Post post_;
  BoundedQueue<Job> jobs_{1};
  SqliteJournal::Capacity initial_capacity_{};
  std::jthread thread_;
};
} // namespace asterion::trading
