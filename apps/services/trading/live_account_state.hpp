#pragma once
#include <asterion/trading.pb.h>
#include "ctp_trader.hpp"
#include "account_policy.hpp"
#include "account_journal.hpp"
#include "account_command.hpp"
#include "account_records.hpp"
#include "alert_webhook.hpp"
#include "strategy_execution.hpp"
#include "strategy_orders.hpp"
#include <asterion/base/time.hpp>
#include <asterion/futures/live_market.hpp>
#include <asterion/system/progress.hpp>
#include <asterion/system/process/file_lock.hpp>
#include <map>
#include <optional>
#include <tuple>
#include <unordered_set>

namespace asterion::trading {

// Private state, constructed, accessed and destroyed only by the account loop.
// One live CTP account session in a dedicated directory. The broker owns the
// ledger; the session owns the execution chain in front of it: the owner's
// authorization, the contract allowlist and exchange units, the pinned
// pre-trade risk policy, and a durable record of every order before it is
// sent. Orders are never resent: after a crash or disconnect the recorded
// broker keys only attribute broker reports. The authorization outlives a
// connection; what was recorded for sending never does.
class LiveAccountState
{
public:
    struct DeferredQuote
    {
        std::future<std::optional<BrokerQuote>> quote;
        std::function<AccountCommand(std::future<std::optional<BrokerQuote>>)> resume;
    };

    // create_manifest absent means recover an existing session without rewriting it.
    // The service supplies the shared OS-user ownership directory; tests supply an isolated one.
    LiveAccountState(
        std::filesystem::path directory,
        const std::filesystem::path& ctp_library,
        const std::filesystem::path& ownership_directory,
        const Json& create_manifest,
        AccountJournal::Post post,
        std::function<void()> broker_ready,
        BrokerSendGate& send_gate,
        Progress& persistence,
        std::shared_ptr<AlertWebhook> alerts = nullptr,
        std::shared_ptr<const Clock> clock = default_clock());
    ~LiveAccountState();
    // Credentials live in the trader's memory for this connection only.
    void connect(BrokerLogin login);
    void disconnect();
    // Asks the broker for the account's rates on every allowed contract.
    void query_costs();
    void admit_revoke(std::string_view account_id, std::string_view policy_revision);
    AccountCommand execute(
        std::string account_id,
        std::string policy_revision,
        AccountRequest request,
        std::uint64_t admitted_control);
    ::trading::Session snapshot() const;
    ::trading::Orders orders(std::uint32_t offset, std::uint32_t limit) const;
    StrategyAuthorityPtr register_strategy(
        std::string instance,
        std::vector<InstrumentId> contracts,
        std::optional<StrategyWeightBudget> budget,
        std::vector<DominantSeriesSpec> series = {},
        StrategyExecutionOptions execution = {});
    void roll_strategy_series(
        const StrategyAuthorityPtr& authority,
        std::size_t series_index,
        const InstrumentId& next_contract);
    void release_strategy(const StrategyAuthorityPtr& authority);
    std::string strategy_failure(const StrategyAuthorityPtr& authority) const;
    // While paused, targets only reduce positions.
    void pause_strategy(const StrategyAuthorityPtr& authority, bool paused);
    // Pauses the instance and closes its contracts through the same execution path.
    void flatten_strategy(const StrategyAuthorityPtr& authority, std::vector<MarketQuote> quotes);
    // Execution events since the last call: orders sent and batches abandoned.
    std::vector<::trading::InstanceEvent> take_strategy_events(
        const StrategyAuthorityPtr& authority);
    void fail_strategy(std::string reason);
    // Drops the current target batch and cancels its orders once; the instance keeps running.
    void abandon_strategy_batch(std::string_view reason);
    void abandon_strategy_contract(const InstrumentId& contract, std::string_view reason);
    void request_strategy_cancels(std::optional<InstrumentId> only = std::nullopt);
    bool strategy_cancellation_pending() const;
    AccountCommand cancel_strategy_order();
    void require_manual_order() const;
    AccountCommand
    execute_strategy(StrategyOrderBasis basis, AccountRequest request, std::uint64_t control);
    void accept_strategy_target(
        const StrategyAuthorityPtr& authority,
        ::strategy::Decision decision,
        std::vector<MarketQuote> quotes,
        std::chrono::steady_clock::time_point expires_at);
    AccountCommand
    advance_strategy_target(StrategyAuthorityPtr authority, std::vector<MarketQuote> quotes);
    void observe_quotes(std::span<const MarketQuote> quotes);

