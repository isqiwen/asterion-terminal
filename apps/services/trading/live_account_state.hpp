#pragma once
#include "ctp_trader.hpp"
#include "account_policy.hpp"
#include "account_journal.hpp"
#include "account_command.hpp"
#include "account_records.hpp"
#include "strategy_host.hpp"
#include <asterion/kernel/progress.hpp>
#include <asterion/kernel/process/file_lock.hpp>
#include <map>
#include <optional>
namespace asterion::trading {
// Private state, constructed, accessed and destroyed only by the account loop.
// One live CTP account session in a dedicated directory. The broker owns the
// ledger; the session owns the execution chain in front of it: the owner's
// authorization, the contract allowlist and exchange units, the pinned
// pre-trade risk plugin, and a durable record of every order before it is
// sent. Orders are never resent: after a crash or disconnect the recorded
// broker keys only attribute broker reports. The authorization and a strategy
// run outlive a connection; what was recorded for sending never does.
class LiveAccountState {
public:
  // create_manifest absent means recover an existing session without rewriting it.
  // The service supplies the shared OS-user ownership directory; tests supply an isolated one.
  LiveAccountState(std::filesystem::path directory, const std::filesystem::path& ctp_library,
                   const std::filesystem::path& ownership_directory, const Json& create_manifest,
                   AccountJournal::Post post, std::function<void()> broker_ready,
                   BrokerSendGate& send_gate, Progress& persistence);
  ~LiveAccountState();
  // Credentials live in the trader's memory for this connection only.
  void connect(std::string password, std::string auth_code);
  void disconnect();
  // Asks the broker for the account's rates on every allowed contract.
  void query_costs();
  void admit_revoke(std::string_view account_id, std::string_view policy_revision);
  AccountCommand execute(std::string account_id, std::string policy_revision,
                         AccountRequest request, std::uint64_t admitted_control);
  Json snapshot() const;
  bool waiting_for_sdk() const { return sdk_waits_.size() > parked_; }
  // The only command is parked on its broker quote and has recorded nothing
  // yet: a cancel may run to completion before that command resumes.
  bool yields_to_cancel() const {
    return !parked_ && sdk_waits_.size() == 1 && sdk_waits_.back().yields;
  }
  bool holds_intent(const std::string& order_id) const { return intents_.contains(order_id); }
  // The run owes a step: cancel requests for its orders, or the order that
  // moves the contract's position to the latest target.
  bool strategy_work_pending() const;
  AccountCommand advance_strategy(std::uint64_t control);
  std::string_view strategy_run() const { return run_ ? std::string_view(run_->id) : ""; }
  // Keeps the yielding command suspended, whatever its quote does, until unpark().
  void park() { parked_ = sdk_waits_.size(); }
  void unpark() { parked_ = 0; }
  void poll_broker();
  bool storage_work_pending() const;
  AccountCommand advance_storage();
  std::chrono::steady_clock::time_point next_broker_deadline() const;
  void poll_sdk();
  bool recovery_required() const noexcept { return failed_; }
  bool business_ready() const;

private:
  template <class T> struct BrokerWait {
    LiveAccountState& account;
    std::future<T> result;
    bool yields;
    bool await_ready() const {
      return result.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    }
    void await_suspend(std::coroutine_handle<> command) {
      account.sdk_waits_.push_back({[this] { return await_ready(); }, command, yields});
    }
    T await_resume() {
      account.poll_broker();
      return result.get();
    }
  };
  template <class T> BrokerWait<T> wait_sdk(std::future<T> result, bool yields = false) {
    return {*this, std::move(result), yields};
  }
  // Commands suspended on an SDK result, innermost last. There is one, or two
  // while a cancel runs in front of a command parked on its quote.
  struct SdkWait {
    std::function<bool()> ready;
    std::coroutine_handle<> command;
    bool yields;
  };
  std::vector<SdkWait> sdk_waits_;
  std::size_t parked_ = 0;
  struct Intent {
    std::string broker_key, trading_day;
    InstrumentId instrument;
    Offset offset = Offset::open;
    Decimal quantity;
    std::optional<BrokerOrder> terminal = std::nullopt;
  };
  static void check_terminal(const TerminalOrder& order, const Intent& intent);
  struct Attribution {
    std::string day;
    std::uint64_t generation = 0, cursor = 0;
    bool complete = false;
  } attribution_;
  AccountCommand append(JournalEntry entry);
  AccountCommand revise_policy(const AccountRequest& request, const ChangePolicy& change);
  void log_broker_observations(const BrokerSnapshot& state) const noexcept;
  struct ObservedOrder {
    BrokerOrderStatus status;
    Decimal filled;
    std::string exchange_id, order_id;
    bool operator==(const ObservedOrder&) const = default;
  };
  // Diagnostic observations are sampled by the service, never by SDK callbacks.
  // They do not replace the journal or broker execution history.
  mutable std::uint64_t observed_generation_ = 0, observed_sequence_ = 0;
  mutable std::size_t observed_trades_ = 0;
  mutable std::map<std::string, ObservedOrder> observed_orders_;
  const Instrument& allowed(const InstrumentId& id) const;
  AccountCommand submit(const AccountRequest& request, const SubmitOrder& submission,
                        std::uint64_t control);
  AccountCommand cancel(const AccountRequest& request, const std::string& order_id);
  // One strategy run: the only originator of new orders while it runs. It
  // lives inside the authorization, continues across connections and trading
  // days, and is never restored. An ended run stays for display until the
  // next one starts.
  struct Run {
    std::string id;
    Instrument instrument;
    std::uint32_t fast = 0, slow = 0;
    Decimal quantity;
    std::int64_t started_ms = 0;
    std::unique_ptr<StrategyHost> host;
    std::uint64_t bars = 0;
    std::optional<StrategyHost::Bar> bar;
    // The bar whose target already produced its orders.
    std::int64_t placed_bar = 0;
    // Recorded orders of this run that are not retired, and whether a cancel
    // was requested for each. Pruned only between steps.
    std::map<std::string, bool> orders;
    // Empty while running.
    std::string ended;
    bool cancel_due = false;
  };
  std::optional<Run> run_;
  bool running() const { return run_ && run_->ended.empty(); }
  AccountCommand start_strategy(const AccountRequest& request, const StartStrategy& start,
                                std::uint64_t control);
  void end_run(std::string reason, bool cancel);
  void end_authorization();
  void report(const std::string& run,
              const std::variant<StrategyHost::Bar, StrategyHost::Failure>& event);
  AccountCommand place_target(std::uint64_t control);
  AccountCommand cancel_run_orders();
  void check_account(std::string_view account_id) const;
  void check_price(const LimitOrder& order, const std::optional<BrokerQuote>& quote,
                   const BrokerSnapshot& state) const;
  std::vector<std::pair<std::string, const Intent*>> unconfirmed(const BrokerSnapshot& state) const;
  // A value computed from the broker's current state, read in place. The full
  // snapshot is copied only where a command needs one basis across a suspension.
  template <class F> auto observed(F read) const {
    std::invoke_result_t<F&, const BrokerSnapshot&> result{};
    trader_->observe([&](const BrokerSnapshot& state) { result = read(state); });
    return result;
  }
  // Declared first: ownership outlives the SDK, risk plugin and journal.
  std::unique_ptr<FileLock> account_owner_;
  // Delivers a completion to the account loop from another thread.
  AccountJournal::Post post_;
  std::unique_ptr<AccountJournal> journal_;
  SqliteJournal::Capacity capacity_{};
  std::filesystem::path directory_;
  std::unique_ptr<const AccountPolicy> policy_;
  BrokerSendGate& send_gate_;
  std::unique_ptr<ctp::Trader> trader_;
  std::string account_id_, trade_front_, app_id_, broker_id_, investor_id_;
  // Journal record 0; the durable form of the account's creation.
  Json header_;
  // Order ID -> what was recorded before sending it.
  std::map<std::string, Intent> intents_;
  // Lasts across connections and trading days; never restored.
  std::optional<Authorization> authorization_;
  // Authorized, and connected to a synchronized broker: orders may be sent.
  bool authorized(const BrokerSnapshot& state) const;
  bool identities_ready(const BrokerSnapshot& state) const;
  bool failed_ = false;
};
} // namespace asterion::trading
