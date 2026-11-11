#include "live_account_state.hpp"
#include <asterion/base/id.hpp>
#include <asterion/system/log.hpp>
#include <asterion/system/process/child.hpp>
#include <asterion/futures/portfolio_targets.hpp>
#include <algorithm>
#include <set>

namespace asterion::trading {
namespace {

std::int64_t wall_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::int64_t quote_age_ms(const MarketQuote& quote, std::int64_t wall)
{
    return std::max({std::int64_t{0}, wall - quote.source_ms, wall - quote.received_ms});
}

} // namespace

StrategyAuthorityPtr LiveAccountState::register_strategy(
    std::string instance,
    std::vector<InstrumentId> contracts,
    std::optional<StrategyWeightBudget> budget)
{
    validate_id(instance);
    if (strategy_)
        throw Error(ErrorCode::conflict, "this account already has a strategy execution owner");
    if (!business_ready())
        throw Error(
            ErrorCode::unavailable,
            "synchronize the account before registering a strategy");
    if (contracts.empty() || contracts.size() > 20)
        throw std::invalid_argument("strategy execution requires 1-20 allowed contracts");
    if (budget && (budget->gross < Decimal{} || budget->net < Decimal{}))
        throw std::invalid_argument("strategy weight budgets must be nonnegative");
    std::set<InstrumentId> identities;
    std::vector<std::string> symbols;
    for (const auto& contract : contracts) {
        (void)allowed(contract);
        if (!identities.insert(contract).second)
            throw std::invalid_argument("strategy execution contracts must be unique");
        const auto* terms = policy_->find(contract);
        symbols.push_back(contract.venue + "/" + terms->product + "/" + terms->delivery_month);
    }
    for (const auto& [id, intent] : intents_)
        if (intent.strategy && intent.strategy->instance == instance)
            throw Error(
                ErrorCode::conflict,
                "strategy instance still has unretired orders; use a new instance id");
    StrategyAuthorityPtr authority(new StrategyAuthority(
        std::move(instance),
        std::move(contracts),
        std::move(symbols),
        policy_->revision));
    // Fence manual submissions which were accepted before this claim, including
    // a command currently suspended on its journal or quote.
    send_gate_.invalidate();
    strategy_.emplace(ActiveStrategy{authority, 0, 0, {}, {}, 0});
    strategy_->budget = budget;
    trader_->observe([&](const BrokerSnapshot& state) {
        strategy_->connection_generation = state.connection_generation;
    });
    return authority;
}

void LiveAccountState::accept_strategy_target(
    const StrategyAuthorityPtr& authority,
    ::strategy::Decision decision,
    std::vector<MarketQuote> quotes,
    std::chrono::steady_clock::time_point expires_at)
{
    if (!strategy_ || strategy_->authority != authority)
        throw Error(ErrorCode::conflict, "strategy execution owner no longer matches");
    try {
        const auto now = std::chrono::steady_clock::now();
        if (!decision.sequence() || decision.sequence() <= strategy_->sequence)
            throw Error(ErrorCode::conflict, "strategy target repeats an earlier batch");
        abandon_strategy_batch("superseded by a newer target");
        if (!strategy_->failure.empty())
            throw Error(ErrorCode::conflict, "strategy execution ended: " + strategy_->failure);
        if (decision.targets_size() > 20 || (!decision.ready() && decision.targets_size()))
            throw std::invalid_argument(
                "strategy targets require readiness and at most 20 contracts");
        // Even an empty or warming decision consumes its sequence. It can never
        // be resubmitted later as a fresh executable target.
        strategy_->sequence = decision.sequence();
        strategy_->next_leg = 0;
        strategy_->deadline = expires_at;
        if (!decision.ready())
            strategy_->held.clear();
        if (!decision.targets_size() && strategy_->held.empty())
            return;
        if (expires_at > now + std::chrono::seconds(10))
            throw std::invalid_argument("strategy targets have at most 10 seconds of validity");
        check_strategy({authority, decision.sequence(), 0, decision.ready(), expires_at});
        const auto state = trader_->snapshot();
        if (!authorized(state))
            throw Error(
                ErrorCode::unavailable,
                "authorize and synchronize the account before strategy execution");
        StrategyTargetBatch batch;
        batch.accepted_at = now;
        const auto wall = wall_ms();
        // A contract's fresh quote of this session; its age shortens the batch.
        const auto direct = [&](const InstrumentId& contract) {
            const auto quote = std::ranges::find_if(quotes, [&](const auto& q) {
                return q.instrument == contract;
            });
            if (quote == quotes.end() || quote->trading_day != state.trading_day || !quote->last
                || *quote->last <= Decimal{} || quote->source_ms <= 0 || quote->received_ms <= 0
                || quote->source_ms - wall > 2000 || quote->received_ms > wall
                || quote->source_ms - quote->received_ms > 2000)
                throw Error(
                    ErrorCode::unavailable,
                    "strategy execution quote has invalid identity, date or timestamps");
            const auto age = quote_age_ms(*quote, wall);
            if (age >= 10000)
                throw Error(ErrorCode::unavailable, "strategy execution quote is stale");
            strategy_->deadline =
                std::min(strategy_->deadline, now + std::chrono::milliseconds(10000 - age));
            return *quote;
        };
        if (!decision.targets_size()) {
            // The last ready targets stand: what is left of them is planned again
            // from the positions, as a backtest places it again at each bar.
            for (const auto& target : strategy_->held)
                batch.quotes.push_back(direct(target.instrument));
            batch.targets = strategy_->held;
            batch.sized = true;
            batch.decision = std::move(decision);
            strategy_->batch = std::move(batch);
            return;
        }
        const bool weighted = decision.targets(0).has_weight();
        if (weighted
            && (!strategy_->budget
                || static_cast<std::size_t>(decision.targets_size())
                    != authority->contracts_.size()))
            throw std::invalid_argument(
                "weight targets require explicit budgets and every selected contract");
        std::set<InstrumentId> seen;
        for (const auto& target : decision.targets()) {
            const auto selected = std::ranges::find_if(authority->contracts_, [&](const auto& id) {
                return id.venue == target.instrument().exchange()
                    && id.symbol == target.instrument().symbol();
            });
            if (selected == authority->contracts_.end() || !seen.insert(*selected).second
                || (weighted ? !target.has_weight() : !target.has_lots()))
                throw std::invalid_argument(
                    "strategy targets require unique selected contracts and one value kind");
            batch.quotes.push_back(direct(*selected));
            if (!weighted) {
                const auto quantity = Decimal::parse(std::to_string(target.lots()));
                if (!quantity.multiple_of(allowed(*selected).quantity_increment))
                    throw std::invalid_argument("strategy target is not a whole contract lot");
                batch.targets.push_back({*selected, quantity});
            }
        }
        batch.sized = !weighted;
        batch.decision = std::move(decision);
        if (weighted)
            trader_->refresh_account();
        // Lots stand from here; weights once sized, as a backtest sizes them once.
        strategy_->held = batch.targets;
        strategy_->batch = std::move(batch);
    } catch (const Error& error) {
        if (error.code() == ErrorCode::unavailable && strategy_->failure.empty()) {
            log::warn(
                "strategy target skipped: instance={} sequence={} reason={}",
                authority->instance(),
                decision.sequence(),
                error.what());
            return;
        }
        fail_strategy(error.what());
        throw;
    } catch (const std::exception& error) {
        fail_strategy(error.what());
        throw;
    }
}

AccountCommand LiveAccountState::advance_strategy_target(
    StrategyAuthorityPtr authority,
    std::vector<MarketQuote> quotes)
{
    if (!strategy_ || strategy_->authority != authority)
        throw Error(ErrorCode::conflict, "strategy execution owner no longer matches");
    std::string sent;
    try {
        if (!strategy_->batch)
            co_return;
        // Each leg is priced from the freshest observation of its contract.
        for (auto& held : strategy_->batch->quotes)
            for (const auto& fresh : quotes)
                if (fresh.instrument == held.instrument && fresh.trading_day == held.trading_day
                    && fresh.source_ms > held.source_ms)
                    held = fresh;
        const auto control = send_gate_.revision();
        poll_broker();
        StrategyOrderBasis
            basis{authority, strategy_->sequence, strategy_->next_leg, true, strategy_->deadline};
        check_strategy(basis);
        const auto state = trader_->snapshot();
        if (!authorized(state))
            throw Error(
                ErrorCode::unavailable,
                "strategy account is not authorized and synchronized");
        auto& batch = *strategy_->batch;
        if (!batch.sized) {
            if (!state.funds || state.funds_completed_at < batch.accepted_at
                || !state.positions_reconciled)
                co_return;
            std::vector<WeightedContract> weights;
            for (int i = 0; i < batch.decision.targets_size(); ++i) {
                const auto& quote = batch.quotes[i];
                weights.push_back(
                    {quote.instrument,
                        Decimal::parse(batch.decision.targets(i).weight().text()),
                        *quote.last,
                        allowed(quote.instrument).multiplier});
            }
            // CTP Balance already includes position profit. Freeze this response
            // once for the batch; fills and later funds reports cannot resize it.
            const auto sized = fixed_weight_targets(
                state.funds->balance,
                weights,
                strategy_->budget->gross,
                strategy_->budget->net);
            batch.equity = sized.equity;
            for (const auto& member : sized.members)
                batch.targets.push_back({member.contract.instrument, member.quantity});
            batch.sized = true;
            strategy_->held = batch.targets;
        }
        if (!batch.last_order.empty()) {
            const auto last = std::ranges::find_if(state.orders, [&](const auto& order) {
                return order.order_id == batch.last_order;
            });
            if (last == state.orders.end() || last->status == BrokerOrderStatus::submitted
                || last->status == BrokerOrderStatus::accepted
                || last->status == BrokerOrderStatus::partially_filled)
                co_return;
            if (last->status != BrokerOrderStatus::filled)
                throw Error(
                    ErrorCode::conflict,
                    "strategy order ended without completing its target leg");
        }
        const auto plan = plan_strategy_orders(
            batch.targets,
            policy_->definition.contracts,
            state,
            !unconfirmed(state).empty());
        if (plan.state == StrategyPlanState::settled
            || (strategy_->paused && plan.state == StrategyPlanState::opening)) {
            strategy_->batch.reset();
            co_return;
        }
        if (plan.orders.empty())
            co_return;
        // Above the order limit, the move goes out in whole-lot legs replanned after each fill.
        auto leg = plan.orders.front();
        leg.quantity = std::min(
            leg.quantity,
            quantize(
                policy_->definition.limits.max_order_quantity,
                allowed(leg.instrument).quantity_increment,
                Rounding::floor));
        const auto quote = std::ranges::find_if(batch.quotes, [&](const auto& q) {
            return q.instrument == leg.instrument;
        });
        if (quote == batch.quotes.end())
            throw Error(ErrorCode::unavailable, "no direct quote of " + leg.instrument.symbol);
        const auto price = leg.side == Side::buy ? quote->ask : quote->bid;
        const auto liquidity = leg.side == Side::buy ? quote->ask_quantity : quote->bid_quantity;
        if (!price || *price <= Decimal{} || liquidity <= 0)
            throw Error(
                ErrorCode::unavailable,
                "strategy execution has no usable opposite-side quote");
        basis.quote = BrokerQuote{
            quote->instrument,
            quote->last,
            quote->previous_settlement,
            quote->upper_limit,
            quote->lower_limit,
            quote->trading_day,
            quote->update_time,
            strategy_->connection_generation,
            // A flattening batch carries whatever quotes were cached: age each leg by its own.
            std::chrono::steady_clock::now()
                - std::chrono::milliseconds(quote_age_ms(*quote, wall_ms()))
        };
        // Before admission this is only a plan. A newly received broker event
        // causes a fresh plan on the next turn, never a resend of an order.
        if (control != send_gate_.revision())
            co_return;
        StrategyOrderSource origin{authority->instance(), basis.target_sequence, basis.leg};
        if (batch.equity) {
            const auto index = static_cast<std::size_t>(quote - batch.quotes.begin());
            origin.weight_sizing = StrategyOrderSource::WeightSizing{
                *batch.equity,
                Decimal::parse(batch.decision.targets(static_cast<int>(index)).weight().text()),
                *quote->last,
                batch.targets[index].quantity
            };
        }
        sent = unique_process_id();
        const auto request = AccountRequest::strategy_order(
            {{sent, leg.instrument, leg.side, leg.quantity, *price}, leg.offset},
            origin);
        batch.last_order = sent;
        // Not std::format: macOS 13.0 libc++ lacks the to_chars it instantiates.
        strategy_->events.push_back(
            "sent " + leg.instrument.venue + "/" + leg.instrument.symbol
            + (leg.side == Side::buy ? " buy " : " sell ")
            + (leg.offset == Offset::open ? "open " : "close ") + leg.quantity.str() + " @ "
            + price->str());
        // No references to batch survive this suspension: a stop can release it.
        co_await execute_strategy(basis, request, control);
    } catch (const std::invalid_argument& error) {
        // Invalid targets and risk refusals would repeat on every batch.
        if (strategy_ && strategy_->authority == authority)
            fail_strategy(error.what());
        throw;
    } catch (const std::exception& error) {
        if (!strategy_ || strategy_->authority != authority || !strategy_->failure.empty())
            throw;
        // A replacement batch must survive the old leg's asynchronous failure.
        if (sent.empty() || (strategy_->batch && strategy_->batch->last_order == sent))
            abandon_strategy_batch(error.what());
    }
}

void LiveAccountState::release_strategy(const StrategyAuthorityPtr& authority)
{
    if (!strategy_ || strategy_->authority != authority)
        throw Error(ErrorCode::conflict, "strategy execution owner no longer matches");
    fail_strategy("strategy stopped");
    strategy_.reset();
}

void LiveAccountState::abandon_strategy_batch(std::string_view reason)
{
    if (!strategy_ || !strategy_->batch)
        return;
    log::warn(
        "strategy batch abandoned: instance={} sequence={} reason={}",
        strategy_->authority->instance(),
        strategy_->sequence,
        reason);
    strategy_->batch.reset();
    strategy_->events.push_back("batch abandoned: " + std::string(reason));
    send_gate_.invalidate();
    request_strategy_cancels();
}

void LiveAccountState::pause_strategy(const StrategyAuthorityPtr& authority, bool paused)
{
    if (!strategy_ || strategy_->authority != authority)
        throw Error(ErrorCode::conflict, "strategy execution owner no longer matches");
    strategy_->paused = paused;
    // An opening leg already queued to the SDK would otherwise still use its permit.
    if (paused && strategy_->batch)
        if (const auto leg = intents_.find(strategy_->batch->last_order);
            leg != intents_.end() && leg->second.offset == Offset::open && !leg->second.terminal)
            send_gate_.invalidate();
}

void LiveAccountState::flatten_strategy(
    const StrategyAuthorityPtr& authority,
    std::vector<MarketQuote> quotes)
{
    if (!strategy_ || strategy_->authority != authority)
        throw Error(ErrorCode::conflict, "strategy execution owner no longer matches");
    abandon_strategy_batch("superseded by flattening");
    strategy_->paused = true;
    const auto now = std::chrono::steady_clock::now();
    StrategyTargetBatch batch;
    batch.accepted_at = now;
    batch.quotes = std::move(quotes);
    batch.sized = true;
    for (const auto& contract : authority->contracts_)
        batch.targets.push_back({contract, Decimal{}});
    // Flattening replaces the strategy's targets until it gives new ones.
    strategy_->held = batch.targets;
    strategy_->next_leg = 0;
    strategy_->deadline = now + std::chrono::seconds(10);
    strategy_->batch = std::move(batch);
    strategy_->events.push_back("flattening");
}

std::vector<std::string> LiveAccountState::take_strategy_events(
    const StrategyAuthorityPtr& authority)
{
    if (!strategy_ || strategy_->authority != authority)
        throw Error(ErrorCode::conflict, "strategy execution owner no longer matches");
    return std::exchange(strategy_->events, {});
}

std::string LiveAccountState::strategy_failure(const StrategyAuthorityPtr& authority) const
{
    if (!strategy_ || strategy_->authority != authority)
        throw Error(ErrorCode::conflict, "strategy execution owner no longer matches");
    return strategy_->failure;
}

void LiveAccountState::fail_strategy(std::string reason)
{
    if (!strategy_ || !strategy_->failure.empty())
        return;
    strategy_->failure = std::move(reason);
    send_gate_.invalidate();
    request_strategy_cancels();
}

void LiveAccountState::request_strategy_cancels()
{
    // Mark once: reconnect, another failure or a repeated stop cannot retry a cancel.
    for (auto& [id, intent] : intents_)
        if (intent.strategy && intent.strategy->instance == strategy_->authority->instance()
            && !intent.terminal && !intent.cancel_requested)
            intent.stop_cancel_pending = true;
}

bool LiveAccountState::strategy_cancellation_pending() const
{
    return !failed_ && std::ranges::any_of(intents_, [](const auto& item) {
        return item.second.stop_cancel_pending;
    });
}

AccountCommand LiveAccountState::cancel_strategy_order()
{
    const auto found = std::ranges::find_if(intents_, [](const auto& item) {
        return item.second.stop_cancel_pending;
    });
    if (found == intents_.end())
        co_return;
    const auto id = found->first;
    found->second.stop_cancel_pending = false;
    if (found->second.terminal)
        co_return;
    ::trading::Command command;
    command.set_request_id(unique_process_id());
    command.mutable_cancel_order()->set_id(id);
    const auto request = AccountRequest::from_wire(command);
    try {
        co_await cancel(request, id);
    } catch (const std::exception& error) {
        // No retry, including a disconnect before invocation. The original intent
        // continues to occupy risk until broker evidence or explicit resolution.
        log::error("strategy stop cancellation failed: order_id={} reason={}", id, error.what());
    }
}

void LiveAccountState::require_manual_order() const
{
    if (strategy_)
        throw Error(
            ErrorCode::conflict,
            "manual orders are disabled while a strategy owns this account");
}

void LiveAccountState::check_strategy(const StrategyOrderBasis& basis) const
{
    if (!strategy_ || !basis.authority || strategy_->authority != basis.authority
        || basis.authority->policy_ != policy_->revision)
        throw Error(ErrorCode::conflict, "strategy execution identity or policy changed");
    if (!strategy_->failure.empty())
        throw Error(ErrorCode::conflict, "strategy execution ended: " + strategy_->failure);
    if (!basis.ready)
        throw Error(ErrorCode::conflict, "strategy is not ready to execute");
    if (std::chrono::steady_clock::now() >= basis.expires_at)
        throw Error(ErrorCode::unavailable, "strategy target expired before sending");
}

AccountCommand LiveAccountState::execute_strategy(
    StrategyOrderBasis basis,
    AccountRequest request,
    std::uint64_t control)
{
    if (!strategy_ || !basis.authority || strategy_->authority != basis.authority
        || basis.authority->policy_ != policy_->revision)
        throw Error(ErrorCode::conflict, "strategy execution identity or policy changed");
    // Sequence 0 is a flattening before the instance's first decision.
    if (basis.leg >= StrategyOrderSource::max_legs || basis.target_sequence < strategy_->sequence
        || (basis.target_sequence == strategy_->sequence
            && (basis.leg != strategy_->next_leg || basis.expires_at != strategy_->deadline))
        || (basis.target_sequence > strategy_->sequence && basis.leg != 0))
        throw Error(
            ErrorCode::conflict,
            "strategy target sequence, leg or original deadline does not match");
    // Consume the leg before any suspension. Failure or an unknown result cannot
    // turn this identity back into an executable request.
    strategy_->sequence = basis.target_sequence;
    strategy_->next_leg = basis.leg + 1;
    strategy_->deadline = basis.expires_at;
    try {
        check_strategy(basis);
        if (failed_)
            throw Error(ErrorCode::unavailable, "live trading record needs recovery");
        if (control != send_gate_.revision())
            throw Error(ErrorCode::conflict, "account changed after strategy order admission");
        const auto& order = std::get<SubmitOrder>(request.operation);
        if (std::ranges::find(basis.authority->contracts_, order.order.instrument)
            == basis.authority->contracts_.end())
            throw std::invalid_argument("order contract is not selected by this strategy instance");
        co_await submit(request, order, control, basis);
    } catch (const std::invalid_argument& error) {
        if (strategy_ && strategy_->authority == basis.authority)
            fail_strategy(error.what());
        throw;
    }
}

} // namespace asterion::trading
