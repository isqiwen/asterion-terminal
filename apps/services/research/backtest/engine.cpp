#include "engine.hpp"
#include <asterion/protocol/data.hpp>
#include "strategy.hpp"
#include <asterion/futures/portfolio_targets.hpp>
#include "performance.hpp"
#include <asterion/futures/trading_days.hpp>
#include <asterion/futures/order_limits.hpp>
#include <asterion/futures/order_limits.hpp>
#include <asterion/futures/paper_execution.hpp>
#include "portfolio.hpp"
#include <algorithm>
#include <map>
#include <cmath>
#include <asterion/futures/futures.hpp>
#include <stdexcept>

namespace asterion::backtest {
namespace {

Decimal decimal(const protocol::proto::Decimal& value)
{
    return Decimal::from_raw(value.units());
}

constexpr std::size_t none = static_cast<std::size_t>(-1);

// For each contract, the dominant series it is a month of, or none.
std::vector<std::size_t> series_of(const backtest::proto::BacktestInput& input)
{
    std::vector<std::size_t> result(static_cast<std::size_t>(input.paper().contracts_size()), none);
    for (int s = 0; s < input.series_size(); ++s)
        for (const auto& roll : input.series(s).rolls())
            result.at(roll.contract()) = static_cast<std::size_t>(s);
    return result;
}

std::vector<bool> sparse(const std::vector<std::size_t>& series)
{
    std::vector<bool> result;
    for (const auto item : series)
        result.push_back(item != none);
    return result;
}

// The bar a series strategy sees: prices scaled to the level of the latest
// month and put back on the price grid. Never used for orders or fills.
MarketBar adjusted(MarketBar bar, Decimal factor, Decimal increment)
{
    for (auto* price : {&bar.open, &bar.high, &bar.low, &bar.close})
        *price = protocol::dominant_price(*price, factor, increment);
    return bar;
}

} // namespace

void validate(const backtest::proto::BacktestInput& input)
{
    static_cast<void>(protocol::decode_backtest(input, protocol::DatasetView::metadata));
    protocol::require_backtest_scoring_assumptions(
        static_cast<std::size_t>(input.strategies_size()),
        input.performance_assumptions());
    const auto& p = input.paper();
    const auto series = series_of(input);
    std::vector<int> series_bars(static_cast<std::size_t>(input.series_size()));
    std::size_t warmup = 0;
    for (const auto& strategy : input.strategies())
        warmup = std::max(warmup, protocol::strategy_warmup(strategy));
    for (int index = 0; index < p.contracts_size(); ++index) {
        const auto& item = p.contracts(index);
        const auto& c = item.dataset().contract();
        FuturesContract contract{protocol::instrument(c), c.product(), c.delivery_month()};
        contract.validate();
        (void)protocol::cost_schedule(item.cost_schedule());
        for (const auto& strategy : input.strategies())
            if (!CrossSection::defines(strategy) && !strategy.has_fixed_weights())
                static_cast<void>(make_strategy(strategy, contract.instrument));
        // A series is one strategy over all of its months.
        if (series[static_cast<std::size_t>(index)] != none)
            series_bars[series[static_cast<std::size_t>(index)]] += item.dataset().bars_size();
        else if (static_cast<std::size_t>(item.dataset().bars_size()) < warmup)
            throw std::invalid_argument(
                "backtest requires enough bars for the strategy to take a side on every contract");
    }
    for (const auto bars : series_bars)
        if (static_cast<std::size_t>(bars) < warmup)
            throw std::invalid_argument(
                "backtest requires enough bars for the strategy to take a side on every contract");
    // A rule that ranks takes each ordinary contract and each series as one.
    const auto units = static_cast<std::size_t>(std::ranges::count(series, none))
        + static_cast<std::size_t>(input.series_size());
    for (const auto& strategy : input.strategies())
        if (CrossSection::defines(strategy)) {
            static_cast<void>(CrossSection(strategy, units));
            // Only a series has a later month to stand against.
            if (strategy.cross().factor() == protocol::proto::TERM_STRUCTURE
                && std::ranges::count(series, none))
                throw std::invalid_argument("ranking by the term structure needs dominant series");
        }
    decode_order_limits(protocol::decode_risk(p.risk()));
    if (decimal(p.deposit()) <= Decimal{})
        throw std::invalid_argument("backtest requires positive capital");
    static_cast<void>(replay_schedule(paper_portfolio(p), sparse(series)));
}

namespace {

// What one strategy wants of every unit as bars arrive: one strategy for each
// unit, or one rule that ranks them all.
struct Follower
{
    const protocol::proto::Strategy* definition = nullptr;
    std::unique_ptr<CrossSection> cross;
    std::size_t portfolio_observations = 0;
    std::vector<std::unique_ptr<Strategy>> strategies;
    // The last target asked for each unit.
    std::vector<std::optional<Decimal>> wanted;
};

// One account replayed over the input's bars: all of them, or only the
// trading days before `until`; a partial replay ends with that day's
// settlement and carries the equity record alone. Every one of `definitions`
// reads every bar. `followed` says, for each trading day, whose targets the
// account takes: one of them, or none and it holds nothing.
backtest::proto::BacktestResult replay(
    const backtest::proto::BacktestInput& input,
    const std::vector<const protocol::proto::Strategy*>& definitions,
    const std::vector<std::optional<std::size_t>>& followed,
    const std::string& until,
    std::stop_token stop,
    const std::function<void()>& step)
{
    const auto& p = input.paper();
    auto portfolio = paper_portfolio(p);
    const auto series = series_of(input);
    const auto schedule = replay_schedule(portfolio, sparse(series));
    auto risk = std::make_shared<OrderLimits>(decode_order_limits(protocol::decode_risk(p.risk())));
    risk->start();
    // What a rule takes a side on is a unit: each ordinary contract, and each
    // dominant series as one, read from the back-adjusted bars of whichever
    // month is dominant. `unit` says which one a contract belongs to.
    std::vector<std::size_t> unit(series.size());
    std::vector<Instrument> units;
    for (std::size_t c = 0; c < series.size(); ++c)
        if (series[c] == none) {
            unit[c] = units.size();
            units.push_back(portfolio.contracts[c].terms.instrument);
        }
    for (std::size_t c = 0; c < series.size(); ++c)
        if (series[c] != none)
            unit[c] = units.size() + series[c];
    for (const auto& rolls : input.series())
        units.push_back(portfolio.contracts[rolls.rolls(0).contract()].terms.instrument);
    // What a ranking by the term structure reads: the carry each trading day
    // of each series begins with, where it has a term point.
    std::vector<std::map<std::string, Decimal>> carry;
    {
        std::vector<const protocol::proto::Contract*> contracts;
        for (const auto& contract : p.contracts())
            contracts.push_back(&contract.dataset().contract());
        for (const auto& rolls : input.series())
            carry.push_back(protocol::term_carries(rolls, contracts));
    }
    std::vector<Follower> followers;
    for (const auto* definition : definitions) {
        auto& follower = followers.emplace_back();
        follower.definition = definition;
        follower.wanted.resize(units.size());
        if (definition->has_fixed_weights())
            continue;
        if (CrossSection::defines(*definition))
            follower.cross = std::make_unique<CrossSection>(*definition, units.size());
        else
            for (const auto& instrument : units)
                follower.strategies.push_back(make_strategy(*definition, instrument));
    }
    PaperExecution execution(decimal(p.deposit()), std::move(portfolio.contracts), risk);
    execution.start();
    for (auto& follower : followers)
        for (auto& strategy : follower.strategies)
            strategy->start();

    // What a contract decided on at a bar. A ranking made once all bars of a
    // timestamp are in decides those contracts again, and a day's end after
    // which the account follows another strategy decides every contract again
    // at its last bar.
    struct Decided
    {
        std::size_t contract, source_index;
        std::string order;
        Decimal close;
        bool leading;
    };

    std::vector<Decided> together;
    std::vector<std::optional<Decided>> last(series.size());
    // What the account wants of the contract a unit trades now while it
    // follows `whom`: the unit's target, and nothing while it follows no one.
    const auto target = [&](const std::optional<std::size_t>& whom,
                            std::size_t contract) -> std::optional<Decimal> {
        if (!whom)
            return Decimal{};
        return followers[*whom].wanted[unit[contract]];
    };
    const auto holds = [&](std::size_t contract) {
        const auto& id = execution.contract(contract).terms.instrument.id;
        return std::ranges::any_of(execution.account().positions(), [&](const auto& lot) {
            return lot.instrument == id;
        });
    };
    backtest::proto::BacktestResult result;
    result.set_dataset_revision(input.dataset_revision());
    auto peak = decimal(p.deposit());
    Decimal drawdown;
    const auto add_equity =
        [&](std::int64_t time, backtest::proto::EquityEvent event, const FuturesAccount& account) {
            const auto equity = account.balance() + account.unrealized();
            peak = std::max(peak, equity);
            drawdown = std::max(drawdown, peak - equity);
            auto* point = result.add_equity();
            point->set_timestamp_ns(time);
            point->set_event(event);
            point->mutable_equity()->set_units(equity.raw());
        };
    std::vector<std::optional<PaperExecution::Target>> pending(series.size());
    // A strategy that has given no target yet leaves the contract alone, and
    // none decides for a month that is not dominant: a roll closes that.
    const auto issue = [&](const Decided& item, const std::optional<std::size_t>& taken) {
        if (!item.leading)
            return;
        if (const auto quantity = target(taken, item.contract))
            pending[item.contract] = PaperExecution::Target{item.order, *quantity, item.close};
    };
    // For a roll: whether a month has taken over what its series holds. The
    // first month of a series has none to take over from and trades as any
    // contract does.
    std::vector<bool> took_over(series.size());
    for (const auto& rolls : input.series())
        took_over[rolls.rolls(0).contract()] = true;
    const auto total = execution.size();
    const auto& account = execution.account();
    std::size_t index = 0, recorded_fills = 0, recorded_portfolio_fills = 0;
    const bool fixed_portfolio =
        input.strategies_size() == 1 && input.strategies(0).has_fixed_weights();
    std::map<std::string, std::size_t> portfolio_order_origins;
    const bool record_fill_days = std::ranges::all_of(p.contracts(), [](const auto& contract) {
        return contract.dataset().interval_minutes() == 0;
    });
    // Every day one of the contracts settles on, in order: its bars, where any
    // contract has some, then its settlement.
    for (std::size_t d = 0; d < schedule.days(); ++d) {
        const auto& day = schedule.day(d);
        const auto& whom = followed[d];
        // A day without bars is settled when its session closes.
        auto settled_at = trading_day_close_ns(day.trading_day);
        for (; index < total && schedule.event(index).day == d; ++index) {
            if (stop.stop_requested())
                throw std::runtime_error("backtest cancelled");
            const auto current = execution.event(index);
            const auto& bar = execution.bar(current);
            const auto& instrument = execution.contract(current.contract).terms.instrument.id;
            // Orders placed after a contract's bar N can only fill on its bar N+1;
            // unfilled remainders expire before its strategy decides again.
            auto& intent = pending[current.contract];
            const auto member = series[current.contract];
            const auto* roll = member == none
                ? nullptr
                : &protocol::dominant_roll(input.series(static_cast<int>(member)), bar.trading_day);
            // The contract its unit trades now: an ordinary one, or the dominant
            // month. A month that is no longer dominant only closes what it holds.
            const bool leading = !roll || roll->contract() == current.contract;
            if (roll) {
                const auto& rolls = input.series(static_cast<int>(member));
                // A roll moves what the series holds and decides nothing, so it waits
                // for no price: both months trade at the open of a bar, which every
                // bar reaches. A month that is no longer dominant gives up what it
                // holds, on each of its bars until their volume has taken it all. The
                // dominant month takes over what its series wanted after the bars
                // before, on its first bar that finds the other months closed: no two
                // months of a series are held at once. On daily bars both happen on
                // the roll day where both months trade on it, the left month first. A
                // month takes no order before the account has a price of it, from a
                // bar or a settlement, as any contract.
                const bool others = std::ranges::any_of(rolls.rolls(), [&](const auto& other) {
                    return other.contract() != current.contract && holds(other.contract());
                });
                if (!leading)
                    intent = PaperExecution::Target{
                        "roll." + std::to_string(index),
                        Decimal{},
                        bar.open
                    };
                else if (others) {
                    if (intent && intent->quantity != Decimal{})
                        intent.reset();
                } else if (!took_over[current.contract] && account.priced(current.contract)) {
                    if (const auto quantity = target(whom, current.contract))
                        intent = PaperExecution::Target{
                            "roll." + std::to_string(index),
                            *quantity,
                            bar.open
                        };
                    took_over[current.contract] = true;
                }
            }
            const auto previous_orders = account.orders().size();
            const auto risk_decision = execution.advance(intent);
            if (fixed_portfolio) {
                for (auto n = previous_orders; n < account.orders().size(); ++n) {
                    if (!intent || result.portfolio_targets().empty())
                        throw std::logic_error("portfolio order has no original decision");
                    const auto& decision =
                        result.portfolio_targets(result.portfolio_targets_size() - 1);
                    const auto& member = decision.members(static_cast<int>(current.contract));
                    const auto& order = account.orders()[n].order.request();
                    if (member.intent_id() != intent->order_id || member.source_index() >= index
                        || order.instrument != instrument)
                        throw std::logic_error(
                            "portfolio order does not follow its original decision");
                    auto* accepted = result.add_portfolio_orders();
                    accepted->set_order_id(order.id);
                    accepted->set_intent_id(member.intent_id());
                    accepted->set_decision_source_index(member.source_index());
                    accepted->set_source_index(static_cast<unsigned>(index));
                    accepted->set_boundary_ns(bar.timestamp_ns);
                    portfolio_order_origins.emplace(
                        order.id,
                        static_cast<std::size_t>(result.portfolio_orders_size() - 1));
                }
                while (recorded_portfolio_fills < account.fills().size()) {
                    const auto& fill = account.fills()[recorded_portfolio_fills++];
                    const auto found = portfolio_order_origins.find(fill.execution.order_id);
                    if (found == portfolio_order_origins.end())
                        throw std::logic_error("portfolio fill has no accepted original order");
                    const auto& accepted = result.portfolio_orders(static_cast<int>(found->second));
                    auto* observed = result.add_portfolio_fills();
                    observed->set_fill_id(fill.execution.execution_id);
                    observed->set_intent_id(accepted.intent_id());
                    observed->set_decision_source_index(accepted.decision_source_index());
                    observed->set_source_index(static_cast<unsigned>(index));
                    observed->set_boundary_ns(bar.timestamp_ns);
                }
            }
            if (!risk_decision.allowed()) {
                if (!intent)
                    throw std::logic_error("risk rejection without a target");
                auto* rejected = result.add_target_rejections();
                rejected->set_intent_id(intent->order_id);
                rejected->set_source_index(static_cast<std::uint32_t>(index));
                rejected->set_boundary_ns(bar.timestamp_ns);
                rejected->mutable_target()->set_units(intent->quantity.raw());
                using Rejection = proto::BacktestTargetRejection;
                switch (risk_decision.reason) {
                    case RiskReason::order_quantity:
                        rejected->set_reason(Rejection::ORDER_QUANTITY);
                        break;
                    case RiskReason::gross_quantity:
                        rejected->set_reason(Rejection::GROSS_QUANTITY);
                        break;
                    case RiskReason::working_orders:
                        rejected->set_reason(Rejection::WORKING_ORDERS);
                        break;
                    case RiskReason::insufficient_funds:
                        rejected->set_reason(Rejection::INSUFFICIENT_FUNDS);
                        break;
                    default:
                        throw std::logic_error("unexpected completed-bar risk decision");
                }
            }
            intent.reset();
            // Daily orders expire after their original bar/day. A target remains
            // evidence of intent; expiration does not turn it into a GTC order.
            execution.cancel_open_orders(instrument);
            const auto order = "strategy." + std::to_string(index);
            if (leading) {
                const auto u = unit[current.contract];
                const auto observe = [&](const MarketBar& seen) {
                    for (auto& follower : followers) {
                        if (follower.definition && follower.definition->has_fixed_weights())
                            continue;
                        if (!follower.cross) {
                            if (const auto wanted = follower.strategies[u]->on_bar(seen))
                                follower.wanted[u] = wanted;
                        } else if (follower.definition->cross().factor()
                            != protocol::proto::TERM_STRUCTURE) {
                            follower.cross->on_bar(u, seen.timestamp_ns, seen.close);
                        } else if (const auto term = carry[member].find(bar.trading_day);
                            term != carry[member].end()) {
                            // A day without a term point is no bar of this unit for the ranking.
                            follower.cross->on_bar(u, seen.timestamp_ns, seen.close, term->second);
                        }
                    }
                };
                if (roll) {
                    // On the price increment of the bar's day, which the account trades at.
                    const auto seen = adjusted(
                        bar,
                        decimal(roll->factor()),
                        account.contracts()[current.contract].instrument.price_increment);
                    observe(seen);
                } else
                    observe(bar);
            }
            const Decided decided{current.contract, index, order, bar.close, leading};
            const bool fixed_portfolio = whom && followers[*whom].definition
                && followers[*whom].definition->has_fixed_weights();
            if (!fixed_portfolio)
                issue(decided, whom);
            last[current.contract] = decided;
            together.push_back(decided);
            if (index + 1 == total
                || execution.bar(execution.event(index + 1)).timestamp_ns != bar.timestamp_ns) {
                for (std::size_t k = 0; k < followers.size(); ++k) {
                    auto& follower = followers[k];
                    if (follower.definition && follower.definition->has_fixed_weights()) {
                        const auto& policy = follower.definition->fixed_weights();
                        ++follower.portfolio_observations;
                        if (follower.portfolio_observations % policy.rebalance() != 0 || whom != k)
                            continue;
                        if (together.size() != static_cast<std::size_t>(p.contracts_size()))
                            throw std::logic_error(
                                "fixed-weight decision is missing a completed member bar");
                        std::vector<WeightedContract> members;
                        for (const auto& item : together) {
                            const auto& terms = execution.contract(item.contract).terms.instrument;
                            const auto fixed =
                                std::ranges::find_if(policy.members(), [&](const auto& m) {
                                    return m.venue() == terms.id.venue
                                        && m.symbol() == terms.id.symbol;
                                });
                            if (fixed == policy.members().end())
                                throw std::logic_error(
                                    "fixed-weight decision has no declared contract");
                            members.push_back(
                                {terms.id, decimal(fixed->weight()), item.close, terms.multiplier});
                        }
                        const auto targets = fixed_weight_targets(
                            account.balance() + account.unrealized(),
                            members,
                            decimal(policy.gross_weight_limit()),
                            decimal(policy.net_weight_limit()),
                            stop);
                        auto* observed = result.add_portfolio_targets();
                        observed->set_boundary_ns(bar.timestamp_ns);
                        observed->set_trading_day(bar.trading_day);
                        observed->set_has_next_bar(index + 1 < total);
                        observed->mutable_equity()->set_units(targets.equity.raw());
                        observed->mutable_gross_weight()->set_units(targets.gross_weight.raw());
                        observed->mutable_net_weight()->set_units(targets.net_weight.raw());
                        observed->mutable_gross_limit()->set_units(targets.gross_limit.raw());
                        observed->mutable_net_limit()->set_units(targets.net_limit.raw());
                        observed->mutable_requested_gross_notional()->set_units(
                            targets.requested_gross_notional.raw());
                        observed->mutable_requested_net_notional()->set_units(
                            targets.requested_net_notional.raw());
                        observed->mutable_allocated_gross_notional()->set_units(
                            targets.allocated_gross_notional.raw());
                        observed->mutable_allocated_net_notional()->set_units(
                            targets.allocated_net_notional.raw());
                        observed->mutable_unallocated_nominal_budget()->set_units(
                            targets.unallocated_nominal_budget.raw());
                        for (std::size_t m = 0; m < targets.members.size(); ++m) {
                            const auto& target = targets.members[m];
                            const auto& item = together[m];
                            auto* member = observed->add_members();
                            member->set_venue(target.contract.instrument.venue);
                            member->set_symbol(target.contract.instrument.symbol);
                            member->set_intent_id(item.order);
                            member->set_source_index(static_cast<unsigned>(item.source_index));
                            member->mutable_weight()->set_units(target.contract.weight.raw());
                            member->mutable_price()->set_units(target.contract.price.raw());
                            member->mutable_multiplier()->set_units(
                                target.contract.multiplier.raw());
                            member->mutable_requested_notional()->set_units(
                                target.requested_notional.raw());
                            member->mutable_quantity()->set_units(target.quantity.raw());
                            member->mutable_allocated_notional()->set_units(
                                target.allocated_notional.raw());
                            member->mutable_nominal_remainder()->set_units(
                                target.nominal_remainder.raw());
                            auto& original_state = *member->mutable_observation();
                            for (const auto& lot : account.positions())
                                if (lot.instrument == target.contract.instrument)
                                    *original_state.add_positions() =
                                        protocol::encode_position(lot);
                            for (const auto index : account.working_orders()) {
                                const auto& accepted = account.orders()[index];
                                if (accepted.order.request().instrument
                                    == target.contract.instrument)
                                    *original_state.add_working_orders() = protocol::encode_order(
                                        {accepted.order.request(),
                                            accepted.offset,
                                            accepted.order.filled_quantity(),
                                            accepted.order.state()});
                            }
                            follower.wanted[unit[item.contract]] = target.quantity;
                            issue(item, whom);
                        }
                        continue;
                    }
                    if (!follower.cross)
                        continue;
                    const auto shares = follower.cross->rank();
                    if (!shares)
                        continue;
                    // A unit holds the lots of the month it trades now whose value at
                    // this close is nearest its share of the rule's notional.
                    const auto notional = decimal(follower.definition->cross().notional());
                    for (const auto& item : together) {
                        if (!item.leading)
                            continue;
                        const auto u = unit[item.contract];
                        const auto share = (*shares)[u];
                        const auto& terms = execution.contract(item.contract).terms.instrument;
                        const auto lots = quantize(
                            divide(
                                multiply(
                                    notional,
                                    Decimal::from_raw(std::llround(std::abs(share) * 100000000)),
                                    Rounding::half_up),
                                multiply(item.close, terms.multiplier, Rounding::half_up),
                                Rounding::half_up),
                            terms.quantity_increment,
                            Rounding::half_up);
                        follower.wanted[u] = share > 0 ? lots
                            : share < 0                ? Decimal{} - lots
                                                       : Decimal{};
                    }
                    // Every contract of this timestamp decided before the ranking was known.
                    if (whom == k)
                        for (const auto& item : together)
                            issue(item, whom);
                }
                together.clear();
            }
            if (record_fill_days)
                while (recorded_fills < account.fills().size()) {
                    const auto& fill = account.fills()[recorded_fills++];
                    auto* observed = result.add_fill_days();
                    observed->set_fill_id(fill.execution.execution_id);
                    observed->set_trading_day(bar.trading_day);
                }
            add_equity(bar.timestamp_ns, backtest::proto::TRADE_MARK, account);
            settled_at = bar.timestamp_ns;
            step();
        }
        execution.cancel_open_orders();
        const auto event = std::ranges::find_if(input.settlement_margins(), [&](const auto& fixed) {
            return fixed.trading_day() == day.trading_day;
        });
        const auto revaluation = execution.settle_scheduled(
            day.trading_day,
            day.prices,
            event == input.settlement_margins().end() ? std::nullopt
                                                      : std::optional{decimal(event->rate())});
        add_equity(settled_at, backtest::proto::DAILY_SETTLEMENT, account);
        auto* settled = result.add_settlements();
        settled->set_trading_day(day.trading_day);
        settled->set_timestamp_ns(settled_at);
        settled->mutable_balance()->set_units(account.balance().raw());
        settled->mutable_equity()->set_units((account.balance() + account.unrealized()).raw());
        settled->mutable_realized()->set_units(account.realized().raw());
        settled->mutable_fees()->set_units(account.fees().raw());
        if (revaluation) {
            auto* change = settled->mutable_margin_revaluation();
            change->mutable_previous_rate()->set_units(revaluation->previous_rate.raw());
            change->mutable_rate()->set_units(revaluation->rate.raw());
            change->mutable_margin_before()->set_units(revaluation->margin_before.raw());
            change->mutable_available_before()->set_units(revaluation->available_before.raw());
        }
        auto* occupancy = settled->mutable_occupancy();
        occupancy->mutable_margin()->set_units(account.margin().raw());
        occupancy->mutable_available()->set_units(account.available().raw());
        occupancy->mutable_frozen()->set_units(account.frozen().raw());
        for (std::size_t c = 0; c < account.contracts().size(); ++c) {
            // A month of a series has no row on the days it is not in the replay.
            if (!day.prices[c])
                continue;
            const auto& id = account.contracts()[c].instrument.id;
            // Net lots: long positive, short negative.
            Decimal quantity, gross_quantity, margin;
            for (const auto& lot : account.positions())
                if (lot.instrument == id) {
                    quantity =
                        lot.side == Side::buy ? quantity + lot.quantity : quantity - lot.quantity;
                    gross_quantity = gross_quantity + lot.quantity;
                    const auto& terms = account.contracts()[c];
                    margin = margin
                        + terms.costs.margin(
                            lot.quantity,
                            *day.prices[c],
                            terms.instrument.multiplier);
                }
            auto* row = settled->add_contracts();
            row->set_venue(id.venue);
            row->set_symbol(id.symbol);
            row->mutable_price()->set_units(day.prices[c]->raw());
            row->mutable_position_quantity()->set_units(quantity.raw());
            row->mutable_gross_quantity()->set_units(gross_quantity.raw());
            row->mutable_margin()->set_units(margin.raw());
        }
        // From the next day the account may follow another strategy: what that one
        // wanted after each contract's last bar is what its next bar fills.
        if (d + 1 < schedule.days() && followed[d + 1] != whom)
            for (const auto& item : last)
                if (item)
                    issue(*item, followed[d + 1]);
        if (!until.empty()
            && (d + 1 == schedule.days() || schedule.day(d + 1).trading_day >= until))
            return result;
    }
    auto* summary = result.mutable_account();
    *summary = protocol::encode_snapshot(account, p);
    summary->set_cursor(static_cast<std::uint32_t>(execution.cursor()));
    summary->set_total(static_cast<std::uint32_t>(total));
    if (const auto timestamp = execution.timestamp_ns())
        summary->set_timestamp_ns(*timestamp);
    result.mutable_max_drawdown()->set_units(drawdown.raw());
    for (auto& follower : followers)
        for (auto& strategy : follower.strategies)
            strategy->stop();
    execution.stop();
    return result;
}

} // namespace

namespace {

// What a candidate's days before the holdout, or a round's training days,
// showed of it.
backtest::proto::BacktestCandidate scored(
    Decimal base,
    std::span<const EquityDay> days,
    std::span<const Decimal> marks,
    const PerformanceContext& context)
{
    backtest::proto::BacktestCandidate candidate;
    // An account already emptied has no return to speak of.
    if (base <= Decimal{})
        return candidate;
    const auto record = performance(base, days, marks, context);
    candidate.set_total_return(record.total_return);
    candidate.set_max_drawdown(record.max_drawdown);
    if (record.sharpe)
        candidate.set_sharpe(*record.sharpe);
    return candidate;
}

// The highest ratio wins; among equals, the one named first. None when no
// candidate has one.
template <class Candidates>
std::optional<std::size_t> best(const Candidates& candidates)
{
    std::optional<std::size_t> selected;
    for (int i = 0; i < static_cast<int>(candidates.size()); ++i)
        if (candidates[i].has_sharpe()
            && (!selected
                || candidates[i].sharpe() > candidates[static_cast<int>(*selected)].sharpe()))
            selected = static_cast<std::size_t>(i);
    return selected;
}

// The equity record of one replay, by trading day.
struct Curve
{
    std::int64_t first_observation_ns = 0;
    std::vector<std::int64_t> settlement_times;
    std::vector<EquityDay> days;
    // Every equity observation in order, and where each day's end.
    std::vector<Decimal> marks;
    std::vector<std::size_t> day_end;
};

Curve curve(const backtest::proto::BacktestResult& result)
{
    Curve value;
    value.first_observation_ns = result.equity(0).timestamp_ns();
    for (const auto& point : result.equity()) {
        value.marks.push_back(decimal(point.equity()));
        if (point.event() == backtest::proto::DAILY_SETTLEMENT)
            value.day_end.push_back(value.marks.size());
    }
    for (const auto& day : result.settlements()) {
        value.days.push_back(
            {std::chrono::sys_days(parse_trading_date(day.trading_day())), decimal(day.equity())});
        value.settlement_times.push_back(day.timestamp_ns());
    }
    return value;
}

PerformanceContext performance_context(
    const backtest::proto::BacktestInput& input,
    const Curve& record,
    std::size_t first,
    std::size_t end)
{
    PerformanceContext context;
    context.begin_ns = first ? record.settlement_times[first - 1] : record.first_observation_ns;
    context.end_ns = record.settlement_times[end - 1];
    context.opening_is_full_period = first > 0;
    const auto& policy = input.performance_assumptions();
    if (policy.has_annual_periods())
        context.annual_periods = policy.annual_periods();
    context.explicitly_zero_risk_free =
        policy.risk_free() == backtest::proto::PerformanceAssumptions::EXPLICIT_ZERO;
    return context;
}

} // namespace

backtest::proto::BacktestResult run(
    const backtest::proto::BacktestInput& input,
    std::stop_token stop,
    const std::function<void(std::size_t, std::size_t)>& progress)
{
    validate(input);
    const auto units = protocol::backtest_work_units(input);
    std::size_t completed = 0;
    const auto step = [&] {
        if (progress)
            progress(++completed, units);
    };
    const auto days = protocol::backtest_trading_days(input).size();
    const auto deposit = decimal(input.paper().deposit());
    // One strategy alone on the account, over all the days or those before `until`.
    const auto alone = [&](const protocol::proto::Strategy& strategy, const std::string& until) {
        return replay(
            input,
            {&strategy},
            std::vector<std::optional<std::size_t>>(days, 0),
            until,
            stop,
            step);
    };
    if (input.strategies_size() == 1)
        return alone(input.strategies(0), {});
    if (!input.has_walk_forward()) {
        // Each strategy is judged by the days before the holdout and nothing
        // else: its replay ends there.
        std::vector<backtest::proto::BacktestCandidate> candidates;
        for (const auto& strategy : input.strategies()) {
            const auto development = curve(alone(strategy, input.holdout_day()));
            candidates.push_back(scored(
                deposit,
                development.days,
                development.marks,
                performance_context(input, development, 0, development.days.size())));
        }
        const auto selected = best(candidates);
        if (!selected)
            throw std::invalid_argument(
                "no compared strategy has a development Sharpe ratio: none of them varied before the " "holdout");
        auto result = alone(input.strategies(static_cast<int>(*selected)), {});
        for (auto& candidate : candidates)
            *result.add_candidates() = std::move(candidate);
        result.set_selected(static_cast<unsigned>(*selected));
        return result;
    }
    // Rolling: every strategy is first replayed alone over all the days. A round
    // reads of those replays its training days and nothing after them, and the
    // account then follows the round's best strategy over its validation days.
    std::vector<Curve> curves;
    std::vector<const protocol::proto::Strategy*> definitions;
    for (const auto& strategy : input.strategies()) {
        curves.push_back(curve(alone(strategy, {})));
        definitions.push_back(&strategy);
    }
    const auto training = input.walk_forward().training_days();
    std::vector<std::optional<std::size_t>> followed(days);
    std::vector<backtest::proto::BacktestFold> folds;
    for (const auto& range : protocol::backtest_folds(input)) {
        auto& fold = folds.emplace_back();
        fold.set_first_day(
            format_trading_date(std::chrono::year_month_day{curves.front().days[range.first].day}));
        const auto begin = range.first - training;
        const auto date = [&](std::size_t index) {
            return format_trading_date(std::chrono::year_month_day{curves.front().days[index].day});
        };
        fold.set_training_first_day(date(begin));
        fold.set_training_last_day(date(range.first - 1));
        fold.set_last_day(date(range.end - 1));
        fold.set_training_days(training);
        fold.set_validation_days(static_cast<unsigned>(range.end - range.first));
        for (const auto& record : curves) {
            const auto first_mark = begin ? record.day_end[begin - 1] : 0;
            *fold.add_candidates() = scored(
                begin ? record.days[begin - 1].equity : deposit,
                std::span(record.days).subspan(begin, training),
                std::span(record.marks)
                    .subspan(first_mark, record.day_end[range.first - 1] - first_mark),
                performance_context(input, record, begin, range.first));
        }
        // A round in which no strategy has a ratio follows none: the account
        // holds nothing through it.
        if (const auto selected = best(fold.candidates())) {
            fold.set_selected(static_cast<unsigned>(*selected));
            std::fill(
                followed.begin() + static_cast<std::ptrdiff_t>(range.first),
                followed.begin() + static_cast<std::ptrdiff_t>(range.end),
                selected);
        }
    }
    auto result = replay(input, definitions, followed, {}, stop, step);
    for (auto& fold : folds)
        *result.add_folds() = std::move(fold);
    return result;
}

} // namespace asterion::backtest
