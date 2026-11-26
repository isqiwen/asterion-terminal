#include "live_session.hpp"
#include "live_account_state.hpp"
#include <asterion/system/trace.hpp>
#include <asterion/system/log.hpp>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>

namespace asterion::trading {
namespace {

constexpr std::size_t max_bounded_commands = 64;

struct PostOptions
{
    bool bounded = false;
    bool allow_when_closing = false;
    bool invalidate_sends = false;
};

} // namespace

struct LiveSession::Loop
{
    using Event = std::function<void(LiveAccountState&)>;

    struct Mutation
    {
        std::function<AccountCommand(LiveAccountState&, std::uint64_t)> begin;
        std::function<void(LiveAccountState&)> admit;
        std::promise<void> result;
        std::string trace;
        std::uint64_t control = 0;
        // The order a cancel names; empty for every other command.
        std::string request_id, cancel_order;
        bool bounded = false;
    };

    struct Active
    {
        std::shared_ptr<Mutation> request;
        AccountCommand command;
    };

    struct WaitingQuote
    {
        std::shared_ptr<Mutation> request;
        LiveAccountState::DeferredQuote deferred;
    };

    void post_completion(std::function<void()> completed)
    {
        std::lock_guard lock(mutex);
        events.push_back([completed = std::move(completed)](LiveAccountState&) {
            completed();
        });
        wake.notify_one();
    }

    void enqueue(Event event, PostOptions options = {}, Mutation* mutation = nullptr)
    {
        if (initialized.wait_for(std::chrono::seconds{0}) != std::future_status::ready)
            throw Error(ErrorCode::unavailable, "account is initializing");
        initialized.get();
        std::lock_guard lock(mutex);
        if (stopping || (!accepting && !options.allow_when_closing))
            throw Error(ErrorCode::unavailable, "account is stopping");
        if (options.bounded && bounded_commands >= max_bounded_commands)
            throw Error(ErrorCode::resource_exhausted, "account request queue is full");
        events.push_back(std::move(event));
        // Queue publication and permission invalidation share one admission boundary.
        if (options.invalidate_sends)
            send_gate.invalidate();
        if (mutation)
            mutation->control = send_gate.revision();
        if (options.bounded)
            ++bounded_commands;
        wake.notify_one();
    }

    void release_bounded()
    {
        std::lock_guard lock(mutex);
        --bounded_commands;
    }

    void publish_strategy_flags(const LiveAccountState& account) noexcept
    {
        strategy_batch_active.store(account.strategy_batch_active(), std::memory_order_release);
        strategy_events_pending.store(account.strategy_events_pending(), std::memory_order_release);
        strategy_has_failure.store(account.strategy_has_failure(), std::memory_order_release);
    }

    void start(
        LiveAccountState& account,
        std::shared_ptr<Mutation> request,
        std::function<AccountCommand(LiveAccountState&)> make = {})
    {
        TraceScope context(request->trace);
        command_progress.begin();
        try {
            auto command = make ? make(account) : request->begin(account, request->control);
            active.emplace(Active{request, std::move(command)});
            active->command.start();
        } catch (...) {
            const bool bounded = request->bounded;
            request->result.set_exception(std::current_exception());
            active.reset();
            command_progress.finish();
            if (bounded)
                release_bounded();
        }
    }

    void start_storage(LiveAccountState& account)
    {
        command_progress.begin();
        active.emplace(Active{nullptr, account.advance_storage()});
        active->command.start();
    }

    // While a submission waits on its broker quote, a cancel of an already
    // recorded order depends on nothing ahead of it and may run immediately.
    std::shared_ptr<Mutation> take_cancel(const LiveAccountState& account)
    {
        for (auto it = mutations.begin(); it != mutations.end(); ++it) {
            const auto& request = **it;
            if (request.cancel_order.empty()
                || request.request_id == waiting_quote->request->request_id
                || !account.holds_intent(request.cancel_order))
                continue;
            auto found = std::move(*it);
            mutations.erase(it);
            return found;
        }
        return nullptr;
    }

