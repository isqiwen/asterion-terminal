#include "engine.hpp"
#include <asterion/protocol/data.hpp>
#include "strategy.hpp"
#include <asterion/futures/portfolio_targets.hpp>
#include <asterion/futures/performance.hpp>
#include <asterion/futures/trading_days.hpp>
#include <asterion/futures/order_limits.hpp>
#include <asterion/futures/replay.hpp>
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
    std::vector<DominantSeries> series;
    for (const auto& schedule : input.series()) {
        auto& item = series.emplace_back();
        for (const auto& roll : schedule.rolls())
            item.rolls.push_back({roll.trading_day(), roll.contract(), decimal(roll.factor())});
    }
    std::map<std::string, Decimal> margins;
    for (const auto& fixed : input.settlement_margins())
        margins.emplace(fixed.trading_day(), decimal(fixed.rate()));
    auto risk = std::make_shared<OrderLimits>(decode_order_limits(protocol::decode_risk(p.risk())));
    risk->start();
    Replay replay(
        decimal(p.deposit()),
        std::move(portfolio.contracts),
        portfolio.days,
        std::move(series),
        risk,
        std::move(margins));
    const auto& execution = replay.execution();
    const auto& account = execution.account();
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
    // A unit's strategy is made for its contract, or for a series' first month.
    std::vector<Instrument> units(replay.units());
    for (std::size_t c = 0; c < static_cast<std::size_t>(p.contracts_size()); ++c)
        if (!replay.series(c))
            units[replay.unit(c)] = execution.contract(c).terms.instrument;
    for (const auto& rolls : input.series()) {
        const auto first = rolls.rolls(0).contract();
        units[replay.unit(first)] = execution.contract(first).terms.instrument;
    }
    std::vector<Follower> followers;
    for (const auto* definition : definitions) {
        auto& follower = followers.emplace_back();
        follower.definition = definition;
        follower.wanted.resize(replay.units());
        if (definition->has_fixed_weights())
            continue;
        if (CrossSection::defines(*definition))
            follower.cross = std::make_unique<CrossSection>(*definition, replay.units());
        else
            for (const auto& instrument : units)
                follower.strategies.push_back(make_strategy(*definition, instrument));
    }
    for (auto& follower : followers)
        for (auto& strategy : follower.strategies)
            strategy->start();
    // What the account wants of every unit while it follows `whom`: its
    // targets, and nothing while it follows no one.
    const auto follow = [&](const std::optional<std::size_t>& whom) {
        for (std::size_t u = 0; u < replay.units(); ++u)
            replay.want(u, whom ? followers[*whom].wanted[u] : std::optional{Decimal{}});
    };
    if (!followed.empty())
        follow(followed.front());

    backtest::proto::BacktestResult result;
    result.set_dataset_revision(input.dataset_revision());
    const bool fixed_portfolio =
        input.strategies_size() == 1 && input.strategies(0).has_fixed_weights();
    const bool record_fill_days = std::ranges::all_of(p.contracts(), [](const auto& contract) {
        return contract.dataset().interval_minutes() == 0;
    });

    // What the replay does, recorded as the result.
    struct Recorder final : Replay::Observer
    {
        const PaperExecution& execution;
        backtest::proto::BacktestResult& result;
        bool fixed_portfolio, record_fill_days;
        const std::function<void()>& step;
        Decimal peak, drawdown;
        std::size_t recorded_fills = 0, recorded_portfolio_fills = 0;
        std::map<std::string, std::size_t> portfolio_order_origins;

        Recorder(
            const PaperExecution& execution,
            backtest::proto::BacktestResult& result,
            bool fixed_portfolio,
            bool record_fill_days,
            const std::function<void()>& step,
            Decimal deposit)
            : execution(execution),
              result(result),
              fixed_portfolio(fixed_portfolio),
              record_fill_days(record_fill_days),
              step(step),
              peak(deposit)
        {}

        void equity(std::int64_t time, backtest::proto::EquityEvent event)
        {
            const auto& account = execution.account();
            const auto value = account.balance() + account.unrealized();
            peak = std::max(peak, value);
            drawdown = std::max(drawdown, peak - value);
            auto* point = result.add_equity();
            point->set_timestamp_ns(time);
            point->set_event(event);
            point->mutable_equity()->set_units(value.raw());
        }

        void advanced(
            std::size_t index,
            const std::optional<PaperExecution::Target>& intent,
            const RiskDecision& risk_decision,
            std::size_t previous_orders) override
        {
            const auto& account = execution.account();
            const auto current = execution.event(index);
            const auto& bar = execution.bar(current);
            const auto& instrument = execution.contract(current.contract).terms.instrument.id;
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
            if (risk_decision.allowed())
                return;
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

        void marked(std::size_t index) override
        {
            const auto& account = execution.account();
            const auto& bar = execution.bar(execution.event(index));
            if (record_fill_days)
                while (recorded_fills < account.fills().size()) {
                    const auto& fill = account.fills()[recorded_fills++];
                    auto* observed = result.add_fill_days();
                    observed->set_fill_id(fill.execution.execution_id);
                    observed->set_trading_day(bar.trading_day);
                }
            equity(bar.timestamp_ns, backtest::proto::TRADE_MARK);
            step();
        }

        void settled(
            const PaperReplaySchedule::Day& day,
            std::int64_t settled_at,
            const std::optional<PaperExecution::MarginRevaluation>& revaluation) override
        {
            const auto& account = execution.account();
            equity(settled_at, backtest::proto::DAILY_SETTLEMENT);
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
                        quantity = lot.side == Side::buy ? quantity + lot.quantity
                                                         : quantity - lot.quantity;
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
        }
    } recorder(execution, result, fixed_portfolio, record_fill_days, step, decimal(p.deposit()));

    // Every day one of the contracts settles on, in order: its bars, where any
    // contract has some, then its settlement.
    for (std::size_t d = 0; !replay.finished(); ++d) {
        const auto& whom = followed[d];
        while (!replay.day_done()) {
            const auto& bars = replay.next(recorder, stop);
            const auto observe = [&](const Replay::Bar& item) {
                const auto u = item.unit;
                for (auto& follower : followers) {
                    if (follower.definition && follower.definition->has_fixed_weights())
                        continue;
                    if (!follower.cross) {
                        if (const auto wanted = follower.strategies[u]->on_bar(item.seen))
                            follower.wanted[u] = wanted;
                    } else if (follower.definition->cross().factor()
                        != protocol::proto::TERM_STRUCTURE) {
                        follower.cross->on_bar(u, item.seen.timestamp_ns, item.seen.close);
                    } else if (const auto s = replay.series(item.contract)) {
                        // A day without a term point is no bar of this unit for the ranking.
                        if (const auto term = carry[*s].find(item.seen.trading_day);
                            term != carry[*s].end())
                            follower.cross
                                ->on_bar(u, item.seen.timestamp_ns, item.seen.close, term->second);
                    }
                }
            };
            for (const auto& item : bars)
                if (item.leading)
                    observe(item);
            const auto& last = bars.back();
            const bool more = last.index + 1 < execution.size();
            bool decided =
                !(whom && followers[*whom].definition
                    && followers[*whom].definition->has_fixed_weights());
            for (std::size_t k = 0; k < followers.size(); ++k) {
                auto& follower = followers[k];
                if (follower.definition && follower.definition->has_fixed_weights()) {
                    const auto& policy = follower.definition->fixed_weights();
                    ++follower.portfolio_observations;
                    if (follower.portfolio_observations % policy.rebalance() != 0 || whom != k)
                        continue;
                    if (bars.size() != static_cast<std::size_t>(p.contracts_size()))
                        throw std::logic_error(
                            "fixed-weight decision is missing a completed member bar");
                    std::vector<WeightedContract> members;
                    for (const auto& item : bars) {
                        const auto& terms = execution.contract(item.contract).terms.instrument;
                        const auto fixed =
                            std::ranges::find_if(policy.members(), [&](const auto& m) {
                                return m.venue() == terms.id.venue && m.symbol() == terms.id.symbol;
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
                    observed->set_boundary_ns(last.seen.timestamp_ns);
                    observed->set_trading_day(last.seen.trading_day);
                    observed->set_has_next_bar(more);
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
                        const auto& item = bars[m];
                        auto* member = observed->add_members();
                        member->set_venue(target.contract.instrument.venue);
                        member->set_symbol(target.contract.instrument.symbol);
                        member->set_intent_id("strategy." + std::to_string(item.index));
                        member->set_source_index(static_cast<unsigned>(item.index));
                        member->mutable_weight()->set_units(target.contract.weight.raw());
                        member->mutable_price()->set_units(target.contract.price.raw());
                        member->mutable_multiplier()->set_units(target.contract.multiplier.raw());
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
                                *original_state.add_positions() = protocol::encode_position(lot);
                        for (const auto index : account.working_orders()) {
                            const auto& accepted = account.orders()[index];
                            if (accepted.order.request().instrument == target.contract.instrument)
                                *original_state.add_working_orders() = protocol::encode_order(
                                    {accepted.order.request(),
                                        accepted.offset,
                                        accepted.order.filled_quantity(),
                                        accepted.order.state()});
                        }
                        follower.wanted[item.unit] = target.quantity;
                    }
                    decided = true;
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
                for (const auto& item : bars) {
                    if (!item.leading)
                        continue;
                    const auto share = (*shares)[item.unit];
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
                    follower.wanted[item.unit] = share > 0 ? lots
                        : share < 0                        ? Decimal{} - lots
                                                           : Decimal{};
                }
            }
            follow(whom);
            if (decided)
                replay.decide();
        }
        replay.settle(recorder);
        // From the next day the account may follow another strategy: what that one
        // wanted after each contract's last bar is what its next bar fills.
        if (!replay.finished() && followed[d + 1] != whom) {
            follow(followed[d + 1]);
            replay.redecide();
        }
        if (!until.empty() && (replay.finished() || replay.day().trading_day >= until))
            return result;
    }
    auto* summary = result.mutable_account();
    *summary = protocol::encode_snapshot(account, p);
    summary->set_cursor(static_cast<std::uint32_t>(execution.cursor()));
    summary->set_total(static_cast<std::uint32_t>(execution.size()));
    if (const auto timestamp = execution.timestamp_ns())
        summary->set_timestamp_ns(*timestamp);
    result.mutable_max_drawdown()->set_units(recorder.drawdown.raw());
    for (auto& follower : followers)
        for (auto& strategy : follower.strategies)
            strategy->stop();
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