    bool waiting_for_sdk() const
    {
        return sdk_wait_.has_value();
    }

    bool holds_intent(const std::string& order_id) const
    {
        return intents_.contains(order_id);
    }

    std::optional<DeferredQuote> take_deferred_quote()
    {
        return std::exchange(deferred_quote_, std::nullopt);
    }

    // What was recorded of a trading day, and every day with a record.
    ::trading::Day day(
        const std::string& trading_day,
        std::uint32_t offset,
        std::uint32_t limit,
        bool oldest_first) const;
    void poll_broker();
    bool storage_work_pending() const;
    AccountCommand advance_storage();
    std::chrono::steady_clock::time_point next_broker_deadline() const;
    void poll_sdk();

    bool recovery_required() const noexcept
    {
        return failed_;
    }

    bool business_ready() const;

    bool strategy_batch_active() const noexcept
    {
        return strategy_ && strategy_->batch.has_value();
    }

    bool strategy_events_pending() const noexcept
    {
        return strategy_ && !strategy_->events.empty();
    }

    bool strategy_has_failure() const noexcept
    {
        return strategy_ && !strategy_->failure.empty();
    }

private:
    template <class T>
    struct BrokerWait
    {
        bool await_ready() const
        {
            return result.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        }

        void await_suspend(std::coroutine_handle<> command)
        {
            account.sdk_wait_.emplace(
                SdkWait{
                    [this] {
                        return await_ready();
                    },
                    command
                });
        }

        T await_resume()
        {
            account.poll_broker();
            return result.get();
        }

        LiveAccountState& account;
        std::future<T> result;
    };

    struct SdkWait
    {
        std::function<bool()> ready;
        std::coroutine_handle<> command;
    };

    struct Intent
    {
        std::string broker_key, trading_day;
        InstrumentId instrument;
        Side side = Side::buy;
        Offset offset = Offset::open;
        Decimal quantity, limit_price;
        std::optional<BrokerOrder> terminal = std::nullopt;
        std::optional<StrategyOrderSource> strategy = std::nullopt;
        bool stop_cancel_pending = false;
        bool cancel_requested = false;
    };

    struct ObservedOrder
    {
        bool operator==(const ObservedOrder&) const = default;

        BrokerOrderStatus status;
        Decimal filled;
        std::string exchange_id, order_id;
    };

    struct TwapProgress
    {
        std::uint32_t slices_done = 0;
        bool slice_active = false;
        Decimal slice_remaining = {};
        std::chrono::steady_clock::time_point next_slice_at = {};
    };

    struct StrategyTargetBatch
    {
        ::strategy::Decision decision;
        std::vector<MarketQuote> quotes;
        std::vector<PositionTarget> targets;
        std::chrono::steady_clock::time_point accepted_at;
        std::set<InstrumentId> abandoned = {};
        std::map<InstrumentId, std::chrono::steady_clock::time_point> deadlines = {};
        std::map<InstrumentId, std::string> active_orders = {};
        bool flattening = false;
        std::map<InstrumentId, int> flatten_retries = {};
        std::map<InstrumentId, Decimal> active_order_prices = {};
        std::map<InstrumentId, Side> active_order_sides = {};
        std::map<InstrumentId, std::chrono::steady_clock::time_point> active_order_sent_at = {};
        std::map<InstrumentId, std::uint32_t> reprice_counts = {};
        std::set<InstrumentId> reprice_cancelling = {};
        std::map<InstrumentId, TwapProgress> twap = {};
    };