    void advance(LiveAccountState& account)
    {
        for (;;) {
            bool completed_storage = false;
            if (active) {
                if (!active->command.done())
                    return;
                failed = account.recovery_required();
                auto request = std::move(active->request);
                std::exception_ptr error;
                try {
                    active->command.result();
                } catch (...) {
                    error = std::current_exception();
                }
                active.reset();
                command_progress.finish();
                publish_strategy_flags(account);
                if (!error) {
                    if (auto deferred = account.take_deferred_quote()) {
                        waiting_quote.emplace(
                            WaitingQuote{std::move(request), std::move(*deferred)});
                        continue;
                    }
                    if (request)
                        request->result.set_value();
                } else {
                    if (request)
                        request->result.set_exception(error);
                    else
                        log::error("journal reconciliation failed");
                }
                completed_storage = !request;
                if (request && request->bounded)
                    release_bounded();
            }
            if (waiting_quote
                && waiting_quote->deferred.quote.wait_for(std::chrono::seconds{0})
                    == std::future_status::ready) {
                auto ready = std::move(*waiting_quote);
                waiting_quote.reset();
                start(account, std::move(ready.request), [&](LiveAccountState&) {
                    return ready.deferred.resume(std::move(ready.deferred.quote));
                });
                continue;
            }
            if (account.strategy_cancellation_pending()) {
                start_storage(account);
                continue;
            }
            // One bounded storage batch alternates with an already queued command.
            if (!completed_storage && account.storage_work_pending()) {
                start_storage(account);
                continue;
            }
            if (waiting_quote) {
                if (auto cancel = take_cancel(account)) {
                    start(account, std::move(cancel));
                    continue;
                }
                if (account.storage_work_pending()) {
                    start_storage(account);
                    continue;
                }
                return;
            }
            if (!mutations.empty()) {
                auto next = std::move(mutations.front());
                mutations.pop_front();
                start(account, std::move(next));
                continue;
            }
            if (account.storage_work_pending()) {
                start_storage(account);
                continue;
            }
            return;
        }
    }

    Loop(
        std::filesystem::path directory,
        std::filesystem::path sdk,
        std::filesystem::path ownership,
        Json manifest,
        std::shared_ptr<AlertWebhook> alerts,
        std::shared_ptr<const Clock> injected_clock)
        : clock(injected_clock ? std::move(injected_clock) : default_clock())
    {
        std::promise<void> started, finished;
        initialized = started.get_future().share();
        stopped = finished.get_future().share();
        thread = std::jthread([this,
                                  directory = std::move(directory),
                                  sdk = std::move(sdk),
                                  ownership = std::move(ownership),
                                  manifest = std::move(manifest),
                                  alerts = std::move(alerts),
                                  started = std::move(started),
                                  finished = std::move(finished),
                                  trace = std::string(current_trace_id())]() mutable {
            state_progress.begin();
            std::unique_ptr<LiveAccountState> account;
            try {
                TraceScope context(trace);
                account = std::make_unique<LiveAccountState>(
                    std::move(directory),
                    sdk,
                    ownership,
                    manifest,
                    [this](std::function<void()> completed) {
                        post_completion(std::move(completed));
                    },
                    [this] {
                        std::lock_guard lock(mutex);
                        broker_pending = true;
                        wake.notify_one();
                    },
                    send_gate,
                    persistence_progress,
                    std::move(alerts),
                    clock);
            } catch (...) {
                started.set_exception(std::current_exception());
                finished.set_value();
                return;
            }
            started.set_value();
            for (;;) {
                const auto broker_deadline = account->next_broker_deadline();
                const auto current = steady_now(*clock);
                const auto wait_for =
                    broker_deadline == std::chrono::steady_clock::time_point::max()
                    ? std::chrono::milliseconds{500}
                    : broker_deadline <= current
                    ? std::chrono::milliseconds{0}
                    : std::min<std::chrono::milliseconds>(
                          std::chrono::milliseconds{500},
                          std::chrono::duration_cast<std::chrono::milliseconds>(
                              broker_deadline - current));
                const bool storage_pending = account->storage_work_pending();
                Event event;
                {
                    std::unique_lock lock(mutex);
                    const auto ready = [&] {
                        return broker_pending || !events.empty()
                            || (stopping && !active && !waiting_quote);
                    };
                    wake.wait_for(lock, wait_for, ready);
                    const bool had_broker = std::exchange(broker_pending, false);
                    if (!had_broker && events.empty() && stopping && !active && !waiting_quote
                        && mutations.empty() && !storage_pending)
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
                    const auto& request = active->request;
                    TraceScope context(
                        request ? std::string_view(request->trace) : std::string_view{});
                    account->poll_sdk();
                }
                advance(*account);
                failed = account->recovery_required();
                business_ready = account->business_ready();
                publish_strategy_flags(*account);
                state_progress.finish();
            }
            business_ready = false;
            strategy_batch_active.store(false, std::memory_order_release);
            strategy_events_pending.store(false, std::memory_order_release);
            strategy_has_failure.store(false, std::memory_order_release);
            state_progress.begin();
            account.reset(); // Keep the owner alive while SDK callbacks and the writer stop.
            state_progress.finish();
            finished.set_value();
        });
    }

