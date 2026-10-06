#include "live_session.hpp"
#include "live_account_state.hpp"
#include <asterion/kernel/trace.hpp>
#include <asterion/kernel/logger.hpp>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
namespace asterion::trading {
struct LiveSession::Loop {
  enum class Admission { command, control, stop_sends, observation, completion };
  using Event = std::function<void(LiveAccountState&)>;
  struct Mutation {
    std::function<AccountCommand(LiveAccountState&, std::uint64_t)> begin;
    std::function<void(LiveAccountState&)> admit;
    std::promise<void> result;
    std::string trace;
    std::uint64_t control = 0;
    // The order a cancel names; empty for every other command.
    std::string request_id, cancel_order;
  };
  struct Active {
    std::shared_ptr<Mutation> request;
    AccountCommand command;
  };
  // One gate outlives the account, SDK and every accepted command.
  BrokerSendGate send_gate;
  std::mutex mutex;
  std::condition_variable wake;
  std::deque<Event> events;
  // Admission covers queued and active requests, including deferred mutations.
  std::size_t admitted = 0;
  bool accepting = true, stopping = false, broker_pending = false;
  std::deque<std::shared_ptr<Mutation>> mutations;
  std::optional<Active> active;
  // A cancel running in front of the active command while that command is
  // parked on its broker quote. The account still advances one durable command
  // at a time: the parked command has recorded nothing and resumes afterwards.
  std::optional<Active> interposed;
  std::atomic<bool> failed{false};
  Progress state_progress, persistence_progress, command_progress;
  std::atomic<bool> business_ready{false};
  std::shared_future<void> initialized, stopped;
  std::jthread thread;
  void enqueue(Event event, Admission admission = Admission::command,
               Mutation* mutation = nullptr) {
    const bool completion = admission == Admission::completion;
    if (!completion) {
      if (initialized.wait_for(std::chrono::seconds{0}) != std::future_status::ready)
        throw Error(ErrorCode::unavailable, "account is initializing");
      initialized.get();
    }
    std::lock_guard lock(mutex);
    const auto limit = admission == Admission::command ? 64U : 72U;
    if (!completion && (stopping || (!accepting && admission != Admission::observation)))
      throw Error(ErrorCode::unavailable, "account is stopping");
    if (!completion && admitted >= limit)
      throw Error(ErrorCode::resource_exhausted, "account request queue is full");
    events.push_back(std::move(event));
    // Queue publication and permission invalidation have the same admission
    // boundary. The owner cannot take this event until its original basis is fixed.
    if (admission == Admission::stop_sends)
      send_gate.invalidate();
    if (mutation)
      mutation->control = send_gate.revision();
    if (!completion)
      ++admitted;
    // One active command produces at most one durable completion at a time.
    // Its reserved slot remains available when external admission is full.
    wake.notify_one();
  }
  void release_request() {
    std::lock_guard lock(mutex);
    --admitted;
  }
  void start(LiveAccountState& account, std::shared_ptr<Mutation> request) {
    TraceScope context(request->trace);
    command_progress.begin();
    try {
      active.emplace(Active{request, request->begin(account, request->control)});
      active->command.start();
    } catch (...) {
      request->result.set_exception(std::current_exception());
      active.reset();
      command_progress.finish();
      release_request();
    }
  }
  // A cancel of an order this account has already recorded depends on no
  // command queued before it.
  std::shared_ptr<Mutation> take_cancel(const LiveAccountState& account) {
    if (!active->request || !account.yields_to_cancel())
      return nullptr;
    for (auto it = mutations.begin(); it != mutations.end(); ++it) {
      const auto& request = **it;
      if (request.cancel_order.empty() || request.request_id == active->request->request_id ||
          !account.holds_intent(request.cancel_order))
        continue;
      auto found = std::move(*it);
      mutations.erase(it);
      return found;
    }
    return nullptr;
  }
  void interpose(LiveAccountState& account, std::shared_ptr<Mutation> request) {
    TraceScope context(request->trace);
    account.park();
    try {
      interposed.emplace(Active{request, request->begin(account, request->control)});
      interposed->command.start();
    } catch (...) {
      request->result.set_exception(std::current_exception());
      interposed.reset();
      account.unpark();
      release_request();
    }
  }
  void finish(LiveAccountState& account) {
    for (;;) {
      bool completed_storage = false;
      if (interposed) {
        if (!interposed->command.done())
          return;
        try {
          interposed->command.result();
          interposed->request->result.set_value();
        } catch (...) {
          interposed->request->result.set_exception(std::current_exception());
        }
        interposed.reset();
        account.unpark();
        release_request();
      }
      if (active) {
        if (!active->command.done()) {
          if (auto cancel = take_cancel(account)) {
            interpose(account, std::move(cancel));
            continue;
          }
          return;
        }
        failed = account.recovery_required();
        try {
          active->command.result();
          if (active->request)
            active->request->result.set_value();
        } catch (...) {
          if (active->request)
            active->request->result.set_exception(std::current_exception());
          else
            log_process_failure("trading", "journal.reconciliation_failed",
                                ErrorCode::operation_failed, 1);
        }
        completed_storage = !active->request;
        active.reset();
        command_progress.finish();
        if (!completed_storage)
          release_request();
      }
      // One bounded storage batch alternates with an already queued command.
      // It uses the same serial writer and reserved completion slot.
      if (account.storage_work_pending() && (!completed_storage || mutations.empty())) {
        command_progress.begin();
        active.emplace(Active{nullptr, account.advance_storage()});
        active->command.start();
        continue;
      }
      if (mutations.empty())
        return;
      auto next = std::move(mutations.front());
      mutations.pop_front();
      start(account, std::move(next));
    }
  }
  Loop(std::filesystem::path directory, std::filesystem::path sdk, std::filesystem::path ownership,
       Json manifest) {
    std::promise<void> started, finished;
    initialized = started.get_future().share();
    stopped = finished.get_future().share();
    thread = std::jthread([this, directory = std::move(directory), sdk = std::move(sdk),
                           ownership = std::move(ownership), manifest = std::move(manifest),
                           started = std::move(started), finished = std::move(finished),
                           trace = std::string(current_trace_id())]() mutable {
      state_progress.begin();
      std::unique_ptr<LiveAccountState> account;
      try {
        TraceScope context(trace);
        account = std::make_unique<LiveAccountState>(
            std::move(directory), sdk, ownership, manifest,
            [this](std::function<void()> completed) {
              enqueue([completed = std::move(completed)](LiveAccountState&) { completed(); },
                      Admission::completion);
            },
            [this] {
              std::lock_guard lock(mutex);
              broker_pending = true;
              wake.notify_one();
            },
            send_gate, persistence_progress);
      } catch (...) {
        started.set_exception(std::current_exception());
        finished.set_value();
        return;
      }
      started.set_value();
      for (;;) {
        Event event;
        {
          std::unique_lock lock(mutex);
          const auto ready = [&] {
            return broker_pending || !events.empty() || (stopping && !active);
          };
          const auto deadline =
              std::min(account->next_broker_deadline(),
                       std::chrono::steady_clock::now() + std::chrono::milliseconds{500});
          wake.wait_until(lock, deadline, ready);
          broker_pending = false;
          if (events.empty() && stopping && !active)
            break;
          if (!events.empty()) {
            event = std::move(events.front());
            events.pop_front();
          }
        }
        state_progress.begin();
        account->poll_broker();
        if (event)
          event(*account);
        if (account->waiting_for_sdk()) {
          TraceScope context((interposed ? interposed : active)->request->trace);
          account->poll_sdk();
        }
        finish(*account);
        failed = account->recovery_required();
        business_ready = account->business_ready();
        state_progress.finish();
      }
      business_ready = false;
      state_progress.begin();
      account.reset(); // Keep the owner alive while SDK callbacks and the writer stop.
      state_progress.finish();
      finished.set_value();
    });
  }
  void close_admission() {
    std::lock_guard lock(mutex);
    if (!accepting)
      return;
    accepting = false;
    send_gate.invalidate();
  }
  void stop() {
    close_admission();
    std::lock_guard lock(mutex);
    if (stopping)
      return;
    // Final stop follows reply drain. Completion delivery retains its reserved slot.
    events.push_front([](LiveAccountState& account) { account.disconnect(); });
    stopping = true;
    wake.notify_one();
  }
  ~Loop() {
    stop();
    thread.join();
  }
  template <class F> auto call(F operation, Admission admission = Admission::control) {
    using Result = std::invoke_result_t<F, LiveAccountState&>;
    auto task = std::make_shared<std::packaged_task<Result(LiveAccountState&)>>(
        [this, operation = std::move(operation),
         trace = std::string(current_trace_id())](LiveAccountState& account) mutable -> Result {
          TraceScope context(trace);
          struct PublishHealth {
            Loop& loop;
            LiveAccountState& account;
            ~PublishHealth() { loop.failed = account.recovery_required(); }
          } publish{*this, account};
          return operation(account);
        });
    auto result = task->get_future();
    enqueue(
        [this, task = std::move(task)](LiveAccountState& account) {
          (*task)(account);
          release_request();
        },
        admission);
    return result;
  }
  std::future<void> mutate(std::function<AccountCommand(LiveAccountState&, std::uint64_t)> begin,
                           std::function<void(LiveAccountState&)> admit = {},
                           Admission admission = Admission::command, std::string request_id = {},
                           std::string cancel_order = {}) {
    auto request = std::make_shared<Mutation>();
    request->begin = std::move(begin);
    request->admit = std::move(admit);
    request->trace = current_trace_id();
    request->request_id = std::move(request_id);
    request->cancel_order = std::move(cancel_order);
    auto result = request->result.get_future();
    enqueue(
        [this, request](LiveAccountState& account) {
          try {
            TraceScope context(request->trace);
            if (request->admit)
              request->admit(account);
          } catch (...) {
            request->result.set_exception(std::current_exception());
            release_request();
            return;
          }
          if (active)
            mutations.push_back(request);
          else
            start(account, request);
        },
        admission, request.get());
    return result;
  }
};
LiveSession::LiveSession(std::filesystem::path directory, const std::filesystem::path& ctp_library,
                         const std::filesystem::path& ownership_directory, const Json& manifest)
    : loop_(std::make_unique<Loop>(std::move(directory), ctp_library, ownership_directory,
                                   manifest)) {}
LiveSession::~LiveSession() = default;
std::shared_future<void> LiveSession::initialized() const {
  return loop_->initialized;
}
void LiveSession::close_admission() {
  loop_->close_admission();
}
void LiveSession::stop() {
  loop_->stop();
}
std::shared_future<void> LiveSession::stopped() const {
  return loop_->stopped;
}
std::future<void> LiveSession::connect(std::string password, std::string auth_code) {
  return loop_->mutate([password = std::move(password), auth_code = std::move(auth_code)](
                           LiveAccountState& account, std::uint64_t) mutable -> AccountCommand {
    account.connect(std::move(password), std::move(auth_code));
    co_return;
  });
}
std::future<void> LiveSession::disconnect() {
  return loop_->call([](LiveAccountState& account) { account.disconnect(); },
                     Loop::Admission::stop_sends);
}
std::future<void> LiveSession::query_costs() {
  return loop_->call([](LiveAccountState& account) { account.query_costs(); });
}
std::future<void> LiveSession::execute(std::string_view account_id,
                                       std::string_view policy_revision, const Json& command) {
  std::optional<AccountRequest> request;
  try {
    request = AccountRequest::parse(command);
  } catch (...) {
    std::promise<void> rejected;
    rejected.set_exception(std::current_exception());
    return rejected.get_future();
  }
  const bool revoke = request->as<Revoke>();
  const auto* cancel = request->as<CancelOrder>();
  const bool cancels = cancel != nullptr;
  auto id = request->id;
  auto cancel_order = cancels ? cancel->order_id : std::string{};
  auto begin = [record = std::string(account_id), policy = std::string(policy_revision),
                request = std::move(*request)](LiveAccountState& account, std::uint64_t control) {
    return account.execute(record, policy, request, control);
  };
  if (revoke)
    return loop_->mutate(
        std::move(begin),
        [record = std::string(account_id), policy = std::string(policy_revision)](
            LiveAccountState& account) { account.admit_revoke(record, policy); },
        Loop::Admission::stop_sends, std::move(id));
  return loop_->mutate(std::move(begin), {},
                       cancels ? Loop::Admission::control : Loop::Admission::command, std::move(id),
                       std::move(cancel_order));
}

std::future<Json> LiveSession::snapshot() const {
  return loop_->call([](LiveAccountState& account) { return account.snapshot(); },
                     Loop::Admission::observation);
}
bool LiveSession::recovery_required() const noexcept {
  return loop_->failed.load();
}
LiveSession::Health LiveSession::health() const noexcept {
  return {loop_->state_progress.observe(), loop_->persistence_progress.observe(),
          loop_->command_progress.observe(), loop_->business_ready.load()};
}

} // namespace asterion::trading