    struct ActiveStrategy
    {
        StrategyAuthorityPtr authority;
        std::uint64_t sequence = 0;
        std::uint32_t next_leg = 0;
        std::chrono::steady_clock::time_point deadline;
        std::string failure;
        std::uint64_t connection_generation = 0;
        std::optional<StrategyWeightBudget> budget = std::nullopt;
        std::optional<StrategyTargetBatch> batch = std::nullopt;
        // The last ready targets, in lots, or a flattening's zeros. Each later
        // ready decision without targets replans what is left of them.
        std::vector<PositionTarget> held = {};
        bool paused = false;
        std::vector<::trading::InstanceEvent> events = {};
        std::vector<InstrumentId> active_series = {};
        std::map<InstrumentId, Decimal> raw_weights = {};
        std::map<InstrumentId, Decimal> pending_weights = {};
        std::chrono::steady_clock::time_point weights_accepted_at = {};
        std::optional<Decimal> frozen_equity = std::nullopt;
        Decimal frozen_margin_scale = Decimal::parse("1");
        std::map<InstrumentId, Decimal> sized_prices = {};
        bool weights_validated = true;
        StrategyExecutionOptions execution = {};
    };

    struct OrderOrigin
    {
        std::string day;
        InstrumentId instrument;
    };

    struct CancelCount
    {
        std::uint32_t cancels = 0, rejected_cancels = 0;
    };

    struct RecordedTrade
    {
        std::string order_id;
        std::size_t index = 0;
    };

    struct Valuation
    {
        Decimal dynamic_equity;
        Decimal effective_peak;
        Decimal daily_loss;
        Decimal drawdown;
    };

    struct MarginDataWait
    {
        bool await_ready() const;
        void await_suspend(std::coroutine_handle<> command);

        void await_resume() const {}

        LiveAccountState& account;
        std::span<const std::pair<InstrumentId, std::string>> contracts;
    };

    template <class T>
    BrokerWait<T> wait_sdk(std::future<T> result)
    {
        return {*this, std::move(result)};
    }

    Decimal projected_margin(const BrokerSnapshot& state, const LimitOrder& order) const;
    AccountCommand query_margin_rates(const InstrumentId& instrument);
    static void check_terminal(const TerminalOrder& order, const Intent& intent);

    AccountCommand append(JournalEntry entry);
    AccountCommand revise_policy(const AccountRequest& request, const ChangePolicy& change);
    void log_broker_observations(const BrokerSnapshot& state) const noexcept;
    Instrument allowed(const InstrumentId& id) const;
    AccountCommand submit(
        const AccountRequest& request,
        const SubmitOrder& submission,
        std::uint64_t control,
        std::optional<StrategyOrderBasis> strategy = std::nullopt);
    AccountCommand finish_submit_quote(
        AccountRequest request,
        SubmitOrder submission,
        std::uint64_t control,
        std::future<std::optional<BrokerQuote>> ready_quote,
        std::optional<StrategyOrderBasis> strategy);
    AccountCommand finish_submit(
        const AccountRequest& request,
        const SubmitOrder& submission,
        std::uint64_t control,
        const std::optional<BrokerQuote>& quote,
        std::optional<StrategyOrderBasis> strategy = std::nullopt);
    void check_order_source(
        const SubmitOrder& submission,
        const std::optional<StrategyOrderBasis>& strategy) const;
    void check_strategy(const StrategyOrderBasis& basis) const;
    AccountCommand cancel(const AccountRequest& request, const std::string& order_id);

    void end_authorization();
    void check_account(std::string_view account_id) const;
    void check_price(
        const LimitOrder& order,
        const std::optional<BrokerQuote>& quote,
        const BrokerSnapshot& state) const;
    void
    check_closable_lots(const BrokerSnapshot& state, const LimitOrder& order, Offset offset) const;
    void check_pre_trade_risk(
        const BrokerSnapshot& state,
        const Instrument& terms,
        const LimitOrder& order,
        Offset offset,
        bool check_margin) const;
    std::vector<std::pair<std::string, const Intent*>> unconfirmed(
        const BrokerSnapshot& state) const;

    // A value computed from the broker's current state, read in place. The full
    // snapshot is copied only where a command needs one basis across a suspension.
    template <class F>
    auto observed(F read) const
    {
        std::invoke_result_t<F&, const BrokerSnapshot&> result{};
        trader_->observe([&](const BrokerSnapshot& state) {
            result = read(state);
        });
        return result;
    }