    void close_admission()
    {
        std::lock_guard lock(mutex);
        if (!accepting)
            return;
        accepting = false;
        send_gate.invalidate();
        events.push_front([](LiveAccountState& account) {
            account.fail_strategy("Trading is stopping");
        });
        wake.notify_one();
    }

    void stop()
    {
        close_admission();
        std::lock_guard lock(mutex);
        if (stopping)
            return;
        stopping = true;
        wake.notify_one();
    }

    ~Loop()
    {
        stop();
        thread.join();
    }

    template <class F>
    auto call(F operation, PostOptions options = {})
    {
        using Result = std::invoke_result_t<F, LiveAccountState&>;
        auto task = std::make_shared<std::packaged_task<Result(LiveAccountState&)>>(
            [this, operation = std::move(operation), trace = std::string(current_trace_id())](
                LiveAccountState& account) mutable -> Result {
                TraceScope context(trace);
                struct PublishHealth
                {
                    ~PublishHealth()
                    {
                        loop.failed = account.recovery_required();
                        loop.publish_strategy_flags(account);
                    }

                    Loop& loop;
                    LiveAccountState& account;
                } publish{*this, account};
                return operation(account);
            });
        auto result = task->get_future();
        enqueue(
            [task = std::move(task)](LiveAccountState& account) {
                (*task)(account);
            },
            options);
        return result;
    }

    std::future<void> mutate(
        std::function<AccountCommand(LiveAccountState&, std::uint64_t)> begin,
        std::function<void(LiveAccountState&)> admit = {},
        PostOptions options = {.bounded = true},
        std::string request_id = {},
        std::string cancel_order = {})
    {
        auto request = std::make_shared<Mutation>();
        request->begin = std::move(begin);
        request->admit = std::move(admit);
        request->trace = current_trace_id();
        request->request_id = std::move(request_id);
        request->cancel_order = std::move(cancel_order);
        request->bounded = options.bounded;
        auto result = request->result.get_future();
        enqueue(
            [this, request](LiveAccountState& account) {
                try {
                    TraceScope context(request->trace);
                    if (request->admit)
                        request->admit(account);
                } catch (...) {
                    const bool bounded = request->bounded;
                    request->result.set_exception(std::current_exception());
                    if (bounded)
                        release_bounded();
                    return;
                }
                mutations.push_back(request);
            },
            options,
            request.get());
        return result;
    }

