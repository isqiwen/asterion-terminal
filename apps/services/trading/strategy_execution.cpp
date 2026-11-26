#include "live_account_state.hpp"
#include <asterion/base/id.hpp>
#include <asterion/system/log.hpp>
#include <asterion/system/process/child.hpp>
#include <asterion/futures/history_identity.hpp>
#include <asterion/futures/portfolio_targets.hpp>
#include <algorithm>
#include <set>

namespace asterion::trading {
namespace {

std::int64_t quote_age_ms(const MarketQuote& quote, std::int64_t wall)
{
    return std::max({std::int64_t{0}, wall - quote.source_ms, wall - quote.received_ms});
}

Decimal abs_decimal(Decimal value)
{
    return value < Decimal{} ? Decimal{} - value : value;
}

std::optional<MarketQuote> usable_quote(
    std::span<const MarketQuote> quotes,
    const InstrumentId& contract,
    std::string_view trading_day,
    std::int64_t wall)
{
    const auto quote = std::ranges::find_if(quotes, [&](const auto& q) {
        return q.instrument == contract;
    });
    if (quote == quotes.end() || quote->trading_day != trading_day || !quote->last
        || *quote->last <= Decimal{} || quote->source_ms <= 0 || quote->received_ms <= 0
        || quote->source_ms - wall > 2000 || quote->received_ms > wall
        || quote->source_ms - quote->received_ms > 2000 || quote_age_ms(*quote, wall) >= 10000)
        return std::nullopt;
    return *quote;
}

Decimal contract_margin_rate(const BrokerSnapshot& state, const InstrumentId& id, Decimal weight)
{
    for (const auto& entry : state.costs)
        if (entry.instrument == id && entry.state == BrokerCostsState::ready && entry.costs)
            return weight < Decimal{} ? entry.costs->short_margin_rate : entry.costs->margin_rate;
    return Decimal{};
}

Decimal net_held_lots(const BrokerSnapshot& state, const InstrumentId& id)
{
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
    if (long_lots > Decimal{} && short_lots > Decimal{})
        return Decimal{};
    return long_lots - short_lots;
}

bool working_order(const BrokerOrder& order)
{
    return order.status == BrokerOrderStatus::submitted
        || order.status == BrokerOrderStatus::accepted
        || order.status == BrokerOrderStatus::partially_filled;
}

bool has_working_order(const BrokerSnapshot& state, const InstrumentId& id)
{
    return std::ranges::any_of(state.orders, [&](const BrokerOrder& order) {
        return working_order(order) && order.instrument == id;
    });
}

bool contract_needs_execution(
    const BrokerSnapshot& state,
    const InstrumentId& id,
    Decimal target_quantity,
    bool paused = false)
{
    if (!state.reconciled_for(id) || has_working_order(state, id))
        return true;
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
    const auto desired_long = std::max(target_quantity, Decimal{});
    const auto desired_short = std::max(Decimal{} - target_quantity, Decimal{});
    if (paused)
        return long_lots > desired_long || short_lots > desired_short;
    return long_lots != desired_long || short_lots != desired_short;
}

bool contract_has_position(const BrokerSnapshot& state, const InstrumentId& id)
{
    return std::ranges::any_of(state.positions, [&](const BrokerPosition& pos) {
        return pos.instrument == id && (pos.today > Decimal{} || pos.yesterday > Decimal{});
    });
}

// The host stamps the time when it records the event.
::trading::InstanceEvent event(::trading::InstanceEventKind kind, std::string text)
{
    ::trading::InstanceEvent result;
    result.set_kind(kind);
    result.set_text(std::move(text));
    return result;
}

std::optional<Decimal> aggressive_limit_price(
    const MarketQuote& uq,
    const Instrument& terms,
    Side side,
    std::uint32_t aggressive_ticks)
{
    const auto base = side == Side::buy ? uq.ask : uq.bid;
    const auto liquidity = side == Side::buy ? uq.ask_quantity : uq.bid_quantity;
    if (!base || *base <= Decimal{} || liquidity <= 0)
        return std::nullopt;
    if (aggressive_ticks == 0)
        return base;
    const auto cushion = terms.price_increment * Decimal::from_integer(aggressive_ticks);
    Decimal candidate = side == Side::buy ? *base + cushion : *base - cushion;
    if (uq.upper_limit && *uq.upper_limit > Decimal{})
        candidate = std::min(candidate, *uq.upper_limit);
    if (uq.lower_limit && *uq.lower_limit > Decimal{})
        candidate = std::max(candidate, *uq.lower_limit);
    if (candidate < terms.price_increment)
        candidate = terms.price_increment;
    candidate = quantize(
        candidate,
        terms.price_increment,
        side == Side::buy ? Rounding::floor : Rounding::ceiling);
    return candidate > Decimal{} ? std::optional{candidate} : std::nullopt;
}

} // namespace

StrategyExecutionOptions normalize_execution_options(StrategyExecutionOptions options)
{
    if (options.twap_slices == 0)
        options.twap_slices = 1;
    if (options.max_reprices > 0 && options.reprice_interval_ms == 0)
        options.reprice_interval_ms = 3000;
    if (options.twap_slices > 1 && options.twap_interval_ms == 0)
        options.twap_interval_ms = 5000;
    if (options.aggressive_ticks > 5)
        throw std::invalid_argument("aggressive_ticks must be between 0 and 5");
    if (options.max_reprices > 5)
        throw std::invalid_argument("max_reprices must be between 0 and 5");
    if (options.max_reprices > 0
        && (options.reprice_interval_ms < 1000 || options.reprice_interval_ms > 30000))
        throw std::invalid_argument("reprice_interval_ms must be between 1000 and 30000");
    if (options.max_reprices == 0 && options.reprice_interval_ms > 30000)
        throw std::invalid_argument("reprice_interval_ms must be at most 30000");
    if (options.twap_slices < 1 || options.twap_slices > 10)
        throw std::invalid_argument("twap_slices must be between 1 and 10");
    if (options.twap_slices > 1
        && (options.twap_interval_ms < 1000 || options.twap_interval_ms > 60000))
        throw std::invalid_argument("twap_interval_ms must be between 1000 and 60000");
    if (options.twap_slices == 1 && options.twap_interval_ms > 60000)
        throw std::invalid_argument("twap_interval_ms must be at most 60000");
    return options;
}

std::chrono::milliseconds strategy_execution_window(const StrategyExecutionOptions& options)
{
    if (options.max_reprices == 0 && options.twap_slices <= 1)
        return std::chrono::milliseconds{10000};
    const auto extra_reprice = static_cast<std::int64_t>(options.max_reprices)
        * (static_cast<std::int64_t>(options.reprice_interval_ms) + 2000);
    const auto extra_twap = static_cast<std::int64_t>(options.twap_slices - 1)
        * (static_cast<std::int64_t>(options.twap_interval_ms) + 5000);
    return std::chrono::milliseconds{
        std::min<std::int64_t>(10000 + extra_reprice + extra_twap, 300000)
    };
}

StrategyAuthorityPtr LiveAccountState::register_strategy(
    std::string instance,
    std::vector<InstrumentId> contracts,
    std::optional<StrategyWeightBudget> budget,
    std::vector<DominantSeriesSpec> series,
    StrategyExecutionOptions execution)
{
    validate_id(instance);
    execution = normalize_execution_options(execution);
    if (strategy_)
        throw Error(ErrorCode::conflict, "this account already has a strategy execution owner");
    if (!business_ready())
        throw Error(
            ErrorCode::unavailable,
            "synchronize the account before registering a strategy");
    const auto total_symbols = contracts.size() + series.size();
    if (total_symbols == 0 || total_symbols > max_portfolio_contracts)
        throw std::invalid_argument("strategy execution requires 1-64 allowed contracts");
    if (budget
        && (budget->gross < Decimal{} || budget->net < Decimal{}
            || budget->max_margin_ratio < Decimal{} || budget->min_rebalance_weight < Decimal{}))
        throw std::invalid_argument("strategy weight budgets must be nonnegative");
    std::set<InstrumentId> identities;
    std::set<std::string> symbol_set;
    std::vector<std::string> symbols;
    for (const auto& contract : contracts) {
        (void)allowed(contract);
        if (!identities.insert(contract).second)
            throw std::invalid_argument("strategy execution contracts must be unique");
        const auto* terms = policy_->find(contract);
        if (terms->delivery_month.empty()) {
            const auto& sym = contract.symbol;
            const auto suffix = sym.substr(terms->product.size());
            const auto ym = contract.venue == "CZCE"
                ? "202" + suffix.substr(0, 1) + "-" + suffix.substr(1, 2)
                : "20" + suffix.substr(0, 2) + "-" + suffix.substr(2, 2);
            symbols.push_back(HistoryIdentity{contract.venue, terms->product, ym}.key());
        } else {
            symbols.push_back(
                HistoryIdentity{contract.venue, terms->product, terms->delivery_month}.key());
        }
        if (!symbol_set.insert(symbols.back()).second)
            throw std::invalid_argument("strategy execution symbols must be unique");
    }
    std::vector<InstrumentId> active_series;
    for (const auto& spec : series) {
        if (spec.symbol.empty() || spec.candidates.empty()
            || spec.initial_index >= spec.candidates.size())
            throw std::invalid_argument("dominant series requires candidate contracts");
        if (!symbol_set.insert(spec.symbol).second)
            throw std::invalid_argument("strategy execution symbols must be unique");
        for (const auto& candidate : spec.candidates)
            if (!identities.insert(candidate).second)
                throw std::invalid_argument("strategy execution contracts must be unique");
        (void)allowed(spec.candidates[spec.initial_index]);
        symbols.push_back(spec.symbol);
        active_series.push_back(spec.candidates[spec.initial_index]);
    }
    for (const auto& [id, intent] : intents_)
        if (intent.strategy && intent.strategy->instance == instance)
            throw Error(
                ErrorCode::conflict,
                "strategy instance still has unretired orders; use a new instance id");
    StrategyAuthorityPtr authority(new StrategyAuthority(
        std::move(instance),
        std::move(contracts),
        std::move(series),
        std::move(symbols),
        policy_->revision));
    // Fence manual submissions which were accepted before this claim, including
    // a command currently suspended on its journal or quote.
    send_gate_.invalidate();
    strategy_.emplace(ActiveStrategy{authority, 0, 0, {}, {}, 0});
    strategy_->budget = budget;
    strategy_->execution = execution;
    strategy_->active_series = std::move(active_series);
    if (budget && budget->max_margin_ratio > Decimal{}) {
        std::vector<std::pair<InstrumentId, std::string>> cost_contracts;
        for (const auto& id : authority->contracts_)
            if (const auto* terms = policy_->find(id))
                cost_contracts.emplace_back(id, terms->product);
        for (const auto& id : strategy_->active_series)
            if (const auto* terms = policy_->find(id))
                cost_contracts.emplace_back(id, terms->product);
        if (!cost_contracts.empty())
            trader_->query_costs(cost_contracts);
    }
    trader_->observe([&](const BrokerSnapshot& state) {
        strategy_->connection_generation = state.connection_generation;
    });
    return authority;
}

void LiveAccountState::roll_strategy_series(
    const StrategyAuthorityPtr& authority,
    std::size_t series_index,
    const InstrumentId& next_contract)
{
    if (!strategy_ || strategy_->authority != authority)
        throw Error(ErrorCode::conflict, "strategy execution owner no longer matches");
    if (series_index >= authority->series_.size())
        throw std::invalid_argument("invalid strategy dominant series index");
    const auto& candidates = authority->series_[series_index].candidates;
    const auto current = std::ranges::find(candidates, strategy_->active_series[series_index]);
    const auto next = std::ranges::find(candidates, next_contract);
    if (next == candidates.end() || next < current)
        throw std::invalid_argument("dominant series cannot roll backward");
    if (next == current)
        return;
    try {
        (void)allowed(next_contract);
    } catch (const std::exception&) {
        const auto reason = "next dominant contract " + next_contract.venue + "/"
            + next_contract.symbol + " is not allowed in account policy";
        fail_strategy(reason);
        throw std::invalid_argument(reason);
    }
    const auto previous = strategy_->active_series[series_index];
    strategy_->active_series[series_index] = next_contract;
    if (strategy_->budget && strategy_->budget->max_margin_ratio > Decimal{})
        if (const auto* terms = policy_->find(next_contract))
            trader_->query_costs({{next_contract, terms->product}});
    auto old_held = std::ranges::find_if(strategy_->held, [&](const auto& item) {
        return item.instrument == previous;
    });
    if (old_held != strategy_->held.end() && old_held->quantity != Decimal{}) {
        const auto carry = old_held->quantity;
        old_held->quantity = Decimal{};
        const auto new_held = std::ranges::find_if(strategy_->held, [&](const auto& item) {
            return item.instrument == next_contract;
        });
        if (new_held == strategy_->held.end())
            strategy_->held.push_back({next_contract, carry});
        else
            new_held->quantity = carry;
    }
    if (const auto w = strategy_->raw_weights.find(previous); w != strategy_->raw_weights.end()) {
        strategy_->raw_weights[next_contract] = w->second;
        strategy_->raw_weights.erase(w);
    }
    if (const auto pw = strategy_->pending_weights.find(previous);
        pw != strategy_->pending_weights.end()) {
        strategy_->pending_weights[next_contract] = pw->second;
        strategy_->pending_weights.erase(pw);
    }
}

void LiveAccountState::accept_strategy_target(
    const StrategyAuthorityPtr& authority,
    ::strategy::Decision decision,
    std::vector<MarketQuote> quotes,
    std::chrono::steady_clock::time_point expires_at)
{
    if (!strategy_ || strategy_->authority != authority)
        throw Error(ErrorCode::conflict, "strategy execution owner no longer matches");
    observe_quotes(quotes);
    try {
        const auto now = steady_now(*clock_);
        if (!decision.sequence() || decision.sequence() <= strategy_->sequence)
            throw Error(ErrorCode::conflict, "strategy target repeats an earlier batch");
        if (strategy_->batch && strategy_->batch->active_orders.empty()
            && strategy_->batch->deadlines.empty())
            strategy_->batch.reset();
        else
            abandon_strategy_batch("superseded by a newer target");
        if (!strategy_->failure.empty())
            throw Error(ErrorCode::conflict, "strategy execution ended: " + strategy_->failure);
        if (static_cast<std::size_t>(decision.targets_size()) > max_portfolio_contracts
            || (!decision.ready() && decision.targets_size()))
            throw std::invalid_argument(
                "strategy targets require readiness and at most 64 contracts");
        // Even an empty or warming decision consumes its sequence. It can never
        // be resubmitted later as a fresh executable target.
        strategy_->sequence = decision.sequence();
        strategy_->next_leg = 0;
        strategy_->deadline = expires_at;
        if (!decision.ready()) {
            strategy_->held.clear();
            strategy_->raw_weights.clear();
            strategy_->pending_weights.clear();
            strategy_->frozen_equity.reset();
            strategy_->frozen_margin_scale = Decimal::parse("1");
            strategy_->sized_prices.clear();
            strategy_->weights_validated = true;
        }
        if (!decision.targets_size() && strategy_->held.empty()
            && strategy_->pending_weights.empty())
            return;
        const auto max_window = strategy_execution_window(strategy_->execution);
        if (expires_at > now + max_window)
            throw std::invalid_argument(
                "strategy targets exceed their permitted execution validity window");
        std::vector<InstrumentId> active_contracts = authority->contracts_;
        active_contracts.insert(
            active_contracts.end(),
            strategy_->active_series.begin(),
            strategy_->active_series.end());
        const bool weighted = decision.targets_size() && decision.targets(0).has_weight();
        if (weighted
            && (!strategy_->budget
                || static_cast<std::size_t>(decision.targets_size()) != active_contracts.size()))
            throw std::invalid_argument(
                "weight targets require explicit budgets and every selected contract");
        auto targets = strategy_->held;
        std::map<InstrumentId, Decimal> parsed_weights;
        std::set<InstrumentId> seen;
        for (const auto& target : decision.targets()) {
            const auto selected = std::ranges::find_if(active_contracts, [&](const auto& id) {
                return id.venue == target.instrument().exchange()
                    && id.symbol == target.instrument().symbol();
            });
            if (selected == active_contracts.end() || !seen.insert(*selected).second
                || (weighted ? !target.has_weight() : !target.has_lots()))
                throw std::invalid_argument(
                    "strategy targets require unique selected contracts and one value kind");
            if (weighted) {
                parsed_weights[*selected] = Decimal::parse(target.weight().text());
            } else {
                const auto quantity = Decimal::from_integer(target.lots());
                if (!quantity.multiple_of(allowed(*selected).quantity_increment))
                    throw std::invalid_argument("strategy target is not a whole contract lot");
                const auto held = std::ranges::find_if(targets, [&](const auto& item) {
                    return item.instrument == *selected;
                });
                if (held == targets.end())
                    targets.push_back({*selected, quantity});
                else
                    held->quantity = quantity;
            }
        }
        // A new intent replaces the old one even when its execution must wait.
        if (decision.targets_size() > 0 && !weighted) {
            strategy_->held = std::move(targets);
            strategy_->raw_weights.clear();
            strategy_->pending_weights.clear();
            strategy_->frozen_equity.reset();
            strategy_->frozen_margin_scale = Decimal::parse("1");
            strategy_->sized_prices.clear();
            strategy_->weights_validated = true;
        } else if (weighted) {
            strategy_->held.clear();
            strategy_->raw_weights = parsed_weights;
            strategy_->pending_weights.clear();
            strategy_->frozen_equity.reset();
            strategy_->frozen_margin_scale = Decimal::parse("1");
            strategy_->sized_prices.clear();
            strategy_->weights_validated = false;
            strategy_->weights_accepted_at = now;
            for (const auto& [id, weight] : parsed_weights) {
                if (weight == Decimal{})
                    strategy_->held.push_back({id, Decimal{}});
                else
                    strategy_->pending_weights[id] = weight;
            }
        }
        check_strategy({authority, decision.sequence(), 0, decision.ready(), expires_at});
        const auto state = trader_->snapshot();
        if (!authorized(state))
            throw Error(
                ErrorCode::unavailable,
                "authorize and synchronize the account before strategy execution");
        const auto inactive_candidate = [&](const InstrumentId& id) {
            if (std::ranges::find(active_contracts, id) != active_contracts.end())
                return false;
            return std::ranges::any_of(authority->series_, [&](const auto& spec) {
                return std::ranges::find(spec.candidates, id) != spec.candidates.end();
            });
        };
        for (const auto& position : state.positions) {
            if ((position.today > Decimal{} || position.yesterday > Decimal{})
                && inactive_candidate(position.instrument)
                && std::ranges::find_if(
                       strategy_->held,
                       [&](const auto& item) {
                           return item.instrument == position.instrument;
                       })
                    == strategy_->held.end())
                strategy_->held.push_back({position.instrument, Decimal{}});
        }
        StrategyTargetBatch batch;
        batch.accepted_at = now;
        batch.targets = strategy_->held;
        const auto wall = utc_now_ms(*clock_);
        std::set<InstrumentId> recorded_quotes;
        const bool awaiting_weights_basis =
            !strategy_->raw_weights.empty() && !strategy_->frozen_equity;
        for (const auto& q : quotes) {
            if (!recorded_quotes.insert(q.instrument).second)
                continue;
            if (const auto uq = usable_quote(quotes, q.instrument, state.trading_day, wall)) {
                batch.quotes.push_back(*uq);
                if (awaiting_weights_basis)
                    continue;
                const auto held = std::ranges::find_if(batch.targets, [&](const auto& item) {
                    return item.instrument == uq->instrument;
                });
                const bool needed = held != batch.targets.end()
                    && contract_needs_execution(
                        state,
                        uq->instrument,
                        held->quantity,
                        strategy_->paused);
                if (needed) {
                    const auto age = quote_age_ms(*uq, wall);
                    batch.deadlines[uq->instrument] =
                        std::min(expires_at, now + max_window - std::chrono::milliseconds(age));
                }
            }
        }
        if (awaiting_weights_basis) {
            trader_->refresh_account();
            if (strategy_->budget && strategy_->budget->max_margin_ratio > Decimal{}) {
                std::vector<std::pair<InstrumentId, std::string>> missing_costs;
                for (const auto& [id, weight] : strategy_->raw_weights) {
                    if (weight == Decimal{})
                        continue;
                    const bool known = std::ranges::any_of(state.costs, [&](const auto& entry) {
                        return entry.instrument == id
                            && (entry.state == BrokerCostsState::ready
                                || entry.state == BrokerCostsState::querying);
                    });
                    if (!known)
                        if (const auto* terms = policy_->find(id))
                            missing_costs.emplace_back(id, terms->product);
                }
                if (!missing_costs.empty())
                    trader_->query_costs(missing_costs);
            }
        }
        batch.decision = std::move(decision);
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
    observe_quotes(quotes);
    std::string sent;
    InstrumentId sent_instrument;
    std::chrono::steady_clock::time_point batch_accepted_at{};
    try {
        if (!strategy_->batch)
            co_return;
        for (const auto& fresh : quotes) {
            auto held = std::ranges::find_if(strategy_->batch->quotes, [&](const auto& q) {
                return q.instrument == fresh.instrument;
            });
            if (held == strategy_->batch->quotes.end())
                strategy_->batch->quotes.push_back(fresh);
            else if (fresh.trading_day > held->trading_day
                || (fresh.trading_day == held->trading_day && fresh.source_ms > held->source_ms))
                *held = fresh;
        }
        const auto control = send_gate_.revision();
        poll_broker();
        if (!strategy_->batch)
            co_return;
        if (authority->policy_ != policy_->revision)
            throw Error(ErrorCode::conflict, "strategy execution identity or policy changed");
        if (!strategy_->failure.empty())
            throw Error(ErrorCode::conflict, "strategy execution ended: " + strategy_->failure);
        const auto state = trader_->snapshot();
        if (!authorized(state))
            throw Error(
                ErrorCode::unavailable,
                "strategy account is not authorized and synchronized");
        auto& batch = *strategy_->batch;
        batch_accepted_at = batch.accepted_at;
        if (!strategy_->weights_validated) {
            Decimal gross{}, net{};
            for (const auto& [_, weight] : strategy_->raw_weights) {
                gross = gross + abs_decimal(weight);
                net = net + weight;
            }
            if (!strategy_->budget || gross > strategy_->budget->gross
                || abs_decimal(net) > strategy_->budget->net)
                throw std::invalid_argument(
                    "fixed-weight targets exceed the declared gross or net nominal budget");
            strategy_->weights_validated = true;
        }
        if (!strategy_->raw_weights.empty() && !strategy_->frozen_equity) {
            if (!state.funds || state.funds_completed_at < strategy_->weights_accepted_at
                || !state.positions_reconciled)
                co_return;
            if (state.funds->balance <= Decimal{})
                throw std::invalid_argument("fixed-weight targets require positive equity");
            if (strategy_->budget->max_margin_ratio > Decimal{}) {
                for (const auto& [id, weight] : strategy_->raw_weights) {
                    if (weight == Decimal{})
                        continue;
                    const auto cost = std::ranges::find_if(state.costs, [&](const auto& entry) {
                        return entry.instrument == id;
                    });
                    if (cost == state.costs.end() || cost->state == BrokerCostsState::querying)
                        co_return;
                    if (cost->state != BrokerCostsState::ready || !cost->costs)
                        throw Error(
                            ErrorCode::unavailable,
                            "strategy execution lacks broker margin rate for " + id.symbol);
                }
            }
            Decimal implied_margin{};
            for (const auto& [id, weight] : strategy_->raw_weights)
                implied_margin = implied_margin
                    + multiply(
                        abs_decimal(weight),
                        contract_margin_rate(state, id, weight),
                        Rounding::half_up);
            Decimal scale = Decimal::parse("1");
            if (strategy_->budget->max_margin_ratio > Decimal{}
                && implied_margin > strategy_->budget->max_margin_ratio)
                scale = divide(
                    strategy_->budget->max_margin_ratio,
                    implied_margin,
                    Rounding::toward_zero);
            // CTP Balance already includes position profit. Freeze this response
            // once for the batch; fills and later funds reports cannot resize it.
            strategy_->frozen_equity = state.funds->balance;
            strategy_->frozen_margin_scale = scale;
        }
        const auto now = steady_now(*clock_);
        const auto wall = utc_now_ms(*clock_);
        if (strategy_->frozen_equity && !strategy_->pending_weights.empty()) {
            std::vector<WeightedContract> ready_weights;
            const auto one = Decimal::parse("1");
            for (const auto& [id, weight] : strategy_->pending_weights) {
                if (batch.abandoned.contains(id))
                    continue;
                if (const auto uq = usable_quote(batch.quotes, id, state.trading_day, wall)) {
                    const auto scaled = strategy_->frozen_margin_scale == one
                        ? weight
                        : multiply(weight, strategy_->frozen_margin_scale, Rounding::toward_zero);
                    ready_weights.push_back(
                        {id,
                            scaled,
                            *uq->last,
                            allowed(id).multiplier,
                            contract_margin_rate(state, id, weight),
                            net_held_lots(state, id)});
                }
            }
            if (!ready_weights.empty()) {
                Decimal remaining_margin_ratio = strategy_->budget->max_margin_ratio;
                if (remaining_margin_ratio > Decimal{}) {
                    const auto cent = Decimal::parse("0.01");
                    Decimal used_margin{};
                    for (const auto& existing : strategy_->held) {
                        if (existing.quantity == Decimal{})
                            continue;
                        const auto p = strategy_->sized_prices.find(existing.instrument);
                        if (p == strategy_->sized_prices.end())
                            continue;
                        const auto notional = abs_decimal(existing.quantity)
                            * (p->second * allowed(existing.instrument).multiplier);
                        const auto w_it = strategy_->raw_weights.find(existing.instrument);
                        const auto w_sign =
                            w_it != strategy_->raw_weights.end() ? w_it->second : Decimal{};
                        used_margin = used_margin
                            + quantize(
                                multiply(
                                    notional,
                                    contract_margin_rate(state, existing.instrument, w_sign),
                                    Rounding::half_up),
                                cent,
                                Rounding::half_up);
                    }
                    const auto total_cap = multiply(
                        *strategy_->frozen_equity,
                        strategy_->budget->max_margin_ratio,
                        Rounding::toward_zero);
                    const auto left_cap =
                        total_cap > used_margin ? total_cap - used_margin : Decimal{};
                    remaining_margin_ratio =
                        divide(left_cap, *strategy_->frozen_equity, Rounding::toward_zero);
                    // Zero disables the sizing helper's cap; an exhausted budget must flatten.
                    if (remaining_margin_ratio == Decimal{})
                        for (auto& member : ready_weights)
                            member.weight = Decimal{};
                }
                // The complete target passed its net budget; quoted legs can exclude its hedge.
                const auto sized = fixed_weight_targets(
                    *strategy_->frozen_equity,
                    ready_weights,
                    strategy_->budget->gross,
                    strategy_->budget->gross,
                    {},
                    remaining_margin_ratio,
                    strategy_->budget->min_rebalance_weight);
                for (const auto& member : sized.members) {
                    const auto& id = member.contract.instrument;
                    strategy_->sized_prices[id] = member.contract.price;
                    strategy_->pending_weights.erase(id);
                    const auto existing = std::ranges::find_if(strategy_->held, [&](const auto& t) {
                        return t.instrument == id;
                    });
                    if (existing == strategy_->held.end())
                        strategy_->held.push_back({id, member.quantity});
                    else
                        existing->quantity = member.quantity;
                }
                batch.targets = strategy_->held;
            }
        }
        const auto exec_window = batch.flattening ? std::chrono::milliseconds{10000}
                                                  : strategy_execution_window(strategy_->execution);
        std::vector<std::pair<InstrumentId, std::string>> failed_orders;
        for (auto it = batch.active_orders.begin(); it != batch.active_orders.end();) {
            const auto found = std::ranges::find_if(state.orders, [&](const BrokerOrder& order) {
                return order.order_id == it->second;
            });
            if (found != state.orders.end() && !working_order(*found)) {
                const auto contract = it->first;
                const auto advance_twap = [&](Decimal filled_lots) {
                    if (filled_lots <= Decimal{})
                        return;
                    auto tw = batch.twap.find(contract);
                    if (tw == batch.twap.end() || !tw->second.slice_active)
                        return;
                    tw->second.slice_remaining = tw->second.slice_remaining > filled_lots
                        ? tw->second.slice_remaining - filled_lots
                        : Decimal{};
                    if (tw->second.slice_remaining <= Decimal{}) {
                        tw->second.slice_active = false;
                        ++tw->second.slices_done;
                        tw->second.next_slice_at =
                            now + std::chrono::milliseconds(strategy_->execution.twap_interval_ms);
                    }
                };
                if (found->status == BrokerOrderStatus::filled) {
                    advance_twap(found->filled);
                    batch.active_order_prices.erase(contract);
                    batch.active_order_sides.erase(contract);
                    batch.active_order_sent_at.erase(contract);
                    batch.reprice_cancelling.erase(contract);
                    it = batch.active_orders.erase(it);
                    continue;
                }
                if (found->status == BrokerOrderStatus::cancelled
                    && batch.reprice_cancelling.erase(contract) > 0) {
                    advance_twap(found->filled);
                    batch.active_order_prices.erase(contract);
                    batch.active_order_sides.erase(contract);
                    batch.active_order_sent_at.erase(contract);
                    it = batch.active_orders.erase(it);
                    continue;
                }
                if (batch.flattening && found->status == BrokerOrderStatus::cancelled
                    && found->filled > Decimal{} && batch.flatten_retries[contract] < 2) {
                    ++batch.flatten_retries[contract];
                    batch.active_order_prices.erase(contract);
                    batch.active_order_sides.erase(contract);
                    batch.active_order_sent_at.erase(contract);
                    it = batch.active_orders.erase(it);
                    continue;
                }
                failed_orders.emplace_back(
                    contract,
                    "strategy order ended without completing its target leg");
            }
            ++it;
        }
        for (const auto& [contract, reason] : failed_orders) {
            abandon_strategy_contract(contract, reason);
            if (!strategy_->batch)
                co_return;
        }
        std::vector<InstrumentId> expired;
        for (const auto& target : batch.targets) {
            if (batch.abandoned.contains(target.instrument))
                continue;
            if (!contract_needs_execution(
                    state,
                    target.instrument,
                    target.quantity,
                    strategy_->paused)) {
                batch.deadlines.erase(target.instrument);
                continue;
            }
            const auto dl = batch.deadlines.find(target.instrument);
            if (dl == batch.deadlines.end()) {
                if (const auto uq =
                        usable_quote(batch.quotes, target.instrument, state.trading_day, wall)) {
                    const auto age = quote_age_ms(*uq, wall);
                    batch.deadlines[target.instrument] =
                        now + exec_window - std::chrono::milliseconds(age);
                }
            } else if (now >= dl->second) {
                expired.push_back(target.instrument);
            }
        }
        for (const auto& contract : expired) {
            abandon_strategy_contract(contract, "strategy target expired before completion");
            if (!strategy_->batch)
                co_return;
        }
        // Bounded reprice: only cancel a broker-confirmed working order once the interval has
        // elapsed and the opposite quote moved adversely, strictly respecting daily cancel limits.
        if (!batch.flattening && strategy_->execution.max_reprices > 0
            && unconfirmed(state).empty()) {
            const auto interval =
                std::chrono::milliseconds(strategy_->execution.reprice_interval_ms);
            for (const auto& [contract, order_id] : batch.active_orders) {
                if (batch.abandoned.contains(contract)
                    || batch.reprice_cancelling.contains(contract))
                    continue;
                if (batch.reprice_counts[contract] >= strategy_->execution.max_reprices)
                    continue;
                const auto intent_it = intents_.find(order_id);
                if (intent_it == intents_.end() || intent_it->second.terminal
                    || intent_it->second.cancel_requested)
                    continue;
                const auto broker_it =
                    std::ranges::find_if(state.orders, [&](const BrokerOrder& order) {
                        return order.order_id == order_id;
                    });
                if (broker_it == state.orders.end() || !working_order(*broker_it))
                    continue;
                const auto sent_it = batch.active_order_sent_at.find(contract);
                const auto price_it = batch.active_order_prices.find(contract);
                const auto side_it = batch.active_order_sides.find(contract);
                if (sent_it == batch.active_order_sent_at.end()
                    || price_it == batch.active_order_prices.end()
                    || side_it == batch.active_order_sides.end()
                    || now < sent_it->second + interval)
                    continue;
                const auto uq = usable_quote(batch.quotes, contract, state.trading_day, wall);
                if (!uq)
                    continue;
                const auto fresh_price = aggressive_limit_price(
                    *uq,
                    allowed(contract),
                    side_it->second,
                    strategy_->execution.aggressive_ticks);
                if (!fresh_price)
                    continue;
                const bool moved_away = side_it->second == Side::buy
                    ? (*fresh_price > price_it->second)
                    : (*fresh_price < price_it->second);
                if (!moved_away)
                    continue;
                if (const auto cancel_limit =
                        policy_->definition.limits.max_daily_cancels_per_contract;
                    cancel_limit > 0) {
                    const auto iso_day = state.trading_day.size() == 8
                        ? state.trading_day.substr(0, 4) + "-" + state.trading_day.substr(4, 2)
                            + "-" + state.trading_day.substr(6, 2)
                        : state.trading_day;
                    std::uint32_t used = 0;
                    if (const auto day_it = cancels_.find(iso_day); day_it != cancels_.end())
                        if (const auto c_it = day_it->second.find(contract);
                            c_it != day_it->second.end())
                            used = c_it->second.cancels;
                    // For opening orders, leave 1 cancel of headroom so the replacement open
                    // order is not immediately blocked by OrderLimits' daily_cancels check.
                    const bool blocked = intent_it->second.offset == Offset::open
                        ? (used + 1 >= cancel_limit)
                        : (used >= cancel_limit);
                    if (blocked)
                        continue;
                }
                const auto cancel_contract = contract;
                const auto cancel_order_id = order_id;
                const auto attempt = ++batch.reprice_counts[cancel_contract];
                batch.reprice_cancelling.insert(cancel_contract);
                ::trading::Command cmd;
                cmd.set_request_id(unique_process_id());
                cmd.mutable_cancel_order()->set_id(cancel_order_id);
                const auto cancel_req = AccountRequest::from_wire(cmd);
                const auto reprice_text = "reprice " + cancel_contract.venue + "/"
                    + cancel_contract.symbol + " (" + std::to_string(attempt) + "/"
                    + std::to_string(strategy_->execution.max_reprices) + ") cancel "
                    + price_it->second.str() + " -> " + fresh_price->str();
                try {
                    co_await cancel(cancel_req, cancel_order_id);
                    if (strategy_ && strategy_->authority == authority)
                        strategy_->events.push_back(
                            event(::trading::INSTANCE_EVENT_KIND_ORDER_SENT, reprice_text));
                } catch (const std::exception& error) {
                    if (strategy_ && strategy_->authority == authority && strategy_->batch
                        && strategy_->batch->accepted_at == batch_accepted_at)
                        strategy_->batch->reprice_cancelling.erase(cancel_contract);
                    log::warn(
                        "strategy reprice cancel failed: order_id={} reason={}",
                        cancel_order_id,
                        error.what());
                }
                co_return;
            }
        }
        std::vector<PositionTarget> executable_targets;
        bool waiting_unquoted = false;
        for (const auto& target : batch.targets) {
            if (batch.abandoned.contains(target.instrument))
                continue;
            const bool needs = contract_needs_execution(
                state,
                target.instrument,
                target.quantity,
                strategy_->paused);
            if (!needs) {
                executable_targets.push_back(target);
                continue;
            }
            if (has_working_order(state, target.instrument)
                || !state.reconciled_for(target.instrument)
                || usable_quote(batch.quotes, target.instrument, state.trading_day, wall)
                    .has_value()) {
                executable_targets.push_back(target);
            } else {
                waiting_unquoted = true;
            }
        }
        const bool waiting_weights =
            std::ranges::any_of(strategy_->pending_weights, [&](const auto& kv) {
                if (batch.abandoned.contains(kv.first))
                    return false;
                return !strategy_->paused || contract_has_position(state, kv.first);
            });
        const auto plan = plan_strategy_orders(
            executable_targets,
            policy_->definition.contracts,
            state,
            !unconfirmed(state).empty());
        if (!waiting_weights && !waiting_unquoted
            && (plan.state == StrategyPlanState::settled
                || (strategy_->paused && plan.state == StrategyPlanState::opening))) {
            strategy_->batch.reset();
            co_return;
        }
        if (plan.orders.empty() || (strategy_->paused && plan.state == StrategyPlanState::opening))
            co_return;
        const StrategyOrder* chosen = nullptr;
        for (const auto& candidate : plan.orders) {
            if (batch.abandoned.contains(candidate.instrument)
                || batch.active_orders.contains(candidate.instrument))
                continue;
            if (!batch.flattening && strategy_->execution.twap_slices > 1) {
                const auto tw = batch.twap.find(candidate.instrument);
                if (tw != batch.twap.end() && !tw->second.slice_active
                    && now < tw->second.next_slice_at)
                    continue;
            }
            chosen = &candidate;
            break;
        }
        if (!chosen)
            co_return;
        // Pace opening legs to the working-order limit while this batch's own orders are in flight.
        if (chosen->offset == Offset::open && !batch.active_orders.empty()
            && static_cast<std::size_t>(std::ranges::count_if(state.orders, working_order))
                >= policy_->definition.limits.max_working_orders)
            co_return;
        // Above the order limit, the move goes out in whole-lot legs replanned after each fill.
        auto leg = *chosen;
        const auto leg_terms = allowed(leg.instrument);
        if (!batch.flattening && strategy_->execution.twap_slices > 1) {
            auto& tw = batch.twap[leg.instrument];
            if (!tw.slice_active) {
                const auto rem_slices = strategy_->execution.twap_slices > tw.slices_done
                    ? (strategy_->execution.twap_slices - tw.slices_done)
                    : std::uint32_t{1};
                const auto total_units = leg.quantity.raw() / leg_terms.quantity_increment.raw();
                const auto slice_units = (total_units + static_cast<std::int64_t>(rem_slices) - 1)
                    / static_cast<std::int64_t>(rem_slices);
                tw.slice_remaining = std::min(
                    leg.quantity,
                    leg_terms.quantity_increment
                        * Decimal::from_integer(std::max<std::int64_t>(1, slice_units)));
                tw.slice_active = true;
            }
            leg.quantity = std::min(leg.quantity, tw.slice_remaining);
        }
        leg.quantity = std::min(
            leg.quantity,
            quantize(
                policy_->definition.limits.max_order_quantity,
                leg_terms.quantity_increment,
                Rounding::floor));
        const auto uq = usable_quote(batch.quotes, leg.instrument, state.trading_day, wall);
        if (!uq)
            co_return;
        auto price = leg.side == Side::buy ? uq->ask : uq->bid;
        const auto liquidity = leg.side == Side::buy ? uq->ask_quantity : uq->bid_quantity;
        if (batch.flattening) {
            const auto& terms = leg_terms;
            const auto ref = uq->last && *uq->last > Decimal{} ? uq->last
                : uq->previous_settlement && *uq->previous_settlement > Decimal{}
                ? uq->previous_settlement
                : std::nullopt;
            if (ref) {
                const auto bound =
                    multiply(*ref, policy_->definition.max_price_deviation, Rounding::toward_zero);
                Decimal upper = *ref + bound;
                Decimal lower = *ref > bound ? *ref - bound : terms.price_increment;
                if (uq->upper_limit && *uq->upper_limit > Decimal{})
                    upper = std::min(upper, *uq->upper_limit);
                if (uq->lower_limit && *uq->lower_limit > Decimal{})
                    lower = std::max(lower, *uq->lower_limit);
                upper = quantize(upper, terms.price_increment, Rounding::floor);
                lower = quantize(lower, terms.price_increment, Rounding::ceiling);
                if (lower <= upper) {
                    const int step = 1 + batch.flatten_retries[leg.instrument];
                    const auto cushion = terms.price_increment * Decimal::from_integer(step);
                    if (price && *price > Decimal{} && liquidity > 0) {
                        const auto candidate =
                            leg.side == Side::buy ? *price + cushion : *price - cushion;
                        price = std::clamp(candidate, lower, upper);
                    } else {
                        // Locked limit board or empty opposite book: submit at the permissible bound.
                        price = leg.side == Side::buy ? upper : lower;
                    }
                } else if (liquidity <= 0) {
                    price.reset();
                }
            } else if (liquidity <= 0) {
                price.reset();
            }
        } else {
            price = aggressive_limit_price(
                *uq,
                leg_terms,
                leg.side,
                strategy_->execution.aggressive_ticks);
        }
        if (!price || *price <= Decimal{}) {
            abandon_strategy_contract(
                leg.instrument,
                "strategy execution has no usable opposite-side quote");
            co_return;
        }
        auto dl = batch.deadlines.find(leg.instrument);
        if (dl == batch.deadlines.end()) {
            dl = batch.deadlines
                     .emplace(
                         leg.instrument,
                         now + exec_window - std::chrono::milliseconds(quote_age_ms(*uq, wall)))
                     .first;
        }
        StrategyOrderBasis
            basis{authority, strategy_->sequence, strategy_->next_leg, true, dl->second};
        check_strategy(basis);
        basis.quote = BrokerQuote{
            uq->instrument,
            uq->last,
            uq->previous_settlement,
            uq->upper_limit,
            uq->lower_limit,
            uq->trading_day,
            uq->update_time,
            strategy_->connection_generation,
            // A flattening batch carries whatever quotes were cached: age each leg by its own.
            steady_now(*clock_) - std::chrono::milliseconds(quote_age_ms(*uq, utc_now_ms(*clock_)))
        };
        // Before admission this is only a plan. A newly received broker event
        // causes a fresh plan on the next turn, never a resend of an order.
        if (control != send_gate_.revision())
            co_return;
        StrategyOrderSource origin{authority->instance(), basis.target_sequence, basis.leg};
        if (strategy_->frozen_equity && !strategy_->raw_weights.empty()) {
            const auto w_it = strategy_->raw_weights.find(leg.instrument);
            const auto weight = w_it != strategy_->raw_weights.end() ? w_it->second : Decimal{};
            const auto p_it = strategy_->sized_prices.find(leg.instrument);
            const auto sized_price =
                p_it != strategy_->sized_prices.end() ? p_it->second : *uq->last;
            const auto t_it = std::ranges::find_if(batch.targets, [&](const auto& t) {
                return t.instrument == leg.instrument;
            });
            const auto target_qty = t_it != batch.targets.end() ? t_it->quantity : Decimal{};
            origin.weight_sizing = StrategyOrderSource::WeightSizing{
                *strategy_->frozen_equity,
                weight,
                sized_price,
                target_qty
            };
        }
        sent = unique_process_id();
        sent_instrument = leg.instrument;
        const auto request = AccountRequest::strategy_order(
            {{sent, leg.instrument, leg.side, leg.quantity, *price}, leg.offset},
            origin);
        batch.active_orders[leg.instrument] = sent;
        batch.active_order_prices[leg.instrument] = *price;
        batch.active_order_sides[leg.instrument] = leg.side;
        batch.active_order_sent_at[leg.instrument] = now;
        // Not std::format: macOS 13.0 libc++ lacks the to_chars it instantiates.
        const auto sent_text = "sent " + leg.instrument.venue + "/" + leg.instrument.symbol
            + (leg.side == Side::buy ? " buy " : " sell ")
            + (leg.offset == Offset::open ? "open " : "close ") + leg.quantity.str() + " @ "
            + price->str();
        // No references to batch survive this suspension: a stop can release it.
        co_await execute_strategy(basis, request, control);
        if (strategy_ && strategy_->authority == authority)
            strategy_->events.push_back(
                event(::trading::INSTANCE_EVENT_KIND_ORDER_SENT, sent_text));
    } catch (const std::invalid_argument& error) {
        if (sent.empty()) {
            if (strategy_ && strategy_->authority == authority)
                fail_strategy(error.what());
            throw;
        }
        if (!strategy_ || strategy_->authority != authority || !strategy_->failure.empty())
            throw;
        if (strategy_->batch && strategy_->batch->accepted_at == batch_accepted_at)
            abandon_strategy_contract(sent_instrument, error.what());
    } catch (const std::exception& error) {
        if (!strategy_ || strategy_->authority != authority || !strategy_->failure.empty())
            throw;
        // A replacement batch must survive the old leg's asynchronous failure.
        if (sent.empty()) {
            abandon_strategy_batch(error.what());
            co_return;
        }
        if (!strategy_->batch || strategy_->batch->accepted_at != batch_accepted_at)
            co_return;
        const std::string_view msg = error.what();
        const bool transient_basis = !strategy_->paused && observed([&](const BrokerSnapshot& now) {
            return authorized(now);
        })
            && (msg == "order execution basis changed before durable submission; the order was not sent"
                || msg
                    == "order execution basis changed during durable submission; the order was not sent"
                || msg == "order permission changed before dispatch; the order was not sent"
                || msg == "account changed after strategy order admission");
        if (transient_basis || msg == "broker exposure changed; evaluate pre-trade risk again"
            || msg
                == "broker fills are awaiting position reconciliation; opening orders are paused") {
            if (const auto active = strategy_->batch->active_orders.find(sent_instrument);
                active != strategy_->batch->active_orders.end() && active->second == sent) {
                strategy_->batch->active_orders.erase(active);
                strategy_->batch->active_order_prices.erase(sent_instrument);
                strategy_->batch->active_order_sides.erase(sent_instrument);
                strategy_->batch->active_order_sent_at.erase(sent_instrument);
            }
            co_return;
        }
        if (const auto active = strategy_->batch->active_orders.find(sent_instrument);
            active != strategy_->batch->active_orders.end() && active->second == sent)
            abandon_strategy_contract(sent_instrument, error.what());
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
    strategy_->events.push_back(event(
        ::trading::INSTANCE_EVENT_KIND_BATCH_ABANDONED,
        "batch abandoned: " + std::string(reason)));
    send_gate_.invalidate();
    request_strategy_cancels();
}

void LiveAccountState::abandon_strategy_contract(
    const InstrumentId& contract,
    std::string_view reason)
{
    if (!strategy_ || !strategy_->batch || strategy_->batch->abandoned.contains(contract))
        return;
    log::warn(
        "strategy contract abandoned: instance={} sequence={} contract={}/{} reason={}",
        strategy_->authority->instance(),
        strategy_->sequence,
        contract.venue,
        contract.symbol,
        reason);
    strategy_->batch->abandoned.insert(contract);
    strategy_->batch->active_orders.erase(contract);
    strategy_->batch->active_order_prices.erase(contract);
    strategy_->batch->active_order_sides.erase(contract);
    strategy_->batch->active_order_sent_at.erase(contract);
    strategy_->batch->reprice_cancelling.erase(contract);
    strategy_->batch->twap.erase(contract);
    strategy_->batch->deadlines.erase(contract);
    strategy_->pending_weights.erase(contract);
    strategy_->events.push_back(event(
        ::trading::INSTANCE_EVENT_KIND_BATCH_ABANDONED,
        contract.venue + "/" + contract.symbol + " abandoned: " + std::string(reason)));
    request_strategy_cancels(contract);
    const auto state = trader_->snapshot();
    const bool waiting_weights =
        std::ranges::any_of(strategy_->pending_weights, [&](const auto& kv) {
            if (strategy_->batch->abandoned.contains(kv.first))
                return false;
            return !strategy_->paused || contract_has_position(state, kv.first);
        });
    const bool any_target_left =
        std::ranges::any_of(strategy_->batch->targets, [&](const PositionTarget& target) {
            return !strategy_->batch->abandoned.contains(target.instrument)
                && contract_needs_execution(
                    state,
                    target.instrument,
                    target.quantity,
                    strategy_->paused);
        });
    if (!waiting_weights && !any_target_left)
        strategy_->batch.reset();
}

void LiveAccountState::pause_strategy(const StrategyAuthorityPtr& authority, bool paused)
{
    if (!strategy_ || strategy_->authority != authority)
        throw Error(ErrorCode::conflict, "strategy execution owner no longer matches");
    strategy_->paused = paused;
    // An opening leg already queued to the SDK would otherwise still use its permit.
    if (paused && strategy_->batch) {
        for (const auto& [_, order_id] : strategy_->batch->active_orders) {
            if (const auto leg = intents_.find(order_id); leg != intents_.end()
                && leg->second.offset == Offset::open && !leg->second.terminal) {
                send_gate_.invalidate();
                break;
            }
        }
    }
}

void LiveAccountState::flatten_strategy(
    const StrategyAuthorityPtr& authority,
    std::vector<MarketQuote> quotes)
{
    if (!strategy_ || strategy_->authority != authority)
        throw Error(ErrorCode::conflict, "strategy execution owner no longer matches");
    observe_quotes(quotes);
    abandon_strategy_batch("superseded by flattening");
    strategy_->paused = true;
    strategy_->raw_weights.clear();
    strategy_->pending_weights.clear();
    strategy_->frozen_equity.reset();
    strategy_->frozen_margin_scale = Decimal::parse("1");
    strategy_->sized_prices.clear();
    strategy_->weights_validated = true;
    const auto now = steady_now(*clock_);
    const auto state = trader_->snapshot();
    const auto wall = utc_now_ms(*clock_);
    StrategyTargetBatch batch;
    batch.accepted_at = now;
    batch.flattening = true;
    std::set<InstrumentId> flatten_set;
    for (const auto& contract : authority->contracts_)
        if (flatten_set.insert(contract).second)
            batch.targets.push_back({contract, Decimal{}});
    for (const auto& contract : strategy_->active_series)
        if (flatten_set.insert(contract).second)
            batch.targets.push_back({contract, Decimal{}});
    for (const auto& spec : authority->series_)
        for (const auto& candidate : spec.candidates)
            if ((std::ranges::any_of(
                     quotes,
                     [&](const auto& q) {
                         return q.instrument == candidate;
                     })
                    || contract_has_position(state, candidate))
                && flatten_set.insert(candidate).second)
                batch.targets.push_back({candidate, Decimal{}});
    std::set<InstrumentId> recorded_quotes;
    for (const auto& q : quotes) {
        if (!recorded_quotes.insert(q.instrument).second)
            continue;
        if (const auto uq = usable_quote(quotes, q.instrument, state.trading_day, wall)) {
            batch.quotes.push_back(*uq);
            if (flatten_set.contains(uq->instrument)
                && contract_needs_execution(state, uq->instrument, Decimal{}, true)) {
                const auto age = quote_age_ms(*uq, wall);
                batch.deadlines[uq->instrument] = now + std::chrono::milliseconds(10000 - age);
            }
        }
    }
    // Flattening replaces the strategy's targets until it gives new ones.
    strategy_->held = batch.targets;
    strategy_->next_leg = 0;
    strategy_->deadline = now + std::chrono::seconds(10);
    strategy_->batch = std::move(batch);
    strategy_->events.push_back(event(::trading::INSTANCE_EVENT_KIND_FLATTENING, "flattening"));
}

std::vector<::trading::InstanceEvent> LiveAccountState::take_strategy_events(
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

void LiveAccountState::request_strategy_cancels(std::optional<InstrumentId> only)
{
    // Mark once: reconnect, another failure or a repeated stop cannot retry a cancel.
    for (auto& [id, intent] : intents_)
        if (intent.strategy && intent.strategy->instance == strategy_->authority->instance()
            && (!only || intent.instrument == *only) && !intent.terminal
            && !intent.cancel_requested)
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
    if (steady_now(*clock_) >= basis.expires_at)
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
    const auto& prelim_order = std::get<SubmitOrder>(request.operation);
    const auto expected_deadline = [&] {
        if (strategy_->batch) {
            const auto dl = strategy_->batch->deadlines.find(prelim_order.order.instrument);
            if (dl != strategy_->batch->deadlines.end())
                return dl->second;
        }
        return strategy_->deadline;
    }();
    // Sequence 0 is a flattening before the instance's first decision.
    if (basis.leg >= StrategyOrderSource::max_legs || basis.target_sequence < strategy_->sequence
        || (basis.target_sequence == strategy_->sequence
            && (basis.leg != strategy_->next_leg || basis.expires_at != expected_deadline))
        || (basis.target_sequence > strategy_->sequence && basis.leg != 0))
        throw Error(
            ErrorCode::conflict,
            "strategy target sequence, leg or original deadline does not match");
    // Consume the leg before any suspension. Failure or an unknown result cannot
    // turn this identity back into an executable request.
    strategy_->sequence = basis.target_sequence;
    strategy_->next_leg = basis.leg + 1;
    if (!strategy_->batch)
        strategy_->deadline = basis.expires_at;
    try {
        check_strategy(basis);
        if (failed_)
            throw Error(ErrorCode::unavailable, "live trading record needs recovery");
        if (control != send_gate_.revision())
            throw Error(ErrorCode::conflict, "account changed after strategy order admission");
        const auto& order = std::get<SubmitOrder>(request.operation);
        const bool fixed_or_active =
            std::ranges::find(basis.authority->contracts_, order.order.instrument)
                != basis.authority->contracts_.end()
            || std::ranges::find(strategy_->active_series, order.order.instrument)
                != strategy_->active_series.end();
        const bool closing_series = order.offset != Offset::open
            && std::ranges::any_of(basis.authority->series_, [&](const auto& spec) {
                   return std::ranges::find(spec.candidates, order.order.instrument)
                       != spec.candidates.end();
               });
        if (!fixed_or_active && !closing_series)
            throw std::invalid_argument("order contract is not selected by this strategy instance");
        co_await submit(request, order, control, basis);
    } catch (const std::invalid_argument& error) {
        if (!basis.quote && strategy_ && strategy_->authority == basis.authority)
            fail_strategy(error.what());
        throw;
    }
}

} // namespace asterion::trading