    void record_trade(::trading::Trade trade);
    Valuation valuation(const BrokerSnapshot& state) const;
    ::trading::DayClose close_with_valuation(const BrokerSnapshot& state) const;
    std::vector<std::string>
    active_risk_breaches(const BrokerSnapshot& state, const Valuation& val) const;
    void evaluate_risk_alerts(const BrokerSnapshot& state) const;
    void attach_cancels(::trading::DayClose& close, const BrokerSnapshot* state = nullptr) const;
    void save_latest_close(const ::trading::DayClose& close, bool force = false) const;
    // Broker fills not yet recorded, at most one batch.
    std::vector<::trading::Trade> unrecorded() const;
    // The previous trading day's close once the next one began, until recorded.
    std::optional<::trading::DayClose> unclosed() const;
    // Authorized, and connected to a synchronized broker: orders may be sent.
    bool authorized(const BrokerSnapshot& state) const;
    bool identities_ready(const BrokerSnapshot& state) const;

    std::optional<SdkWait> sdk_wait_;
    std::optional<DeferredQuote> deferred_quote_;

    struct Attribution
    {
        std::string day;
        std::uint64_t generation = 0, cursor = 0;
        bool complete = false;
    } attribution_;

    // Diagnostic observations are sampled by the service, never by SDK callbacks.
    // They do not replace the journal or broker execution history.
    mutable std::uint64_t observed_generation_ = 0, observed_sequence_ = 0;
    mutable std::size_t observed_trades_ = 0;
    mutable std::map<std::string, ObservedOrder> observed_orders_;

    std::optional<ActiveStrategy> strategy_;

    // Declared first: ownership outlives the SDK, risk policy and journal.
    std::unique_ptr<FileLock> account_owner_;
    // Delivers a completion to the account loop from another thread.
    AccountJournal::Post post_;
    std::unique_ptr<AccountJournal> journal_;
    SqliteJournal::Capacity capacity_{};
    std::filesystem::path directory_;
    std::unique_ptr<const AccountPolicy> policy_;
    BrokerSendGate& send_gate_;
    std::unique_ptr<ctp::Trader> trader_;
    ::trading::Environment environment_;
    std::string account_id_, broker_id_, investor_id_;
    // Journal record 0; the durable form of the account's creation.
    Json header_;
    // Order ID -> what was recorded before sending it.
    std::map<std::string, Intent> intents_;
    // Recorded fills by trading day, oldest first; rebuilt from the journal on start.
    std::map<std::string, std::vector<::trading::Trade>> trades_;
    std::map<std::string, std::map<std::pair<std::string, Side>, RecordedTrade>> recorded_ids_;
    // The leading broker fills known recorded. The broker rebuilds its list on
    // a new connection or trading day, which restarts the check.
    mutable std::size_t recorded_through_ = 0;
    mutable std::pair<std::uint64_t, std::string> recorded_list_;
    // Recorded closes by trading day; rebuilt from the journal on start.
    std::map<std::string, ::trading::DayClose> closes_;
    // The current trading day's funds and positions as last synchronized.
    mutable ::trading::DayClose latest_;
    mutable std::string latest_close_bytes_;
    std::map<std::string, OrderOrigin> order_origins_;
    std::map<std::string, std::string> cancel_requests_;
    std::set<std::string> cancel_awaited_;
    std::map<std::string, std::map<InstrumentId, CancelCount>> cancels_;
    // Lasts across connections and trading days; never restored.
    std::optional<Authorization> authorization_;
    bool failed_ = false;
    bool was_ready_ = false;
    std::shared_ptr<AlertWebhook> alerts_;
    std::map<std::string, std::pair<std::uint64_t, std::chrono::steady_clock::time_point>>
        recent_dispatches_;
    mutable std::map<InstrumentId, std::pair<std::string, Decimal>> market_marks_;
    mutable std::string peak_day_;
    mutable Decimal pre_cashflow_peak_;
    mutable Decimal last_effective_peak_;
    mutable std::chrono::steady_clock::time_point latest_saved_at_;
    mutable std::string latest_saved_day_;
    std::shared_ptr<const Clock> clock_;
    mutable std::uint64_t close_state_revision_ = 0;
    mutable std::optional<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>>
        last_close_basis_;
    mutable bool latest_dirty_ = false;
    mutable std::optional<std::pair<std::uint64_t, std::uint64_t>> unrecorded_clean_basis_;
};

} // namespace asterion::trading