    // One gate outlives the account, SDK and every accepted command.
    BrokerSendGate send_gate;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Event> events;
    std::size_t bounded_commands = 0;
    bool accepting = true, stopping = false, broker_pending = false;
    std::deque<std::shared_ptr<Mutation>> mutations;
    std::optional<Active> active;
    std::optional<WaitingQuote> waiting_quote;
    std::atomic<bool> failed{false};
    Progress state_progress, persistence_progress, command_progress;
    std::atomic<bool> business_ready{false};
    std::atomic<bool> strategy_batch_active{false};
    std::atomic<bool> strategy_events_pending{false};
    std::atomic<bool> strategy_has_failure{false};
    std::shared_future<void> initialized, stopped;
    std::shared_ptr<const Clock> clock;
    std::jthread thread;
};

LiveSession::LiveSession(
    std::filesystem::path directory,
    const std::filesystem::path& ctp_library,
    const std::filesystem::path& ownership_directory,
    const Json& manifest,
    std::shared_ptr<AlertWebhook> alerts,
    std::shared_ptr<const Clock> clock)
    : loop_(
          std::make_unique<Loop>(
              std::move(directory),
              ctp_library,
              ownership_directory,
              manifest,
              std::move(alerts),
              std::move(clock)))
{}

LiveSession::~LiveSession() = default;

std::shared_future<void> LiveSession::initialized() const
{
    return loop_->initialized;
}

void LiveSession::close_admission()
{
    loop_->close_admission();
}

void LiveSession::stop()
{
    loop_->stop();
}

std::shared_future<void> LiveSession::stopped() const
{
    return loop_->stopped;
}

std::future<void> LiveSession::connect(BrokerLogin login)
{
    return loop_->mutate(
        [login = std::move(
             login)](LiveAccountState& account, std::uint64_t) mutable -> AccountCommand {
            account.connect(std::move(login));
            co_return;
        });
}

std::future<void> LiveSession::disconnect()
{
    return loop_->call(
        [](LiveAccountState& account) {
            account.disconnect();
        },
        {.invalidate_sends = true});
}

std::future<void> LiveSession::query_costs()
{
    return loop_->call([](LiveAccountState& account) {
        account.query_costs();
    });
}

std::future<void> LiveSession::execute(
    std::string_view account_id,
    std::string_view policy_revision,
    AccountRequest request)
{
    if (const auto* order = request.as<SubmitOrder>(); order && order->strategy)
        throw Error(
            ErrorCode::invalid_request,
            "strategy orders require the internal execution entry");
    const bool submits = request.as<SubmitOrder>() != nullptr;
    const bool revoke = request.as<Revoke>();
    const auto* cancel = request.as<CancelOrder>();
    const bool cancels = cancel != nullptr;
    auto id = request.id;
    auto cancel_order = cancels ? cancel->order_id : std::string{};
    auto begin = [record = std::string(account_id),
                     policy = std::string(policy_revision),
                     request =
                         std::move(request)](LiveAccountState& account, std::uint64_t control) {
        return account.execute(record, policy, request, control);
    };
    if (revoke)
        return loop_->mutate(
            std::move(begin),
            [record = std::string(account_id), policy = std::string(policy_revision)](
                LiveAccountState& account) {
                account.admit_revoke(record, policy);
            },
            {.invalidate_sends = true},
            std::move(id));
    return loop_->mutate(
        std::move(begin),
        [submits](LiveAccountState& account) {
            if (submits)
                account.require_manual_order();
        },
        {.bounded = !cancels},
        std::move(id),
        std::move(cancel_order));
}

std::future<StrategyAuthorityPtr> LiveSession::register_strategy(
    std::string instance,
    std::vector<InstrumentId> contracts,
    std::optional<StrategyWeightBudget> budget,
    std::vector<DominantSeriesSpec> series,
    StrategyExecutionOptions execution)
{
    return loop_->call([instance = std::move(instance),
                           contracts = std::move(contracts),
                           budget,
                           series = std::move(series),
                           execution](LiveAccountState& account) mutable {
        return account.register_strategy(
            std::move(instance),
            std::move(contracts),
            budget,
            std::move(series),
            execution);
    });
}

std::future<void> LiveSession::roll_strategy_series(
    StrategyAuthorityPtr authority,
    std::size_t series_index,
    InstrumentId next_contract)
{
    return loop_->call(
        [authority = std::move(authority), series_index, next_contract = std::move(next_contract)](
            LiveAccountState& account) {
            account.roll_strategy_series(authority, series_index, next_contract);
        });
}

std::future<void> LiveSession::release_strategy(StrategyAuthorityPtr authority)
{
    return loop_->call(
        [authority = std::move(authority)](LiveAccountState& account) {
            account.release_strategy(authority);
        },
        {.allow_when_closing = true, .invalidate_sends = true});
}

std::future<std::string> LiveSession::strategy_failure(StrategyAuthorityPtr authority) const
{
    return loop_->call(
        [authority = std::move(authority)](LiveAccountState& account) {
            return account.strategy_failure(authority);
        },
        {.allow_when_closing = true});
}

std::future<void> LiveSession::pause_strategy(StrategyAuthorityPtr authority, bool paused)
{
    return loop_->call([authority = std::move(authority), paused](LiveAccountState& account) {
        account.pause_strategy(authority, paused);
    });
}

std::future<void>
LiveSession::flatten_strategy(StrategyAuthorityPtr authority, std::vector<MarketQuote> quotes)
{
    return loop_->call([authority = std::move(authority), quotes = std::move(quotes)](
                           LiveAccountState& account) mutable {
        account.flatten_strategy(authority, std::move(quotes));
    });
}

std::future<std::vector<::trading::InstanceEvent>> LiveSession::take_strategy_events(
    StrategyAuthorityPtr authority)
{
    return loop_->call(
        [authority = std::move(authority)](LiveAccountState& account) {
            return account.take_strategy_events(authority);
        },
        {.allow_when_closing = true});
}

std::future<void> LiveSession::execute_strategy(StrategyOrderBasis basis, SubmitOrder order)
{
    if (!basis.authority || basis.expires_at > steady_now(*loop_->clock) + std::chrono::seconds{10})
        throw Error(
            ErrorCode::invalid_request,
            "strategy order requires an account authority and at most 10 seconds of validity");
    auto request = AccountRequest::strategy_order(
        std::move(order),
        {basis.authority->instance(), basis.target_sequence, basis.leg});
    const auto request_id = request.id;
    return loop_->mutate(
        [basis = std::move(basis),
            request = std::move(request)](LiveAccountState& account, std::uint64_t control) {
            return account.execute_strategy(basis, request, control);
        },
        {},
        {.bounded = true},
        request_id);
}

std::future<void> LiveSession::accept_strategy_target(
    StrategyAuthorityPtr authority,
    ::strategy::Decision decision,
    std::vector<MarketQuote> quotes,
    std::chrono::steady_clock::time_point expires_at)
{
    return loop_->call([authority = std::move(authority),
                           decision = std::move(decision),
                           quotes = std::move(quotes),
                           expires_at](LiveAccountState& account) mutable {
        account
            .accept_strategy_target(authority, std::move(decision), std::move(quotes), expires_at);
    });
}

std::future<void> LiveSession::advance_strategy_target(
    StrategyAuthorityPtr authority,
    std::vector<MarketQuote> quotes)
{
    return loop_->mutate(
        [authority = std::move(authority),
            quotes = std::move(quotes)](LiveAccountState& account, std::uint64_t) mutable {
            return account.advance_strategy_target(authority, std::move(quotes));
        });
}

std::future<void> LiveSession::observe_quotes(std::vector<MarketQuote> quotes)
{
    return loop_->call([quotes = std::move(quotes)](LiveAccountState& account) {
        account.observe_quotes(quotes);
    });
}

std::future<::trading::Session> LiveSession::snapshot() const
{
    return loop_->call(
        [](LiveAccountState& account) {
            return account.snapshot();
        },
        {.allow_when_closing = true});
}

std::future<::trading::Orders> LiveSession::orders(std::uint32_t offset, std::uint32_t limit) const
{
    return loop_->call(
        [offset, limit](LiveAccountState& account) {
            return account.orders(offset, limit);
        },
        {.allow_when_closing = true});
}

std::future<::trading::Day> LiveSession::day(
    std::string trading_day,
    std::uint32_t offset,
    std::uint32_t limit,
    bool oldest_first) const
{
    return loop_->call(
        [trading_day = std::move(trading_day), offset, limit, oldest_first](
            LiveAccountState& account) {
            return account.day(trading_day, offset, limit, oldest_first);
        },
        {.allow_when_closing = true});
}

bool LiveSession::recovery_required() const noexcept
{
    return loop_->failed.load();
}

bool LiveSession::strategy_batch_active() const noexcept
{
    return loop_->strategy_batch_active.load(std::memory_order_acquire);
}

bool LiveSession::strategy_events_pending() const noexcept
{
    return loop_->strategy_events_pending.load(std::memory_order_acquire);
}

bool LiveSession::strategy_has_failure() const noexcept
{
    return loop_->strategy_has_failure.load(std::memory_order_acquire);
}

LiveSession::Health LiveSession::health() const noexcept
{
    return {
        loop_->state_progress.observe(),
        loop_->persistence_progress.observe(),
        loop_->command_progress.observe(),
        loop_->business_ready.load()
    };
}

} // namespace asterion::trading
