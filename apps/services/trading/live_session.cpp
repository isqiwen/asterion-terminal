#include <asterion/system/log.hpp>
#include <asterion/system/trace.hpp>
#include "live_account_state.hpp"
#include <algorithm>
#include <asterion/futures/account.hpp>
#include <asterion/futures/futures.hpp>
#include <asterion/base/error.hpp>
#include <asterion/system/durable_file.hpp>
#include <asterion/system/process/child.hpp>
#include <asterion/system/process/artifact.hpp>
#include <asterion/protocol/trading.hpp>
#include <chrono>
#include <set>
#include <fstream>
#include <stdexcept>

namespace asterion::trading {
namespace {

::common::Offset wire_offset(Offset offset)
{
    switch (offset) {
        case Offset::open:
            return ::common::OFFSET_OPEN;
        case Offset::close_today:
            return ::common::OFFSET_CLOSE_TODAY;
        case Offset::close_yesterday:
            return ::common::OFFSET_CLOSE_YESTERDAY;
        case Offset::close:
            return ::common::OFFSET_CLOSE;
    }
    throw std::logic_error("invalid open/close offset");
}

void put(::common::Decimal* field, const Decimal& value)
{
    field->set_text(value.str());
}

void put(::common::Instrument* field, const InstrumentId& id)
{
    field->set_exchange(id.venue);
    field->set_symbol(id.symbol);
}

// The counter's YYYYMMDD as the protocol writes trading days.
std::string dashed(const std::string& day)
{
    return day.empty() ? day : day.substr(0, 4) + "-" + day.substr(4, 2) + "-" + day.substr(6, 2);
}

::trading::Trade wire_trade(const BrokerTrade& trade)
{
    ::trading::Trade result;
    result.set_id(trade.trade_id);
    result.set_order_id(trade.order_id);
    put(result.mutable_instrument(), trade.instrument);
    result.set_side(trade.side == Side::buy ? ::common::SIDE_BUY : ::common::SIDE_SELL);
    result.set_offset(wire_offset(trade.offset));
    put(result.mutable_lots(), trade.quantity);
    put(result.mutable_price(), trade.price);
    result.set_trading_day(dashed(trade.trading_day));
    result.set_trade_time(trade.trade_time);
    result.set_received_ms(trade.received_ms);
    return result;
}

::trading::Order wire_order(const BrokerOrder& order)
{
    ::trading::Order result;
    result.set_id(order.order_id);
    result.set_broker_key(order.broker_key);
    put(result.mutable_instrument(), order.instrument);
    result.set_side(order.side == Side::buy ? ::common::SIDE_BUY : ::common::SIDE_SELL);
    result.set_offset(wire_offset(order.offset));
    put(result.mutable_lots(), order.quantity);
    put(result.mutable_filled_lots(), order.filled);
    put(result.mutable_price(), order.limit_price);
    result.set_state(static_cast<::trading::OrderState>(static_cast<int>(order.status) + 1));
    result.set_ctp_error(order.error_code);
    return result;
}

// The broker's funds and positions of its trading day.
::trading::DayClose close_of(const BrokerSnapshot& state)
{
    ::trading::DayClose result;
    result.set_trading_day(dashed(state.trading_day));
    if (state.funds) {
        auto* funds = result.mutable_funds();
        put(funds->mutable_balance(), state.funds->balance);
        put(funds->mutable_available(), state.funds->available);
        put(funds->mutable_margin(), state.funds->margin);
        put(funds->mutable_commission(), state.funds->commission);
        put(funds->mutable_close_profit(), state.funds->close_profit);
        put(funds->mutable_position_profit(), state.funds->position_profit);
        put(funds->mutable_pre_balance(), state.funds->pre_balance);
        put(funds->mutable_deposit(), state.funds->deposit);
        put(funds->mutable_withdraw(), state.funds->withdraw);
    }
    for (const auto& position : state.positions) {
        auto* item = result.add_positions();
        put(item->mutable_instrument(), position.instrument);
        item->set_side(position.side == Side::buy ? ::common::SIDE_BUY : ::common::SIDE_SELL);
        put(item->mutable_today_lots(), position.today);
        put(item->mutable_yesterday_lots(), position.yesterday);
        put(item->mutable_cost(), position.cost);
        put(item->mutable_profit(), position.profit);
        put(item->mutable_margin(), position.margin);
    }
    return result;
}

constexpr auto quote_validity = std::chrono::seconds(10);

std::string text(const Json& value, const char* key)
{
    auto result = value.at(key).get<std::string>();
    if (result.empty() || result.find('\0') != std::string::npos)
        throw std::invalid_argument("invalid live trading field");
    return result;
}

} // namespace

LiveAccountState::LiveAccountState(
    std::filesystem::path directory,
    const std::filesystem::path& ctp_library,
    const std::filesystem::path& ownership_directory,
    const Json& create_manifest,
    AccountJournal::Post post,
    std::function<void()> broker_ready,
    BrokerSendGate& send_gate,
    Progress& persistence,
    std::shared_ptr<AlertWebhook> alerts,
    std::shared_ptr<const Clock> clock)
    : post_(post),
      directory_(directory),
      send_gate_(send_gate),
      alerts_(std::move(alerts)),
      clock_(clock ? std::move(clock) : default_clock())
{
    Json manifest;
    if (!create_manifest.is_null()) {
        manifest = protocol::live_input(create_manifest);
        if (std::filesystem::exists(directory / "journal.sqlite"))
            throw std::invalid_argument("directory already holds a session; recover it instead");
    } else {
        header_ = read_journal_header(directory, AccountJournal::sidecars);
        manifest = account_manifest(header_);
    }
    environment_ = manifest.at("environment") == "real" ? ::trading::ENVIRONMENT_REAL
                                                        : ::trading::ENVIRONMENT_SIMULATION;
    const auto& broker = manifest.at("broker");
    broker_id_ = broker.at("broker_id");
    investor_id_ = broker.at("user_id");
    account_id_ = text(manifest, "account_id");
    const auto& account_id = account_id_;
    create_directories_durably(ownership_directory);
    const auto owner_key = sha256_bytes(broker_id_ + '\n' + investor_id_);
    try {
        account_owner_ = std::make_unique<FileLock>(ownership_directory, owner_key);
    } catch (const std::runtime_error&) {
        throw Error(
            ErrorCode::conflict,
            "account execution owner is already active or ownership storage is unavailable");
    }
    const auto binding_file = ownership_directory / (owner_key + ".json");
    Json binding{
        {"directory", std::filesystem::canonical(directory).string()},
        {"initialized", false}
    };
    if (std::filesystem::exists(binding_file)) {
        if (std::filesystem::is_symlink(binding_file)
            || !std::filesystem::is_regular_file(binding_file)
            || std::filesystem::file_size(binding_file) > 16384)
            throw std::invalid_argument("invalid account ownership binding");
        std::ifstream source(binding_file);
        const auto stored =
            parse_json(std::string(std::istreambuf_iterator<char>(source), {}), 16384);
        require_fields(stored, {"directory", "initialized"});
        if (!stored.at("initialized").is_boolean())
            throw std::invalid_argument("invalid account ownership binding");
        if (stored.at("directory") != binding.at("directory"))
            throw Error(
                ErrorCode::conflict,
                "this broker account is bound to trading directory "
                    + stored.at("directory").get<std::string>());
        if (stored.at("initialized") == true && !create_manifest.is_null())
            throw Error(
                ErrorCode::conflict,
                "this account is initialized; recover its original trading record");
        binding = stored;
    } else {
        // Claim before any writable journal or SDK work. A failed initial creation
        // may be retried only at this location for this account record.
        replace_file_durably(binding_file, binding.dump());
    }
    const auto finish_initialization = [&] {
        if (binding.at("initialized") == false) {
            binding["initialized"] = true;
            replace_file_durably(binding_file, binding.dump());
        }
    };
    // Acquire the account before opening its journal for writing or creating an SDK instance.
    journal_ = std::make_unique<AccountJournal>(
        directory,
        [](const Json& header) {
            (void)account_manifest(header);
        },
        std::move(post),
        persistence);
    capacity_ = journal_->initial_capacity();
    auto definition = PolicyDefinition::from_wire(protocol::encode_policy(manifest.at("policy")));
    auto revision =
        create_manifest.is_null() ? text(header_, "policy_revision") : unique_process_id();
    std::set<std::string> revisions{revision};
    std::set<std::string> pending_cancels;
    {
        const auto saved_close = directory / "ctp-flow/latest_close.pb";
        if (std::filesystem::is_regular_file(saved_close)
            && !std::filesystem::is_symlink(saved_close)
            && std::filesystem::file_size(saved_close) <= 1048576) {
            std::ifstream in(saved_close, std::ios::binary);
            std::string bytes{std::istreambuf_iterator<char>(in), {}};
            if (latest_.ParseFromString(bytes) && latest_.trading_day().size() == 10) {
                latest_close_bytes_ = std::move(bytes);
                latest_saved_day_ = latest_.trading_day();
            } else
                latest_.Clear();
        }
    }
    journal_->replay(
        [&](const Json& header) {
            if (header != header_)
                throw Error(
                    ErrorCode::conflict,
                    "account record changed while acquiring execution ownership");
        },
        [&](const JournalRecord& record) {
            if (const auto* recorded = std::get_if<TradesRecorded>(&record)) {
                for (const auto& trade : recorded->trades)
                    record_trade(trade);
                return;
            }
            if (const auto* closed = std::get_if<DayClosed>(&record)) {
                closes_[closed->close.trading_day()] = closed->close;
                return;
            }
            if (const auto* terminal = std::get_if<OrdersTerminal>(&record)) {
                for (const auto& order : terminal->orders) {
                    const auto found = intents_.find(order.order_id);
                    if (found == intents_.end())
                        throw std::invalid_argument("invalid persisted terminal order evidence");
                    check_terminal(order, found->second);
                    if (cancel_awaited_.erase(order.order_id)
                        && order.status != BrokerOrderStatus::cancelled)
                        if (const auto origin = order_origins_.find(order.order_id);
                            origin != order_origins_.end())
                            ++cancels_[origin->second.day][origin->second.instrument]
                                  .rejected_cancels;
                    intents_.erase(found);
                }
                return;
            }
            if (const auto* unsent = std::get_if<OrderNotSent>(&record)) {
                if (!intents_.erase(unsent->order_id))
                    throw std::invalid_argument("invalid unsent live order result");
                order_origins_.erase(unsent->order_id);
                return;
            }
            if (const auto* cancel = std::get_if<CancelResult>(&record)) {
                if (!pending_cancels.erase(cancel->request_id))
                    throw std::invalid_argument("invalid live cancellation result");
                if (const auto req = cancel_requests_.find(cancel->request_id);
                    req != cancel_requests_.end()) {
                    if (!cancel->acknowledged && cancel_awaited_.erase(req->second))
                        if (const auto origin = order_origins_.find(req->second);
                            origin != order_origins_.end())
                            ++cancels_[origin->second.day][origin->second.instrument]
                                  .rejected_cancels;
                    cancel_requests_.erase(req);
                }
                return;
            }
            const auto& command = std::get<CommandRecord>(record);
            const auto& request = command.request;
            if (command.policy_revision != revision)
                throw std::invalid_argument(
                    "trading command policy does not match the active revision");
            if (const auto* cancel = request.as<CancelOrder>()) {
                pending_cancels.insert(request.id);
                cancel_requests_[request.id] = cancel->order_id;
                cancel_awaited_.insert(cancel->order_id);
                if (const auto origin = order_origins_.find(cancel->order_id);
                    origin != order_origins_.end())
                    ++cancels_[origin->second.day][origin->second.instrument].cancels;
                if (const auto found = intents_.find(cancel->order_id); found != intents_.end())
                    found->second.cancel_requested = true;
            } else if (const auto* change = request.as<ChangePolicy>()) {
                revision = command.new_policy_revision;
                if (!revisions.insert(revision).second)
                    throw std::invalid_argument("account policy revision is repeated");
                definition = change->definition;
            } else if (const auto* resolve = request.as<ResolveOrder>()) {
                if (!intents_.erase(resolve->order_id))
                    throw std::invalid_argument("live trading record resolves an unknown order");
            } else if (const auto* submit = request.as<SubmitOrder>()) {
                order_origins_[submit->order.id] = {
                    dashed(command.trading_day),
                    submit->order.instrument
                };
                intents_[submit->order.id] = {
                    command.broker_key,
                    command.trading_day,
                    submit->order.instrument,
                    submit->order.side,
                    submit->offset,
                    submit->order.quantity,
                    submit->order.limit_price,
                    std::nullopt,
                    submit->strategy
                };
            }
        });
    policy_ = std::make_unique<AccountPolicy>(std::move(definition), revision);
    trader_ = std::make_unique<ctp::Trader>(
        ctp_library,
        directory / "ctp-flow",
        send_gate_,
        std::move(broker_ready),
        clock_);
    trader_->start();
    if (!create_manifest.is_null()) {
        header_ = {
            {"manifest", manifest},
            {"policy_revision", policy_->revision},
            {"account_id", account_id}
        };
        capacity_ = journal_->initialize(header_);
    }
    finish_initialization();
}

LiveAccountState::~LiveAccountState()
{
    if (!latest_.trading_day().empty())
        save_latest_close(latest_, true);
    if (trader_)
        trader_->disconnect();
}

AccountCommand LiveAccountState::append(JournalEntry entry)
{
    const auto trace = entry.trace;
    try {
        capacity_ = (co_await journal_->append(std::move(entry))).capacity;
    } catch (...) {
        failed_ = true;
        fail_strategy("account journal needs recovery");
        if (alerts_)
            alerts_->notify(
                "recovery_required",
                "recovery_required",
                "trading journal requires operator recovery");
        throw;
    }
    log::info(
        "journal committed: trace_id={} account_id={} request_id={} action={} order_id={} " "broker_key={} outcome={}",
        current_trace_id(),
        account_id_,
        trace.request_id,
        trace.action,
        trace.order_id,
        trace.broker_key,
        trace.outcome);
}

void LiveAccountState::log_broker_observations(const BrokerSnapshot& state) const noexcept
{
    try {
        if (!log::enabled(log::Level::info)
            || (observed_generation_ == state.connection_generation
                && observed_sequence_ == state.sequence))
            return;
        if (observed_generation_ != state.connection_generation) {
            observed_orders_.clear();
            observed_trades_ = 0;
            observed_generation_ = state.connection_generation;
        }
        const auto& record = account_id_;
        for (const auto& order : state.orders) {
            ObservedOrder
                observation{order.status, order.filled, order.exchange_order_id, order.order_id};
            auto [found, inserted] = observed_orders_.try_emplace(order.broker_key, observation);
            if (!inserted && found->second == observation)
                continue;
            found->second = std::move(observation);
            const auto status = status_name(order.status);
            log::info(
                "broker order observed: account_id={} trading_day={} order_id={} broker_key={} " "exchange_order_id={} status={}",
                record,
                state.trading_day,
                order.order_id,
                order.broker_key,
                order.exchange_order_id,
                status);
        }
        if (observed_trades_ > state.trades.size())
            observed_trades_ = 0;
        for (; observed_trades_ < state.trades.size(); ++observed_trades_) {
            const auto& trade = state.trades[observed_trades_];
            log::info(
                "broker trade observed: account_id={} order_id={} exchange_order_id={} " "trade_id={} trading_day={}",
                record,
                trade.order_id,
                trade.exchange_order_id,
                trade.trade_id,
                trade.trading_day);
        }
        observed_sequence_ = state.sequence;
    } catch (...) {
        log::error("broker observations could not be logged");
    }
}

void LiveAccountState::connect(BrokerLogin login)
{
    if (failed_)
        throw std::invalid_argument("live trading record needs recovery; reopen the session");
    if (login.password.empty() || login.auth_code.empty())
        throw std::invalid_argument("enter the CTP password and authentication code");
    send_gate_.invalidate();
    trader_->connect(
        {std::move(login.front),
            broker_id_,
            investor_id_,
            std::move(login.password),
            std::move(login.app_id),
            std::move(login.auth_code),
            policy_->definition.limits.max_margin_ratio > Decimal{}});
}

void LiveAccountState::query_costs()
{
    std::vector<std::pair<InstrumentId, std::string>> contracts;
    for (const auto& c : policy_->definition.contracts)
        if (!c.delivery_month.empty())
            contracts.emplace_back(c.instrument.id, c.product);
    trader_->observe([&](const BrokerSnapshot& state) {
        for (const auto& position : state.positions)
            if (const auto* found = policy_->find(position.instrument);
                found && found->delivery_month.empty())
                contracts.emplace_back(position.instrument, found->product);
    });
    if (!contracts.empty())
        trader_->query_costs(contracts);
}

void LiveAccountState::disconnect()
{
    fail_strategy("account connection interrupted");
    send_gate_.invalidate();
    trader_->disconnect();
}

Instrument LiveAccountState::allowed(const InstrumentId& id) const
{
    if (const auto* contract = policy_->find(id))
        return contract->terms_for(id);
    throw std::invalid_argument("contract is not allowed in this live session");
}

// Unretired durable intents which the synchronized broker does not report.
// A new trading day cannot prove that a previous day's unknown command was absent.
std::vector<std::pair<std::string, const LiveAccountState::Intent*>> LiveAccountState::unconfirmed(
    const BrokerSnapshot& state) const
{
    std::vector<std::pair<std::string, const Intent*>> result;
    if (state.phase != BrokerPhase::ready || !identities_ready(state))
        return result;
    for (const auto& [order_id, intent] : intents_) {
        const bool reported = std::ranges::any_of(state.orders, [&](const BrokerOrder& order) {
            return intent.trading_day == state.trading_day && order.broker_key == intent.broker_key
                && order.order_id == order_id;
        });
        if (!reported)
            result.emplace_back(order_id, &intent);
    }
    return result;
}

void LiveAccountState::check_account(std::string_view account_id) const
{
    if (account_id.empty() || account_id != account_id_)
        throw Error(
            ErrorCode::conflict,
            "trading account identity does not match; review the current account before acting");
}

AccountCommand
LiveAccountState::revise_policy(const AccountRequest& request, const ChangePolicy& change)
{
    std::unique_ptr<const AccountPolicy> next =
        std::make_unique<AccountPolicy>(change.definition, unique_process_id());
    {
        const auto state = trader_->snapshot();
        if (state.phase != BrokerPhase::ready || !identities_ready(state)
            || !state.positions_reconciled || !unconfirmed(state).empty())
            throw Error(
                ErrorCode::conflict,
                "policy change requires reconciled positions and no unconfirmed orders");
        for (const auto& position : state.positions)
            if (position.today + position.yesterday > Decimal{})
                next->preserve_exposure(*policy_, position.instrument);
        for (const auto& order : state.orders)
            if (working(order.status))
                next->preserve_exposure(*policy_, order.instrument);
        trader_->disconnect_checked(state.connection_generation, state.exposure_revision);
        end_authorization();
        send_gate_.invalidate();
        co_await append(policy_entry(request, policy_->revision, next->revision));
        policy_.swap(next);
        ++close_state_revision_;
    }
}

void LiveAccountState::admit_revoke(std::string_view account_id, std::string_view revision)
{
    check_account(account_id);
    if (revision != policy_->revision)
        throw Error(
            ErrorCode::conflict,
            "account policy changed; review the current revision before acting");
    // The entry point already closed permission. Only the owner changes authorization.
    end_authorization();
}

AccountCommand LiveAccountState::execute(
    std::string account_id,
    std::string policy_revision,
    AccountRequest request,
    std::uint64_t admitted_control)
{
    check_account(account_id);
    if (failed_)
        throw std::invalid_argument("live trading record needs recovery; reopen the session");
    std::optional<CommandRecord> recorded;
    try {
        recorded = std::move((co_await journal_->find_command(request.id)).command);
    } catch (...) {
        failed_ = true;
        throw;
    }
    if (recorded) {
        // A retried order or cancel is acknowledged, never sent again. An
        // authorization is never acknowledged from history: it may have ended.
        if (recorded->request.command != request.command
            || recorded->policy_revision != policy_revision || request.as<Authorize>()
            || request.as<Revoke>())
            throw Error(ErrorCode::conflict, "request ID was already used");
        co_return;
    }
    if (policy_revision != policy_->revision)
        throw Error(
            ErrorCode::conflict,
            "account policy changed; review the current revision before acting");
    if ((request.as<Authorize>() || request.as<SubmitOrder>())
        && admitted_control != send_gate_.revision())
        throw Error(
            ErrorCode::conflict,
            "account control changed after command admission; submit a new request");
    if (const auto* change = request.as<ChangePolicy>()) {
        co_await revise_policy(request, *change);
    } else if (const auto* grant = request.as<Authorize>()) {
        if (grant->user_id != investor_id_)
            throw std::invalid_argument("authorization names a different account");
        const auto state = trader_->snapshot();
        if (state.phase != BrokerPhase::ready || !identities_ready(state))
            throw Error(
                ErrorCode::unavailable,
                "connect and synchronize the account before authorizing");
        Authorization authorization{utc_now_ms(*clock_)};
        co_await append(authorization_entry(request, policy_->revision, authorization));
        if (admitted_control != send_gate_.revision())
            throw Error(
                ErrorCode::conflict,
                "account state changed before durable grant completed");
        authorization_ = authorization;
    } else if (request.as<Revoke>()) {
        co_await append(command_entry(request, policy_->revision));
    } else if (const auto* resolve = request.as<ResolveOrder>()) {
        // Only an order listed as unconfirmed right now: the broker is
        // synchronized and does not report it.
        const auto listed = observed([&](const BrokerSnapshot& state) {
            return unconfirmed(state);
        });
        if (std::ranges::none_of(listed, [&](const auto& item) {
                return item.first == resolve->order_id;
            }))
            throw std::invalid_argument(
                "only an unconfirmed order of a synchronized account can be " "resolved");
        co_await append(command_entry(request, policy_->revision));
        intents_.erase(resolve->order_id);
    } else if (const auto* order = request.as<SubmitOrder>()) {
        require_manual_order();
        co_await submit(request, *order, admitted_control);
    } else {
        co_await cancel(request, std::get<CancelOrder>(request.operation).order_id);
    }
}

AccountCommand LiveAccountState::cancel(const AccountRequest& request, const std::string& order_id)
{
    if (!observed([&](const BrokerSnapshot& state) {
            return identities_ready(state);
        }))
        throw Error(ErrorCode::unavailable, "account identity reconciliation is not complete");
    if (!intents_.contains(order_id))
        throw Error(ErrorCode::conflict, "order is not active in this account");
    co_await append(command_entry(request, policy_->revision));
    const auto origin = order_origins_.find(order_id);
    if (origin != order_origins_.end())
        ++cancels_[origin->second.day][origin->second.instrument].cancels;
    cancel_awaited_.insert(order_id);
    ++close_state_revision_;
    trader_->observe([&](const BrokerSnapshot& state) {
        evaluate_risk_alerts(state);
    });
    auto& intent = intents_.at(order_id);
    intent.cancel_requested = true;
    intent.stop_cancel_pending = false;
    std::exception_ptr failure;
    try {
        const auto result = co_await wait_sdk(trader_->cancel(order_id));
        if (!result.invoked || result.code)
            throw Error(
                ErrorCode::operation_failed,
                "CTP cancel request failed with code " + std::to_string(result.code));
    } catch (...) {
        failure = std::current_exception();
    }
    // SDK acceptance is not exchange cancellation. Broker reports own status.
    co_await append(entry(CancelResult{request.id, !failure}));
    if (failure) {
        if (cancel_awaited_.erase(order_id)) {
            if (origin != order_origins_.end())
                ++cancels_[origin->second.day][origin->second.instrument].rejected_cancels;
            ++close_state_revision_;
        }
        std::rethrow_exception(failure);
    }
}

void LiveAccountState::end_authorization()
{
    fail_strategy("account authorization ended");
    authorization_.reset();
}

// The broker's current market bounds the limit price: within the exchange's
// limits and within the session's deviation from the latest price (the
// pre-settlement price before the first trade). No market, no order.
void LiveAccountState::check_price(
    const LimitOrder& order,
    const std::optional<BrokerQuote>& quote,
    const BrokerSnapshot& state) const
{
    if (quote
        && (quote->instrument != order.instrument || quote->trading_day != state.trading_day
            || quote->connection_generation != state.connection_generation
            || steady_now(*clock_) >= quote->completed_at + quote_validity))
        throw Error(
            ErrorCode::unavailable,
            "broker quote is expired or belongs to another trading session; the order was not sent");
    const auto reference = !quote ? std::nullopt
        : quote->last             ? quote->last
                                  : quote->pre_settlement;
    if (!reference || *reference <= Decimal{})
        throw std::invalid_argument(
            "no current market price for this contract; the order was not sent");
    if ((quote->upper_limit && order.limit_price > *quote->upper_limit)
        || (quote->lower_limit && order.limit_price < *quote->lower_limit))
        throw std::invalid_argument("limit price is outside the exchange price limits");
    const auto distance = order.limit_price > *reference ? order.limit_price - *reference
                                                         : *reference - order.limit_price;
    // Truncate toward zero so rounding never widens the deviation limit.
    const auto bound =
        multiply(*reference, policy_->definition.max_price_deviation, Rounding::toward_zero);
    if (distance > bound)
        throw std::invalid_argument(
            "limit price deviates from the latest price beyond the session limit");
}

bool LiveAccountState::MarginDataWait::await_ready() const
{
    return account.observed([&](const BrokerSnapshot& state) {
        if (state.phase != BrokerPhase::ready)
            return true;
        if (!state.margin_snapshot && state.refreshing_funds)
            return false;
        return std::ranges::none_of(state.costs, [&](const BrokerCosts& row) {
            return row.state == BrokerCostsState::querying
                && std::ranges::any_of(contracts, [&](const auto& item) {
                       return item.first == row.instrument;
                   });
        });
    });
}

void LiveAccountState::MarginDataWait::await_suspend(std::coroutine_handle<> command)
{
    account.sdk_wait_.emplace(
        SdkWait{
            [this] {
                return await_ready();
            },
            command
        });
}

AccountCommand LiveAccountState::query_margin_rates(const InstrumentId& instrument)
{
    co_await MarginDataWait{*this, {}};
    const auto state = trader_->snapshot();
    if (!state.margin_snapshot)
        throw Error(ErrorCode::unavailable, "opening orders require synchronized account margin");
    std::set<InstrumentId> needed{instrument};
    for (const auto& order : state.orders)
        if (order.offset == Offset::open && working(order.status))
            needed.insert(order.instrument);
    for (std::size_t i = state.margin_snapshot->trade_count; i < state.trades.size(); ++i)
        if (state.trades[i].offset == Offset::open)
            needed.insert(state.trades[i].instrument);
    for (const auto& [_, intent] : unconfirmed(state))
        if (intent->offset == Offset::open)
            needed.insert(intent->instrument);

    std::vector<std::pair<InstrumentId, std::string>> missing;
    for (const auto& id : needed) {
        const auto found = std::ranges::find(state.costs, id, &BrokerCosts::instrument);
        if (found != state.costs.end() && found->state == BrokerCostsState::ready)
            continue;
        const auto* contract = policy_->find(id);
        if (!contract)
            throw Error(ErrorCode::unavailable, "margin cannot be valued for " + id.symbol);
        missing.emplace_back(id, contract->product);
    }
    if (!missing.empty()) {
        trader_->query_costs(missing);
        co_await MarginDataWait{*this, missing};
    }
}

Decimal
LiveAccountState::projected_margin(const BrokerSnapshot& state, const LimitOrder& order) const
{
    if (!state.margin_snapshot || !state.funds)
        throw Error(ErrorCode::unavailable, "opening orders require synchronized account margin");
    const auto margin = [&](const InstrumentId& id, Side side, Decimal quantity, Decimal price) {
        const auto cost = std::ranges::find(state.costs, id, &BrokerCosts::instrument);
        const auto* terms = policy_->find(id);
        if (!terms || price <= Decimal{} || cost == state.costs.end()
            || cost->state != BrokerCostsState::ready || !cost->costs
            || (cost->costs->margin_per_lot <= Decimal{} && cost->costs->margin_rate <= Decimal{}))
            throw Error(
                ErrorCode::unavailable,
                "opening orders require a broker margin rate and price for " + id.symbol);
        return cost->costs->margin(side, quantity, price, terms->instrument.multiplier);
    };

    const auto& basis = *state.margin_snapshot;
    const auto unknown = unconfirmed(state);
    Decimal previously_working, additional;
    std::set<std::string> frozen_ids;
    std::map<std::string, Decimal> traded;
    for (const auto& reported : state.orders)
        if (basis.working_opens.contains(reported.broker_key)
            && !reported.exchange_order_id.empty())
            frozen_ids.insert(reported.exchange_order_id);
    for (std::size_t i = 0; i < state.trades.size(); ++i) {
        const auto& fill = state.trades[i];
        if (fill.offset != Offset::open)
            continue;
        auto& quantity = traded[fill.exchange_order_id];
        quantity = quantity + fill.quantity;
        if (i >= basis.trade_count) {
            if (fill.order_id.empty() && std::ranges::any_of(unknown, [&](const auto& entry) {
                    const auto* intent = entry.second;
                    return intent->trading_day == state.trading_day
                        && intent->instrument == fill.instrument && intent->offset == Offset::open
                        && intent->side == fill.side;
                }))
                throw Error(
                    ErrorCode::unavailable,
                    "opening margin awaits broker trade attribution");
            auto& reserve =
                frozen_ids.contains(fill.exchange_order_id) ? previously_working : additional;
            reserve = reserve + margin(fill.instrument, fill.side, fill.quantity, fill.price);
        }
    }
    for (const auto& reported : state.orders) {
        if (reported.offset != Offset::open)
            continue;
        const auto filled = traded[reported.exchange_order_id];
        if (reported.filled > filled) {
            // The trade price is still unknown when the order report arrives first.
            throw Error(ErrorCode::unavailable, "opening margin awaits broker trade prices");
        }
        if (working(reported.status) && reported.quantity > filled) {
            auto& reserve =
                basis.working_opens.contains(reported.broker_key) ? previously_working : additional;
            reserve = reserve
                + margin(
                    reported.instrument,
                    reported.side,
                    reported.quantity - filled,
                    reported.limit_price);
        }
    }
    for (const auto& [_, intent] : unknown)
        if (intent->offset == Offset::open)
            additional = additional
                + margin(intent->instrument, intent->side, intent->quantity, intent->limit_price);

    // Only orders already at the counter during the funds query share its frozen margin.
    return basis.held + std::max(previously_working, basis.frozen) + additional
        + margin(order.instrument, order.side, order.quantity, order.limit_price);
}

void LiveAccountState::check_closable_lots(
    const BrokerSnapshot& state,
    const LimitOrder& order,
    Offset offset) const
{
    const auto& id = order.instrument;
    const auto held_side = order.side == Side::buy ? Side::sell : Side::buy;
    Decimal closable;
    for (const auto& position : state.positions) {
        if (position.instrument != id || position.side != held_side)
            continue;
        if (offset == Offset::close_today)
            closable = closable + position.today;
        else if (offset == Offset::close_yesterday
            || close_policy(id.venue) == ClosePolicy::explicit_buckets)
            // Where today's lots are kept apart, a plain close takes yesterday's.
            closable = closable + position.yesterday;
        else
            closable = closable + position.today + position.yesterday;
    }
    const auto reduces_pool = [&](Offset pending_offset) {
        if (close_policy(id.venue) != ClosePolicy::explicit_buckets)
            return true;
        return (offset == Offset::close ? Offset::close_yesterday : offset)
            == (pending_offset == Offset::close ? Offset::close_yesterday : pending_offset);
    };
    for (const auto& [key, quantity] : state.unreconciled_close_lots) {
        const auto& [instrument, side, filled_offset] = key;
        if (instrument == id && side == order.side && reduces_pool(filled_offset))
            closable = closable - quantity;
    }
    for (const auto& o : state.orders)
        if (working(o.status) && o.instrument == id && o.side == order.side
            && o.offset != Offset::open && reduces_pool(o.offset))
            closable = closable - (o.quantity - o.filled);
    for (const auto& [_, intent] : unconfirmed(state))
        if (intent->instrument == id && intent->side == order.side && intent->offset != Offset::open
            && reduces_pool(intent->offset))
            closable = closable - intent->quantity;
    if (order.quantity > closable)
        throw std::invalid_argument("closing quantity exceeds available position");
}

void LiveAccountState::check_delivery_month(
    const InstrumentId& id,
    Offset offset,
    std::string_view trading_day) const
{
    if (offset != Offset::open)
        return;
    const auto* contract = policy_->find(id);
    if (!contract || !prohibits_natural_person_delivery_month(id.venue, contract->product))
        return;
    if (trading_day.size() != 8 || !std::ranges::all_of(trading_day, [](char c) {
            return c >= '0' && c <= '9';
        }))
        throw Error(ErrorCode::unavailable, "opening orders require a valid broker trading day");
    const auto trading_month =
        std::string(trading_day.substr(0, 4)) + "-" + std::string(trading_day.substr(4, 2));
    if (trading_month >= contract->delivery_month_for(id, trading_day))
        throw std::invalid_argument(
            "opening orders are prohibited in or after the delivery month for natural person accounts");
}

void LiveAccountState::check_self_trade(const BrokerSnapshot& state, const LimitOrder& order) const
{
    const auto crosses = [&](const InstrumentId& id, Side side, Decimal price) {
        if (id != order.instrument || side == order.side)
            return false;
        return order.side == Side::buy ? order.limit_price >= price : order.limit_price <= price;
    };
    for (const auto& existing : state.orders)
        if (working(existing.status)
            && crosses(existing.instrument, existing.side, existing.limit_price))
            throw std::invalid_argument(
                "order price crosses an active opposite-side order on the same contract");
    for (const auto& [_, intent] : unconfirmed(state))
        if (crosses(intent->instrument, intent->side, intent->limit_price))
            throw std::invalid_argument(
                "order price crosses an active opposite-side order on the same contract");
}

void LiveAccountState::check_pre_trade_risk(
    const BrokerSnapshot& state,
    const Instrument& terms,
    const LimitOrder& order,
    Offset offset,
    bool check_margin) const
{
    // Exposure from broker reports, plus recorded orders the broker has not
    // confirmed: they may still reach the exchange.
    const auto& id = order.instrument;
    RiskExposure exposure;
    exposure.add_unreconciled_open(state.unreconciled_open_lots);
    for (const auto& position : state.positions)
        exposure.add_position(id, position.instrument, position.today + position.yesterday);
    for (const auto& o : state.orders)
        if (working(o.status))
            exposure.add_working_order(id, o.instrument, o.offset, o.quantity - o.filled);
    for (const auto& [_, intent] : unconfirmed(state))
        exposure.add_working_order(id, intent->instrument, intent->offset, intent->quantity);
    if (!state.funds)
        throw Error(ErrorCode::unavailable, "orders require synchronized account funds");
    const auto val = valuation(state);
    std::uint32_t contract_cancels = 0;
    if (const auto day_it = cancels_.find(dashed(state.trading_day)); day_it != cancels_.end())
        if (const auto inst_it = day_it->second.find(id); inst_it != day_it->second.end())
            contract_cancels = inst_it->second.cancels;
    for (const auto& existing : state.orders)
        if (existing.instrument == id && existing.order_id.empty()
            && existing.status == BrokerOrderStatus::cancelled)
            ++contract_cancels;
    auto context = exposure.context(terms, order, offset);
    context.daily_loss = val.daily_loss;
    context.drawdown = val.drawdown;
    context.account_equity = val.dynamic_equity;
    context.account_margin = check_margin ? projected_margin(state, order) : Decimal{};
    context.contract_daily_cancels = contract_cancels;
    const auto decision = policy_->risk.evaluate(context);
    if (!decision.allowed()) {
        evaluate_risk_alerts(state);
        throw std::invalid_argument(
            "pre-trade risk rejected: " + std::string(risk_reason_name(decision.reason)));
    }
}

void LiveAccountState::check_order_source(
    const SubmitOrder& submission,
    const std::optional<StrategyOrderBasis>& strategy) const
{
    if (!strategy)
        return require_manual_order();
    check_strategy(*strategy);
    if (submission.offset == Offset::open && strategy_->paused)
        throw Error(ErrorCode::conflict, "strategy is paused; the opening order was not sent");
}

AccountCommand LiveAccountState::submit(
    const AccountRequest& request,
    const SubmitOrder& submission,
    std::uint64_t control,
    std::optional<StrategyOrderBasis> strategy)
{
    check_order_source(submission, strategy);
    const auto& order = submission.order;
    const auto& order_id = order.id;
    const auto& id = order.instrument;
    const auto offset = submission.offset;
    bool known = false;
    try {
        known = (co_await journal_->find_order(order_id)).order_known;
    } catch (...) {
        failed_ = true;
        throw;
    }
    if (known)
        throw Error(ErrorCode::conflict, "order ID was already used");
    check_order_source(submission, strategy);
    if (!authorization_)
        throw std::invalid_argument("authorize live trading for this account first");
    const auto [permitted, reconciled, trading_day] = observed([&](const BrokerSnapshot& state) {
        return std::tuple{authorized(state), state.reconciled_for(id), state.trading_day};
    });
    if (!permitted)
        throw Error(
            ErrorCode::unavailable,
            "connect and synchronize the account before sending orders");
    const auto& terms = allowed(id);
    if (offset == Offset::open && !reconciled)
        throw Error(
            ErrorCode::unavailable,
            "broker fills are awaiting position reconciliation; opening orders are paused");
    check_delivery_month(id, offset, trading_day);
    if (order.quantity <= Decimal{} || !order.quantity.multiple_of(terms.quantity_increment))
        throw std::invalid_argument("order quantity must be a positive multiple of the lot size");
    if (order.limit_price <= Decimal{} || !order.limit_price.multiple_of(terms.price_increment))
        throw std::invalid_argument("limit price must be a positive multiple of the price tick");
    const bool check_margin =
        offset == Offset::open && policy_->definition.limits.max_margin_ratio > Decimal{};
    if (check_margin)
        co_await query_margin_rates(id);
    if (strategy && strategy->quote) {
        co_await finish_submit(request, submission, control, strategy->quote, strategy);
        co_return;
    }
    // Waiting for CTP's rate-limited quote yields the active slot so cancels can run.
    deferred_quote_.emplace(
        DeferredQuote{
            trader_->quote(id),
            [this, request, submission, control, strategy](
                std::future<std::optional<BrokerQuote>> ready_quote) {
                return finish_submit_quote(
                    request,
                    submission,
                    control,
                    std::move(ready_quote),
                    strategy);
            }
        });
}

AccountCommand LiveAccountState::finish_submit_quote(
    AccountRequest request,
    SubmitOrder submission,
    std::uint64_t control,
    std::future<std::optional<BrokerQuote>> ready_quote,
    std::optional<StrategyOrderBasis> strategy)
{
    try {
        const auto quote = ready_quote.get();
        co_await finish_submit(request, submission, control, quote, strategy);
    } catch (const std::invalid_argument& error) {
        if (strategy && strategy_ && strategy_->authority == strategy->authority)
            fail_strategy(error.what());
        throw;
    }
}

AccountCommand LiveAccountState::finish_submit(
    const AccountRequest& request,
    const SubmitOrder& submission,
    std::uint64_t control,
    const std::optional<BrokerQuote>& quote,
    std::optional<StrategyOrderBasis> strategy)
{
    const auto& order = submission.order;
    const auto& order_id = order.id;
    const auto& id = order.instrument;
    const auto offset = submission.offset;
    const bool check_margin =
        offset == Offset::open && policy_->definition.limits.max_margin_ratio > Decimal{};
    if (check_margin)
        co_await MarginDataWait{*this, {}};
    check_order_source(submission, strategy);
    const auto terms = allowed(id);
    const auto state = trader_->snapshot();
    if (!authorized(state))
        throw Error(ErrorCode::unavailable, "CTP trading session changed; the order was not sent");
    if (offset == Offset::open && !state.reconciled_for(id))
        throw Error(
            ErrorCode::unavailable,
            "broker fills are awaiting position reconciliation; opening orders are paused");
    check_price(order, quote, state);
    if (quote && quote->last && *quote->last > Decimal{}
        && quote->trading_day == state.trading_day) {
        market_marks_[id] = {quote->trading_day, *quote->last};
        ++close_state_revision_;
    }
    if (offset != Offset::open)
        check_closable_lots(state, order, offset);
    check_delivery_month(id, offset, state.trading_day);
    check_self_trade(state, order);
    check_pre_trade_risk(state, terms, order, offset, check_margin);
    if (control != send_gate_.revision())
        throw Error(
            ErrorCode::conflict,
            "order execution basis changed before durable submission; the order was not sent");
    // The order is recorded durably before the trader sends anything.
    const auto deadline = strategy
        ? std::min(strategy->expires_at, quote->completed_at + quote_validity)
        : quote->completed_at + quote_validity;
    auto prepared = trader_->prepare(
        order,
        offset,
        state.connection_generation,
        state.exposure_revision,
        deadline);
    const auto& prepared_order = prepared->order();
    trader_->observe([&](const BrokerSnapshot& now) {
        check_price(order, quote, now);
    });
    check_order_source(submission, strategy);
    co_await append(
        submission_entry(request, policy_->revision, prepared_order.broker_key, state.trading_day));
    const auto sequence = capacity_.total_records - 1;
    order_origins_[order_id] = {dashed(state.trading_day), id};
    intents_[order_id] = {
        prepared_order.broker_key,
        state.trading_day,
        id,
        order.side,
        offset,
        order.quantity,
        order.limit_price,
        std::nullopt,
        submission.strategy
    };
    // Only positive local evidence can release the recorded intent. If recording
    // it fails, recovery retains the intent as an unknown order.
    const auto not_sent = [&](int code) -> AccountCommand {
        co_await append(entry(OrderNotSent{order_id, code}));
        intents_.erase(order_id);
        order_origins_.erase(order_id);
    };
    std::exception_ptr invalid_source;
    try {
        check_order_source(submission, strategy);
    } catch (...) {
        invalid_source = std::current_exception();
    }
    if (invalid_source) {
        co_await not_sent(broker_code::permit_refused);
        std::rethrow_exception(invalid_source);
    }
    if (control != send_gate_.revision() || !observed([&](const BrokerSnapshot& now) {
            return authorized(now);
        })) {
        co_await not_sent(broker_code::permit_refused);
        throw Error(
            ErrorCode::conflict,
            "order execution basis changed during durable submission; the order was not sent");
    }
    std::optional<BrokerSendPermit> permit;
    try {
        permit.emplace(send_gate_.issue(control, order_id));
    } catch (const Error&) {
        // An ingress producer can invalidate between the last check and this CAS.
    }
    if (!permit) {
        co_await not_sent(broker_code::permit_refused);
        throw Error(
            ErrorCode::conflict,
            "order permission changed before dispatch; the order was not sent");
    }
    const auto dispatch_generation = state.connection_generation;
    recent_dispatches_[order_id] = {
        dispatch_generation,
        steady_now(*clock_) + std::chrono::seconds{5}
    };
    const auto result =
        co_await wait_sdk(trader_->dispatch(std::move(prepared), std::move(*permit), sequence));
    if (!result.invoked) {
        recent_dispatches_.erase(order_id);
        co_await not_sent(result.code);
        if (strategy && result.code == broker_code::exposure_changed)
            throw Error(
                ErrorCode::unavailable,
                "broker exposure changed; evaluate pre-trade risk again");
        if (strategy && result.code == broker_code::permit_refused)
            throw Error(
                ErrorCode::conflict,
                "order permission changed before dispatch; the order was not sent");
        throw Error(
            ErrorCode::conflict,
            "order dispatch was refused before the SDK call; the order was not sent");
    }
    if (result.code) {
        recent_dispatches_.erase(order_id);
        if (!observed([&](const BrokerSnapshot& now) {
                return std::ranges::any_of(now.orders, [&](const BrokerOrder& value) {
                    return value.order_id == order_id;
                });
            })) {
            // CTP sends nothing it refuses: -1 no connection, -2 too many
            // pending requests, -3 over the counter's request rate.
            co_await not_sent(result.code);
            throw Error(
                ErrorCode::unavailable,
                "CTP API refused to send the order; the order was not sent");
        }
    }
}

void LiveAccountState::poll_broker()
{
    if (failed_) {
        fail_strategy("account journal needs recovery");
        if (alerts_)
            alerts_->notify(
                "recovery_required",
                "recovery_required",
                "trading journal requires operator recovery");
    }
    trader_->poll();
    std::vector<std::pair<InstrumentId, std::string>> contract_abandons;
    trader_->observe([&](const BrokerSnapshot& state) {
        if (state.phase == BrokerPhase::ready) {
            was_ready_ = true;
            if (alerts_)
                alerts_->clear("broker_disconnected");
        } else if (alerts_
            && ((was_ready_ && state.phase != BrokerPhase::ready)
                || state.phase == BrokerPhase::error)) {
            alerts_->notify(
                "broker_disconnected",
                "broker_disconnected",
                "broker connection interrupted",
                {{"phase", static_cast<int>(state.phase)}, {"error_code", state.error_code}});
        }
        if (strategy_
            && (state.phase != BrokerPhase::ready
                || state.connection_generation != strategy_->connection_generation)) {
            abandon_strategy_batch("account connection changed");
            if (state.phase == BrokerPhase::ready)
                strategy_->connection_generation = state.connection_generation;
        }
        if (strategy_ && strategy_->batch) {
            const auto now = steady_now(*clock_);
            for (auto it = strategy_->batch->deadlines.begin();
                it != strategy_->batch->deadlines.end();) {
                if (now < it->second) {
                    ++it;
                    continue;
                }
                const auto& id = it->first;
                bool settled = false;
                if (!strategy_->pending_weights.contains(id) && state.reconciled_for(id)
                    && std::ranges::none_of(state.orders, [&](const BrokerOrder& o) {
                           return working(o.status) && o.instrument == id;
                       })) {
                    const auto target_it = std::ranges::find_if(
                        strategy_->batch->targets,
                        [&](const PositionTarget& t) {
                            return t.instrument == id;
                        });
                    if (target_it != strategy_->batch->targets.end()) {
                        Decimal long_lots{}, short_lots{};
                        for (const auto& pos : state.positions) {
                            if (pos.instrument != id)
                                continue;
                            const auto qty = pos.today + pos.yesterday;
                            if (pos.side == Side::buy)
                                long_lots = long_lots + qty;
                            else
                                short_lots = short_lots + qty;
                        }
                        const auto desired_long = std::max(target_it->quantity, Decimal{});
                        const auto desired_short =
                            std::max(Decimal{} - target_it->quantity, Decimal{});
                        settled = strategy_->paused
                            ? (long_lots <= desired_long && short_lots <= desired_short)
                            : (long_lots == desired_long && short_lots == desired_short);
                    }
                }
                if (settled) {
                    it = strategy_->batch->deadlines.erase(it);
                } else {
                    contract_abandons.emplace_back(id, "strategy target expired before completion");
                    ++it;
                }
            }
        }
        if (attribution_.generation != state.connection_generation
            || attribution_.day != state.trading_day)
            attribution_ = state.phase == BrokerPhase::ready
                ? Attribution{state.trading_day, state.connection_generation}
                : Attribution{};
        if (!identities_ready(state))
            return;
        if (alerts_) {
            const auto now = steady_now(*clock_);
            for (const auto& [unconfirmed_id, intent] : unconfirmed(state)) {
                if (const auto recent = recent_dispatches_.find(unconfirmed_id);
                    recent != recent_dispatches_.end()
                    && recent->second.first == state.connection_generation
                    && now < recent->second.second)
                    continue;
                alerts_->notify(
                    "unconfirmed_order",
                    "unconfirmed_order:" + intent->trading_day + ":" + unconfirmed_id,
                    intent->instrument.symbol + " order " + unconfirmed_id
                        + " is unconfirmed at broker",
                    {{"order_id", unconfirmed_id},
                        {"trading_day", intent->trading_day},
                        {"symbol", intent->instrument.symbol}});
            }
        }
        for (const auto& order : state.orders) {
            if (alerts_ && order.status == BrokerOrderStatus::rejected) {
                const auto& id = order.order_id.empty() ? order.broker_key : order.order_id;
                alerts_->notify(
                    "order_rejected",
                    "order_rejected:" + state.trading_day + ":" + id,
                    order.instrument.symbol + " order " + id + " rejected",
                    {{"order_id", order.order_id},
                        {"broker_key", order.broker_key},
                        {"symbol", order.instrument.symbol},
                        {"error_code", order.error_code}});
            }
            if (working(order.status))
                continue;
            const auto intent = intents_.find(order.order_id);
            if (intent != intents_.end() && !intent->second.terminal
                && intent->second.trading_day == state.trading_day
                && intent->second.broker_key == order.broker_key) {
                intent->second.terminal = order;
                if (strategy_ && intent->second.strategy
                    && intent->second.strategy->instance == strategy_->authority->instance()
                    && strategy_->batch
                    && (order.status == BrokerOrderStatus::rejected
                        || order.status == BrokerOrderStatus::cancelled)) {
                    const auto active = strategy_->batch->active_orders.find(order.instrument);
                    if (active != strategy_->batch->active_orders.end()
                        && active->second == order.order_id) {
                        if (order.status == BrokerOrderStatus::cancelled
                            && strategy_->batch->reprice_cancelling.erase(order.instrument) > 0) {
                            if (order.filled > Decimal{}) {
                                if (auto tw = strategy_->batch->twap.find(order.instrument);
                                    tw != strategy_->batch->twap.end() && tw->second.slice_active) {
                                    tw->second.slice_remaining =
                                        tw->second.slice_remaining > order.filled
                                        ? tw->second.slice_remaining - order.filled
                                        : Decimal{};
                                    if (tw->second.slice_remaining <= Decimal{}) {
                                        tw->second.slice_active = false;
                                        ++tw->second.slices_done;
                                        tw->second.next_slice_at = steady_now(*clock_)
                                            + std::chrono::milliseconds(
                                                strategy_->execution.twap_interval_ms);
                                    }
                                }
                            }
                            strategy_->batch->active_order_prices.erase(order.instrument);
                            strategy_->batch->active_order_sides.erase(order.instrument);
                            strategy_->batch->active_order_sent_at.erase(order.instrument);
                            strategy_->batch->active_orders.erase(active);
                        } else if (strategy_->batch->flattening
                            && order.status == BrokerOrderStatus::cancelled
                            && order.filled > Decimal{}
                            && strategy_->batch->flatten_retries[order.instrument] < 2) {
                            ++strategy_->batch->flatten_retries[order.instrument];
                            strategy_->batch->active_order_prices.erase(order.instrument);
                            strategy_->batch->active_order_sides.erase(order.instrument);
                            strategy_->batch->active_order_sent_at.erase(order.instrument);
                            strategy_->batch->active_orders.erase(active);
                        } else {
                            contract_abandons.emplace_back(
                                order.instrument,
                                order.status == BrokerOrderStatus::rejected
                                    ? "broker rejected a strategy order"
                                    : "strategy order was cancelled before completion");
                        }
                    }
                }
            }
        }
        evaluate_risk_alerts(state);
    });
    for (const auto& [contract, reason] : contract_abandons)
        abandon_strategy_contract(contract, reason);
}

void LiveAccountState::check_terminal(const TerminalOrder& order, const Intent& intent)
{
    const bool final = order.status == BrokerOrderStatus::filled
        || order.status == BrokerOrderStatus::cancelled
        || order.status == BrokerOrderStatus::rejected;
    if (order.broker_key != intent.broker_key || order.trading_day != intent.trading_day || !final
        || order.filled < Decimal{} || order.filled > intent.quantity
        || (order.status == BrokerOrderStatus::filled && order.filled != intent.quantity)
        || (order.status == BrokerOrderStatus::rejected && order.filled != Decimal{}))
        throw std::invalid_argument("invalid persisted terminal order evidence");
}

void LiveAccountState::record_trade(::trading::Trade trade)
{
    const auto& day = trade.trading_day();
    const auto& id = trade.id();
    const auto side = trade.side() == ::common::SIDE_SELL ? Side::sell : Side::buy;
    auto& list = trades_[day];
    auto [it, inserted] = recorded_ids_[day].try_emplace(
        std::pair{id, side},
        RecordedTrade{trade.order_id(), list.size()});
    if (inserted) {
        list.push_back(std::move(trade));
    } else {
        it->second.order_id = trade.order_id();
        list[it->second.index] = std::move(trade);
    }
}

std::vector<::trading::Trade> LiveAccountState::unrecorded() const
{
    return observed([&](const BrokerSnapshot& state) {
        if (!identities_ready(state))
            return std::vector<::trading::Trade>{};
        const std::pair basis{state.connection_generation, state.sequence};
        if (unrecorded_clean_basis_ == basis)
            return std::vector<::trading::Trade>{};
        const std::pair list{state.connection_generation, state.trading_day};
        if (list != recorded_list_ || recorded_through_ > state.trades.size()) {
            recorded_list_ = list;
            recorded_through_ = 0;
        }
        std::vector<::trading::Trade> result;
        for (auto i = recorded_through_; i < state.trades.size(); ++i) {
            const auto& trade = state.trades[i];
            const auto day = recorded_ids_.find(dashed(trade.trading_day));
            if (day != recorded_ids_.end()) {
                const auto found = day->second.find({trade.trade_id, trade.side});
                if (found != day->second.end()
                    && (found->second.order_id == trade.order_id || trade.order_id.empty())) {
                    if (result.empty() && !trade.order_id.empty())
                        recorded_through_ = i + 1;
                    continue;
                }
            }
            // A trade reported before its order waits for that order to say whose it is.
            if (trade.order_id.empty()
                && std::ranges::none_of(state.orders, [&](const BrokerOrder& order) {
                       return order.exchange_order_id == trade.exchange_order_id;
                   }))
                continue;
            result.push_back(wire_trade(trade));
            if (result.size() == TradesRecorded::batch)
                break;
        }
        if (result.empty())
            unrecorded_clean_basis_ = basis;
        else
            unrecorded_clean_basis_.reset();
        return result;
    });
}

void LiveAccountState::attach_cancels(::trading::DayClose& close, const BrokerSnapshot* state) const
{
    std::map<InstrumentId, CancelCount> counts;
    for (const auto& item : close.cancels()) {
        auto& row = counts[{item.instrument().exchange(), item.instrument().symbol()}];
        row.cancels = std::max(row.cancels, item.cancels());
        row.rejected_cancels = std::max(row.rejected_cancels, item.rejected_cancels());
    }
    if (const auto recorded = cancels_.find(close.trading_day()); recorded != cancels_.end())
        for (const auto& [instrument, item] : recorded->second) {
            auto& row = counts[instrument];
            row.cancels = std::max(row.cancels, item.cancels);
            row.rejected_cancels = std::max(row.rejected_cancels, item.rejected_cancels);
        }
    if (state && dashed(state->trading_day) == close.trading_day()) {
        for (const auto& order : state->orders) {
            if (order.order_id.empty() && order.status == BrokerOrderStatus::cancelled)
                ++counts[order.instrument].cancels;
            else if (!order.order_id.empty() && cancel_awaited_.contains(order.order_id)
                && working(order.status) && order.error_code != 0)
                ++counts[order.instrument].rejected_cancels;
        }
    }
    close.clear_cancels();
    for (const auto& [instrument, item] : counts) {
        if (!item.cancels && !item.rejected_cancels)
            continue;
        auto* out = close.add_cancels();
        put(out->mutable_instrument(), instrument);
        out->set_cancels(item.cancels);
        out->set_rejected_cancels(item.rejected_cancels);
    }
}

void LiveAccountState::observe_quotes(std::span<const MarketQuote> quotes)
{
    bool marks_changed = false;
    for (const auto& q : quotes)
        if (q.last && *q.last > Decimal{} && !q.trading_day.empty()) {
            const std::pair mark{q.trading_day, *q.last};
            auto [it, inserted] = market_marks_.try_emplace(q.instrument, mark);
            if (inserted) {
                marks_changed = true;
            } else if (it->second != mark) {
                it->second = mark;
                marks_changed = true;
            }
        }
    if (marks_changed)
        ++close_state_revision_;
    std::optional<::trading::DayClose> to_save;
    trader_->observe([&](const BrokerSnapshot& state) {
        const auto basis =
            std::tuple{state.connection_generation, state.sequence, close_state_revision_};
        const bool basis_changed = last_close_basis_ != basis;
        const auto today = dashed(state.trading_day);
        if (basis_changed) {
            const auto day = latest_.trading_day();
            if (state.funds && state.positions_reconciled && !today.empty() && today >= day) {
                latest_ = close_with_valuation(state);
                attach_cancels(latest_, &state);
                to_save = latest_;
            }
            if (day.empty() || today.empty() || today <= day || closes_.contains(day))
                last_close_basis_ = basis;
        } else if (latest_dirty_
            && steady_now(*clock_) - latest_saved_at_ >= std::chrono::seconds{1}) {
            to_save = latest_;
        }
        if (basis_changed)
            evaluate_risk_alerts(state);
    });
    if (to_save)
        save_latest_close(*to_save);
}

LiveAccountState::Valuation LiveAccountState::valuation(const BrokerSnapshot& state) const
{
    if (!state.funds)
        return {};
    const auto& f = *state.funds;
    Decimal dynamic_equity = f.balance;
    const auto cent = Decimal::parse("0.01");
    for (const auto& pos : state.positions) {
        const auto qty = pos.today + pos.yesterday;
        if (qty <= Decimal{} || pos.cost <= Decimal{})
            continue;
        const auto mark = market_marks_.find(pos.instrument);
        const auto* terms = policy_->find(pos.instrument);
        if (mark == market_marks_.end() || mark->second.first != state.trading_day || !terms)
            continue;
        const auto notional = quantize(
            multiply(qty * terms->instrument.multiplier, mark->second.second, Rounding::half_up),
            cent,
            Rounding::half_up);
        const auto live_profit = pos.side == Side::buy ? notional - pos.cost : pos.cost - notional;
        dynamic_equity = dynamic_equity + (live_profit - pos.profit);
    }
    const auto day_baseline = f.pre_balance + f.deposit - f.withdraw;
    const auto daily_loss =
        day_baseline > dynamic_equity ? day_baseline - dynamic_equity : Decimal{};
    const auto today = dashed(state.trading_day);
    const auto net_cashflow = f.deposit - f.withdraw;
    const auto pre_cashflow_equity = dynamic_equity - net_cashflow;
    if (!today.empty()) {
        if (peak_day_ == today) {
            pre_cashflow_peak_ = std::max(pre_cashflow_peak_, pre_cashflow_equity);
        } else if (peak_day_.empty() || today > peak_day_) {
            Decimal prior_peak{};
            if (!peak_day_.empty()) {
                prior_peak = last_effective_peak_;
            } else {
                const auto parse_opt = [](const ::common::Decimal& d) {
                    return d.text().empty() ? Decimal{} : Decimal::parse(d.text());
                };
                const ::trading::DayClose* saved = nullptr;
                if (!latest_.trading_day().empty() && latest_.has_funds()
                    && latest_.funds().has_peak_equity()) {
                    saved = &latest_;
                } else if (!closes_.empty() && closes_.rbegin()->second.has_funds()
                    && closes_.rbegin()->second.funds().has_peak_equity()) {
                    saved = &closes_.rbegin()->second;
                }
                if (saved) {
                    if (saved->trading_day() == today) {
                        prior_peak = parse_opt(saved->funds().peak_equity())
                            - (parse_opt(saved->funds().deposit())
                                - parse_opt(saved->funds().withdraw()));
                    } else if (saved->trading_day() < today) {
                        prior_peak = parse_opt(saved->funds().peak_equity());
                    }
                }
            }
            peak_day_ = today;
            pre_cashflow_peak_ = std::max({prior_peak, f.pre_balance, pre_cashflow_equity});
        }
    }
    const auto effective_peak =
        today.empty() ? std::max(f.pre_balance, dynamic_equity) : pre_cashflow_peak_ + net_cashflow;
    if (!today.empty())
        last_effective_peak_ = effective_peak;
    const auto drawdown =
        effective_peak > dynamic_equity ? effective_peak - dynamic_equity : Decimal{};
    return {dynamic_equity, effective_peak, daily_loss, drawdown};
}

::trading::DayClose LiveAccountState::close_with_valuation(const BrokerSnapshot& state) const
{
    auto close = close_of(state);
    if (state.funds) {
        const auto val = valuation(state);
        put(close.mutable_funds()->mutable_dynamic_equity(), val.dynamic_equity);
        put(close.mutable_funds()->mutable_peak_equity(), val.effective_peak);
    }
    return close;
}

std::vector<std::string>
LiveAccountState::active_risk_breaches(const BrokerSnapshot& state, const Valuation& val) const
{
    std::vector<std::string> breaches;
    const auto& lim = policy_->definition.limits;
    const auto today = dashed(state.trading_day);
    if (state.funds && state.phase == BrokerPhase::ready) {
        if (lim.max_daily_loss > Decimal{} && val.daily_loss >= lim.max_daily_loss)
            breaches.emplace_back("daily_loss");
        if (lim.max_drawdown > Decimal{} && val.drawdown >= lim.max_drawdown)
            breaches.emplace_back("drawdown");
        if (lim.max_margin_ratio > Decimal{}
            && (val.dynamic_equity <= Decimal{}
                || state.funds->margin + state.funds->frozen_margin
                    > multiply(val.dynamic_equity, lim.max_margin_ratio, Rounding::toward_zero)))
            breaches.emplace_back("margin_ratio");
    }
    if (lim.max_daily_cancels_per_contract > 0 && !today.empty()) {
        std::map<InstrumentId, std::uint32_t> day_cancels;
        if (const auto day_it = cancels_.find(today); day_it != cancels_.end())
            for (const auto& [inst, count] : day_it->second)
                day_cancels[inst] += count.cancels;
        for (const auto& order : state.orders)
            if (order.order_id.empty() && order.status == BrokerOrderStatus::cancelled)
                ++day_cancels[order.instrument];
        for (const auto& [inst, cancels] : day_cancels)
            if (cancels >= lim.max_daily_cancels_per_contract)
                breaches.push_back("daily_cancels:" + inst.venue + "." + inst.symbol);
    }
    return breaches;
}

void LiveAccountState::evaluate_risk_alerts(const BrokerSnapshot& state) const
{
    if (!alerts_ || state.trading_day.empty())
        return;
    const auto val = valuation(state);
    const auto breaches = active_risk_breaches(state, val);
    const auto has = [&](std::string_view code) {
        return std::ranges::find(breaches, code) != breaches.end();
    };
    if (state.funds && state.phase == BrokerPhase::ready) {
        const auto loss_key = "risk_daily_loss:" + state.trading_day + ":" + policy_->revision;
        if (has("daily_loss"))
            alerts_->notify(
                "risk_limit_breached",
                loss_key,
                "daily loss limit breached: " + val.daily_loss.str(),
                {{"reason", "daily_loss"},
                    {"daily_loss", val.daily_loss.str()},
                    {"dynamic_equity", val.dynamic_equity.str()},
                    {"trading_day", state.trading_day}});
        else
            alerts_->clear(loss_key);

        const auto dd_key = "risk_drawdown:" + policy_->revision;
        if (has("drawdown"))
            alerts_->notify(
                "risk_limit_breached",
                dd_key,
                "max drawdown limit breached: " + val.drawdown.str(),
                {{"reason", "drawdown"},
                    {"drawdown", val.drawdown.str()},
                    {"dynamic_equity", val.dynamic_equity.str()},
                    {"peak_equity", val.effective_peak.str()}});
        else
            alerts_->clear(dd_key);

        const auto margin_key = "risk_margin_ratio:" + state.trading_day + ":" + policy_->revision;
        if (has("margin_ratio"))
            alerts_->notify(
                "risk_limit_breached",
                margin_key,
                "margin ratio limit breached",
                {{"reason", "margin_ratio"},
                    {"margin", state.funds->margin.str()},
                    {"frozen_margin", state.funds->frozen_margin.str()},
                    {"dynamic_equity", val.dynamic_equity.str()},
                    {"trading_day", state.trading_day}});
        else
            alerts_->clear(margin_key);
    }

    for (const auto& breach : breaches) {
        if (!breach.starts_with("daily_cancels:"))
            continue;
        const auto inst = breach.substr(std::string_view("daily_cancels:").size());
        alerts_->notify(
            "risk_limit_breached",
            "risk_daily_cancels:" + state.trading_day + ":" + inst + ":" + policy_->revision,
            inst + " daily cancel limit breached",
            {{"reason", "daily_cancels"}, {"contract", inst}, {"trading_day", state.trading_day}});
    }
}

void LiveAccountState::save_latest_close(const ::trading::DayClose& close, bool force) const
{
    const auto now = steady_now(*clock_);
    const bool immediate = force || close.trading_day() != latest_saved_day_;
    if (!immediate && latest_saved_at_ != std::chrono::steady_clock::time_point{}
        && now - latest_saved_at_ < std::chrono::seconds{1}) {
        latest_dirty_ = true;
        return;
    }
    const auto bytes = close.SerializeAsString();
    if (bytes == latest_close_bytes_) {
        latest_dirty_ = false;
        return;
    }
    try {
        create_directories_durably(directory_ / "ctp-flow");
        replace_file_durably(directory_ / "ctp-flow/latest_close.pb", bytes);
        latest_close_bytes_ = bytes;
        latest_saved_at_ = now;
        latest_saved_day_ = close.trading_day();
        latest_dirty_ = false;
    } catch (const std::exception& error) {
        log::warn("latest close snapshot could not be saved: {}", error.what());
    }
}

std::optional<::trading::DayClose> LiveAccountState::unclosed() const
{
    std::optional<::trading::DayClose> to_save;
    auto closing = observed([&](const BrokerSnapshot& state) -> std::optional<::trading::DayClose> {
        const auto basis =
            std::tuple{state.connection_generation, state.sequence, close_state_revision_};
        if (last_close_basis_ == basis) {
            if (latest_dirty_ && steady_now(*clock_) - latest_saved_at_ >= std::chrono::seconds{1})
                to_save = latest_;
            return std::nullopt;
        }
        const auto day = latest_.trading_day();
        const auto today = dashed(state.trading_day);
        if (!day.empty() && !today.empty() && today > day && !closes_.contains(day)) {
            auto result = latest_;
            attach_cancels(result);
            return result;
        }
        if (state.funds && state.positions_reconciled && !today.empty() && today >= day) {
            latest_ = close_with_valuation(state);
            attach_cancels(latest_, &state);
            to_save = latest_;
        }
        last_close_basis_ = basis;
        return std::nullopt;
    });
    if (to_save)
        save_latest_close(*to_save);
    return closing;
}

::trading::Orders LiveAccountState::orders(std::uint32_t offset, std::uint32_t limit) const
{
    return observed([&](const BrokerSnapshot& state) {
        ::trading::Orders result;
        result.set_total(state.orders.size());
        const auto end = std::min(state.orders.size(), std::size_t{offset} + limit);
        for (std::size_t index = offset; index < end; ++index)
            *result.add_orders() = wire_order(state.orders[state.orders.size() - index - 1]);
        return result;
    });
}

::trading::Day LiveAccountState::day(
    const std::string& trading_day,
    std::uint32_t offset,
    std::uint32_t limit,
    bool oldest_first) const
{
    ::trading::Day result;
    const auto active_day = observed([](const BrokerSnapshot& state) {
        return state.phase == BrokerPhase::disconnected ? std::string{} : dashed(state.trading_day);
    });
    std::set<std::string> days;
    for (const auto& [day, _] : trades_)
        days.insert(day);
    for (const auto& [day, _] : closes_)
        days.insert(day);
    for (const auto& [day, _] : cancels_)
        days.insert(day);
    if (!latest_.trading_day().empty() && latest_.trading_day() != active_day)
        days.insert(latest_.trading_day());
    for (const auto& day : days)
        result.add_trading_days(day);
    if (trading_day.empty()) {
        std::optional<::trading::DayClose> to_save;
        trader_->observe([&](const BrokerSnapshot& state) {
            result.set_total(state.trades.size());
            const auto end = std::min(state.trades.size(), std::size_t{offset} + limit);
            for (std::size_t index = offset; index < end; ++index)
                *result.add_trades() = wire_trade(state.trades[state.trades.size() - index - 1]);
            const auto today = dashed(state.trading_day);
            const auto basis =
                std::tuple{state.connection_generation, state.sequence, close_state_revision_};
            if (last_close_basis_ != basis && state.funds && state.positions_reconciled
                && !today.empty() && today >= latest_.trading_day()) {
                const auto prev_day = latest_.trading_day();
                latest_ = close_with_valuation(state);
                attach_cancels(latest_, &state);
                to_save = latest_;
                if (prev_day.empty() || today <= prev_day || closes_.contains(prev_day))
                    last_close_basis_ = basis;
            }
            if (!today.empty() && latest_.trading_day() == today) {
                *result.mutable_close() = latest_;
                attach_cancels(*result.mutable_close(), &state);
            } else if (!today.empty() && cancels_.contains(today)) {
                result.mutable_close()->set_trading_day(today);
                attach_cancels(*result.mutable_close(), &state);
            }
        });
        if (to_save)
            save_latest_close(*to_save);
    } else {
        if (const auto found = trades_.find(trading_day); found != trades_.end()) {
            const auto& trades = found->second;
            result.set_total(trades.size());
            const auto end = std::min(trades.size(), std::size_t{offset} + limit);
            for (std::size_t index = offset; index < end; ++index)
                *result.add_trades() = trades[oldest_first ? index : trades.size() - index - 1];
        }
        if (const auto found = closes_.find(trading_day); found != closes_.end()) {
            *result.mutable_close() = found->second;
            attach_cancels(*result.mutable_close());
        } else if (oldest_first && active_day == trading_day) {
            trader_->observe([&](const BrokerSnapshot& state) {
                if (dashed(state.trading_day) == trading_day) {
                    const auto ids = recorded_ids_.find(trading_day);
                    if (offset == 0 && std::ranges::any_of(state.trades, [&](const auto& trade) {
                            return ids == recorded_ids_.end()
                                || !ids->second.contains({trade.trade_id, trade.side});
                        }))
                        throw Error(
                            ErrorCode::conflict,
                            "waiting for today's fills to be recorded");
                    if (state.funds && state.positions_reconciled)
                        *result.mutable_close() = close_with_valuation(state);
                    else if (latest_.trading_day() == trading_day)
                        *result.mutable_close() = latest_;
                    result.mutable_close()->set_trading_day(trading_day);
                    attach_cancels(*result.mutable_close(), &state);
                }
            });
        } else if (active_day != trading_day) {
            if (latest_.trading_day() == trading_day) {
                *result.mutable_close() = latest_;
                attach_cancels(*result.mutable_close());
            } else if (cancels_.contains(trading_day)) {
                result.mutable_close()->set_trading_day(trading_day);
                attach_cancels(*result.mutable_close());
            }
        }
    }
    return result;
}

bool LiveAccountState::storage_work_pending() const
{
    return !failed_
        && (strategy_cancellation_pending() || !unrecorded().empty() || unclosed()
            || (!attribution_.day.empty() && !attribution_.complete)
            || std::ranges::any_of(intents_, [](const auto& entry) {
                   return bool(entry.second.terminal);
               }));
}

AccountCommand LiveAccountState::advance_storage()
{
    try {
        if (strategy_cancellation_pending()) {
            co_await cancel_strategy_order();
            co_return;
        }
        OrdersTerminal terminal;
        for (const auto& [id, intent] : intents_) {
            if (!intent.terminal)
                continue;
            const auto& order = *intent.terminal;
            TerminalOrder evidence{
                id,
                intent.broker_key,
                intent.trading_day,
                order.status,
                order.filled,
                order.exchange_order_id,
                order.error_code
            };
            check_terminal(evidence, intent);
            terminal.orders.push_back(std::move(evidence));
            if (terminal.orders.size() == OrdersTerminal::batch)
                break;
        }
        if (!terminal.orders.empty()) {
            co_await append(entry(terminal));
            for (const auto& order : terminal.orders) {
                if (cancel_awaited_.erase(order.order_id)) {
                    if (order.status != BrokerOrderStatus::cancelled)
                        if (const auto origin = order_origins_.find(order.order_id);
                            origin != order_origins_.end())
                            ++cancels_[origin->second.day][origin->second.instrument]
                                  .rejected_cancels;
                    ++close_state_revision_;
                }
                intents_.erase(order.order_id);
            }
            co_return;
        }
        if (TradesRecorded recorded{unrecorded()}; !recorded.trades.empty()) {
            co_await append(entry(recorded));
            for (auto& trade : recorded.trades)
                record_trade(std::move(trade));
            co_return;
        }
        if (auto close = unclosed()) {
            co_await append(entry(DayClosed{*close}));
            closes_[close->trading_day()] = std::move(*close);
            ++close_state_revision_;
            co_return;
        }
        const auto scope = attribution_;
        auto result = co_await journal_->restore_orders(scope.day, scope.cursor);
        if (attribution_.day != scope.day || attribution_.generation != scope.generation
            || !trader_->restore_orders(scope.day, scope.generation, result.orders))
            co_return;
        attribution_.cursor = result.order_cursor;
        attribution_.complete = result.orders.empty();
    } catch (...) {
        failed_ = true;
        send_gate_.invalidate();
        if (alerts_)
            alerts_->notify(
                "recovery_required",
                "recovery_required",
                "trading journal requires operator recovery");
        throw;
    }
}

std::chrono::steady_clock::time_point LiveAccountState::next_broker_deadline() const
{
    auto deadline = trader_->next_deadline();
    if (strategy_ && strategy_->batch && strategy_->failure.empty()) {
        const auto& batch = *strategy_->batch;
        for (const auto& [contract, dl] : batch.deadlines)
            if (!batch.abandoned.contains(contract))
                deadline = std::min(deadline, dl);
        if (!batch.flattening && strategy_->execution.max_reprices > 0) {
            const auto interval =
                std::chrono::milliseconds(strategy_->execution.reprice_interval_ms);
            for (const auto& [contract, sent_at] : batch.active_order_sent_at) {
                if (batch.abandoned.contains(contract)
                    || batch.reprice_cancelling.contains(contract))
                    continue;
                const auto count_it = batch.reprice_counts.find(contract);
                const auto used = count_it == batch.reprice_counts.end() ? 0U : count_it->second;
                if (used < strategy_->execution.max_reprices)
                    deadline = std::min(deadline, sent_at + interval);
            }
        }
        if (!batch.flattening && strategy_->execution.twap_slices > 1) {
            for (const auto& [contract, tw] : batch.twap)
                if (!batch.abandoned.contains(contract) && !tw.slice_active
                    && tw.next_slice_at != std::chrono::steady_clock::time_point{})
                    deadline = std::min(deadline, tw.next_slice_at);
        }
    }
    return deadline;
}

void LiveAccountState::poll_sdk()
{
    if (sdk_wait_ && sdk_wait_->ready()) {
        const auto command = sdk_wait_->command;
        sdk_wait_.reset();
        command.resume();
    }
}

bool LiveAccountState::business_ready() const
{
    return !failed_ && attribution_.complete && observed([](const BrokerSnapshot& state) {
        return state.phase == BrokerPhase::ready && state.positions_reconciled;
    });
}

bool LiveAccountState::identities_ready(const BrokerSnapshot& state) const
{
    return attribution_.complete && attribution_.generation == state.connection_generation
        && attribution_.day == state.trading_day;
}

bool LiveAccountState::authorized(const BrokerSnapshot& state) const
{
    return authorization_ && !failed_ && state.phase == BrokerPhase::ready
        && identities_ready(state);
}

::trading::Session LiveAccountState::snapshot() const
{
    return observed([&](const BrokerSnapshot& state) {
        log_broker_observations(state);
        ::trading::Session result;
        auto* policy = result.mutable_policy();
        auto* limits = policy->mutable_limits();
        put(limits->mutable_max_order_lots(), policy_->definition.limits.max_order_quantity);
        put(limits->mutable_max_gross_lots(), policy_->definition.limits.max_gross_quantity);
        put(limits->mutable_max_contract_lots(), policy_->definition.limits.max_contract_quantity);
        limits->set_max_working_orders(policy_->definition.limits.max_working_orders);
        put(policy->mutable_max_price_deviation(), policy_->definition.max_price_deviation);
        if (policy_->definition.limits.max_daily_loss > Decimal{})
            put(policy->mutable_max_daily_loss(), policy_->definition.limits.max_daily_loss);
        if (policy_->definition.limits.max_drawdown > Decimal{})
            put(policy->mutable_max_drawdown(), policy_->definition.limits.max_drawdown);
        if (policy_->definition.limits.max_margin_ratio > Decimal{})
            put(policy->mutable_max_margin_ratio(), policy_->definition.limits.max_margin_ratio);
        if (policy_->definition.limits.max_daily_cancels_per_contract > 0)
            policy->set_max_daily_cancels_per_contract(
                policy_->definition.limits.max_daily_cancels_per_contract);
        for (const auto& contract : policy_->definition.contracts) {
            const auto& terms = contract.instrument;
            auto* item = policy->add_contracts();
            put(item->mutable_instrument(), terms.id);
            item->set_product(contract.product);
            item->set_delivery_month(contract.delivery_month);
            put(item->mutable_multiplier(), terms.multiplier);
            put(item->mutable_price_tick(), terms.price_increment);
            put(item->mutable_lot_size(), terms.quantity_increment);
        }
        result.set_policy_revision(policy_->revision);
        const auto phase = state.phase == BrokerPhase::ready && !identities_ready(state)
            ? BrokerPhase::synchronizing
            : state.phase;
        result.set_broker_state(static_cast<::trading::BrokerState>(static_cast<int>(phase) + 1));
        result.set_ctp_error(state.error_code);
        result.set_trading_day(dashed(state.trading_day));
        if (authorization_ && !failed_)
            result.set_authorized_ms(authorization_->authorized_at_ms);
        result.set_positions_reconciled(state.positions_reconciled);
        result.set_recovery_required(failed_);
        const auto val = valuation(state);
        auto close = close_of(state);
        attach_cancels(close, &state);
        *result.mutable_cancels() = std::move(*close.mutable_cancels());
        if (close.has_funds()) {
            put(close.mutable_funds()->mutable_dynamic_equity(), val.dynamic_equity);
            put(close.mutable_funds()->mutable_peak_equity(), val.effective_peak);
            *result.mutable_funds() = std::move(*close.mutable_funds());
        }
        for (const auto& breach : active_risk_breaches(state, val))
            result.add_risk_breaches(breach);
        *result.mutable_positions() = std::move(*close.mutable_positions());
        for (const auto& order : state.orders)
            if (working(order.status))
                *result.add_working_orders() = wire_order(order);
        result.set_order_count(state.orders.size());
        result.set_trade_count(state.trades.size());
        for (const auto& [order, intent] : unconfirmed(state)) {
            auto* item = result.add_unconfirmed();
            item->set_id(order);
            item->set_broker_key(intent->broker_key);
            item->set_trading_day(dashed(intent->trading_day));
        }
        auto* journal = result.mutable_journal();
        journal->set_records(capacity_.records_used);
        journal->set_records_limit(capacity_.records_limit);
        journal->set_bytes(capacity_.bytes_used);
        journal->set_bytes_limit(capacity_.bytes_limit);
        journal->set_segments(capacity_.segment_count);
        for (const auto& costs : state.costs) {
            auto* item = result.add_costs();
            put(item->mutable_instrument(), costs.instrument);
            item->set_state(static_cast<::trading::CostsState>(static_cast<int>(costs.state) + 1));
            item->set_ctp_error(costs.error_code);
            if (costs.costs)
                *item->mutable_costs() = protocol::encode_costs(*costs.costs);
        }
        return result;
    });
}

} // namespace asterion::trading
