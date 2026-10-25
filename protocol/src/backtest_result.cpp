#include <asterion/protocol/backtest.hpp>
#include <asterion/domain/trading_days.hpp>
#include <algorithm>
#include <set>
#include <stdexcept>
#include <optional>
#include <map>

namespace asterion::protocol {
namespace {

void validate_portfolio_targets(
    const backtest::proto::BacktestInput& input,
    const backtest::proto::BacktestResult& result)
{
    const bool enabled = input.strategies_size() == 1 && input.strategies(0).has_fixed_weights();
    if (!enabled) {
        if (!result.portfolio_targets().empty() || !result.portfolio_fills().empty()
            || !result.portfolio_orders().empty())
            throw std::invalid_argument("unexpected fixed-weight target evidence");
        return;
    }
    const auto& policy = input.strategies(0).fixed_weights();
    const auto& data = input.paper().contracts(0).dataset();
    const auto count = static_cast<std::size_t>(input.paper().contracts_size());
    if (result.portfolio_targets_size() != data.bars_size() / static_cast<int>(policy.rebalance()))
        throw std::invalid_argument("fixed-weight result is missing original decision boundaries");
    const auto decimal = [](const auto& v) {
        return Decimal::from_raw(v.units());
    };
    const auto magnitude = [](Decimal v) {
        return v < Decimal{} ? Decimal{} - v : v;
    };
    const auto invalid = [] {
        throw std::invalid_argument(
            "fixed-weight target evidence differs from its original boundary or policy");
    };
    if (result.portfolio_fills_size() != result.account().fills_size())
        invalid();
    std::map<std::string, const proto::Order*> actual_orders;
    for (const auto& order : result.account().orders())
        if (!actual_orders.emplace(order.id(), &order).second)
            invalid();
    std::vector<Decimal> net_held(count);
    std::size_t fill_cursor = 0;
    std::size_t curve_cursor = 0;
    std::map<std::string, const backtest::proto::PortfolioTargetMember*> intentions;
    for (int d = 0; d < result.portfolio_targets_size(); ++d) {
        const auto& decision = result.portfolio_targets(d);
        const auto b = (static_cast<std::size_t>(d) + 1) * policy.rebalance() - 1;
        const auto& boundary = data.bars(static_cast<int>(b));
        // A linear pass over original executions checks position conservation;
        // it does not recompute basis, fees or account valuation.
        while (fill_cursor < static_cast<std::size_t>(result.portfolio_fills_size())
            && result.portfolio_fills(static_cast<int>(fill_cursor)).source_index()
                < (b + 1) * count) {
            const auto& fill = result.account().fills(static_cast<int>(fill_cursor));
            const auto& link = result.portfolio_fills(static_cast<int>(fill_cursor++));
            const auto order = actual_orders.find(fill.order_id());
            const auto contract = link.source_index() % count;
            const auto& terms =
                input.paper().contracts(static_cast<int>(contract)).dataset().contract();
            if (order == actual_orders.end() || !fill.has_quantity() || fill.quantity().units() <= 0
                || fill.venue() != terms.venue() || fill.symbol() != terms.symbol()
                || (order->second->side() != proto::BUY && order->second->side() != proto::SELL))
                invalid();
            const auto quantity = decimal(fill.quantity());
            net_held[contract] = net_held[contract]
                + (order->second->side() == proto::BUY ? quantity : Decimal{} - quantity);
        }
        std::optional<Decimal> observed_equity;
        while (curve_cursor < static_cast<std::size_t>(result.equity_size())
            && result.equity(static_cast<int>(curve_cursor)).timestamp_ns()
                <= boundary.timestamp_ns()) {
            const auto& point = result.equity(static_cast<int>(curve_cursor++));
            if (point.timestamp_ns() == boundary.timestamp_ns()
                && point.event() == backtest::proto::TRADE_MARK)
                observed_equity = decimal(point.equity());
        }
        if (!observed_equity || !decision.has_equity()
            || decimal(decision.equity()) != *observed_equity || *observed_equity <= Decimal{}
            || decision.boundary_ns() != boundary.timestamp_ns()
            || decision.trading_day() != boundary.trading_day()
            || decision.has_next_bar() != (b + 1 < static_cast<std::size_t>(data.bars_size()))
            || decision.members_size() != static_cast<int>(count) || !decision.has_gross_weight()
            || !decision.has_net_weight() || !decision.has_gross_limit()
            || !decision.has_net_limit() || !decision.has_requested_gross_notional()
            || !decision.has_requested_net_notional() || !decision.has_allocated_gross_notional()
            || !decision.has_allocated_net_notional() || !decision.has_unallocated_nominal_budget()
            || decision.gross_limit().units() != policy.gross_weight_limit().units()
            || decision.net_limit().units() != policy.net_weight_limit().units())
            invalid();
        Decimal gross_weight, net_weight, requested_gross, requested_net, allocated_gross,
            allocated_net;
        for (std::size_t c = 0; c < count; ++c) {
            const auto& member = decision.members(static_cast<int>(c));
            const auto& original = input.paper().contracts(static_cast<int>(c)).dataset();
            const auto& terms = original.contract();
            const auto fixed = std::ranges::find_if(policy.members(), [&](const auto& m) {
                return m.venue() == terms.venue() && m.symbol() == terms.symbol();
            });
            if (fixed == policy.members().end() || member.venue() != terms.venue()
                || member.symbol() != terms.symbol() || member.source_index() != b * count + c
                || member.intent_id() != "strategy." + std::to_string(b * count + c)
                || !member.has_weight() || !member.has_price() || !member.has_multiplier()
                || !member.has_requested_notional() || !member.has_quantity()
                || !member.has_allocated_notional() || !member.has_nominal_remainder()
                || member.weight().units() != fixed->weight().units()
                || member.price().units() != original.bars(static_cast<int>(b)).close().units()
                || member.multiplier().units() != terms.multiplier().units()
                || member.quantity().units() % 100000000 != 0 || member.price().units() <= 0
                || member.multiplier().units() <= 0)
                invalid();
            if (!member.has_observation() || !member.observation().working_orders().empty())
                invalid(); // Current complete daily boundaries are observed after DAY expiry.
            Decimal observed_net, observed_gross;
            for (const auto& lot : member.observation().positions()) {
                if (lot.venue() != terms.venue() || lot.symbol() != terms.symbol()
                    || !lot.has_quantity() || !lot.has_basis() || lot.quantity().units() <= 0
                    || lot.quantity().units() % 100000000 != 0
                    || (lot.side() != proto::BUY && lot.side() != proto::SELL))
                    invalid();
                const auto quantity = decimal(lot.quantity());
                observed_net =
                    observed_net + (lot.side() == proto::BUY ? quantity : Decimal{} - quantity);
                observed_gross = observed_gross + quantity;
            }
            if (observed_net != net_held[c] || observed_gross != magnitude(observed_net))
                invalid();
            intentions.emplace(member.intent_id(), &member);
            const auto requested = decimal(member.requested_notional());
            const auto per_lot = decimal(member.price()) * decimal(member.multiplier());
            const auto allocated = decimal(member.allocated_notional()),
                       remainder = decimal(member.nominal_remainder());
            if (requested != *observed_equity * decimal(member.weight())
                || allocated != decimal(member.quantity()) * per_lot
                || remainder != requested - allocated || magnitude(remainder) >= per_lot
                || (requested < Decimal{} ? remainder > Decimal{} : remainder < Decimal{}))
                invalid();
            gross_weight = gross_weight + magnitude(decimal(member.weight()));
            net_weight = net_weight + decimal(member.weight());
            requested_gross = requested_gross + magnitude(requested);
            requested_net = requested_net + requested;
            allocated_gross = allocated_gross + magnitude(allocated);
            allocated_net = allocated_net + allocated;
        }
        if (decimal(decision.gross_weight()) != gross_weight
            || decimal(decision.net_weight()) != net_weight
            || gross_weight > decimal(decision.gross_limit())
            || magnitude(net_weight) > decimal(decision.net_limit())
            || decimal(decision.requested_gross_notional()) != requested_gross
            || decimal(decision.requested_net_notional()) != requested_net
            || decimal(decision.allocated_gross_notional()) != allocated_gross
            || decimal(decision.allocated_net_notional()) != allocated_net
            || decimal(decision.unallocated_nominal_budget())
                != *observed_equity * decimal(decision.gross_limit()) - requested_gross)
            invalid();
    }
    if (result.portfolio_orders_size() != result.account().orders_size()
        || result.portfolio_fills_size() != result.account().fills_size()
        || result.fill_days_size() != result.account().fills_size())
        invalid();
    std::map<std::string, const backtest::proto::PortfolioTargetOrder*> orders;
    for (int i = 0; i < result.portfolio_orders_size(); ++i) {
        const auto& accepted = result.portfolio_orders(i);
        const auto& actual = result.account().orders(i);
        const auto found = intentions.find(accepted.intent_id());
        if (found == intentions.end() || accepted.order_id() != actual.id()
            || !orders.emplace(accepted.order_id(), &accepted).second)
            invalid();
        const auto& target = *found->second;
        const auto contract = target.source_index() % count;
        const auto bar_index = target.source_index() / count + 1;
        if (bar_index >= static_cast<std::size_t>(data.bars_size())
            || accepted.decision_source_index() != target.source_index()
            || accepted.source_index() != target.source_index() + count
            || actual.venue() != target.venue() || actual.symbol() != target.symbol()
            || accepted.boundary_ns()
                != input.paper()
                    .contracts(static_cast<int>(contract))
                    .dataset()
                    .bars(static_cast<int>(bar_index))
                    .timestamp_ns())
            invalid();
    }
    for (int i = 0; i < result.portfolio_fills_size(); ++i) {
        const auto& observed = result.portfolio_fills(i);
        const auto& fill = result.account().fills(i);
        const auto found = orders.find(fill.order_id());
        if (found == orders.end())
            invalid();
        const auto& accepted = *found->second;
        const auto& target = *intentions.at(accepted.intent_id());
        const auto contract = target.source_index() % count;
        const auto source_bar = observed.source_index() / count;
        if (source_bar >= static_cast<std::size_t>(data.bars_size())
            || observed.fill_id() != fill.id() || observed.intent_id() != accepted.intent_id()
            || observed.decision_source_index() != accepted.decision_source_index()
            || observed.source_index() < accepted.source_index()
            || observed.source_index() % count != contract || fill.venue() != target.venue()
            || fill.symbol() != target.symbol())
            invalid();
        const auto& original = input.paper()
                                   .contracts(static_cast<int>(contract))
                                   .dataset()
                                   .bars(static_cast<int>(source_bar));
        const auto& admitted_bar = input.paper()
                                       .contracts(static_cast<int>(contract))
                                       .dataset()
                                       .bars(static_cast<int>(accepted.source_index() / count));
        if (observed.boundary_ns() != original.timestamp_ns()
            || original.trading_day() != admitted_bar.trading_day()
            || result.fill_days(i).fill_id() != fill.id()
            || result.fill_days(i).trading_day() != original.trading_day())
            invalid();
    }
}

} // namespace

void validate_backtest_result(
    const backtest::proto::BacktestInput& input,
    const backtest::proto::BacktestResult& result)
{
    protocol::validate_message(result);
    // Validate fixed input before comparing its result evidence.
    (void)protocol::decode_backtest(input, protocol::DatasetView::metadata);
    validate_portfolio_targets(input, result);
    const auto& paper = input.paper();
    // The replay settles every day any contract settles on, with bars that day
    // or without, and costs follow its last day.
    std::set<std::string> trading_days;
    for (const auto& contract : paper.contracts())
        for (const auto& day : contract.dataset().days())
            trading_days.insert(day.trading_day());
    const auto days = static_cast<int>(trading_days.size());
    const auto last_day = trading_days.empty() ? std::string() : *trading_days.rbegin();
    std::uint32_t bars = 0;
    for (const auto& contract : paper.contracts())
        bars += static_cast<std::uint32_t>(contract.dataset().bars_size());
    // A comparison scores every strategy it was given: once before a holdout,
    // or in every round of a rolling one. One strategy has no scores.
    const auto strategies = input.strategies_size();
    const bool rolling = input.has_walk_forward();
    const auto compared = strategies > 1 && !rolling ? strategies : 0;
    const auto rounds =
        rolling ? protocol::backtest_folds(input) : std::vector<protocol::BacktestRound>{};
    const std::vector<std::string> ordered(trading_days.begin(), trading_days.end());
    bool folds = static_cast<std::size_t>(result.folds_size()) == rounds.size();
    for (std::size_t i = 0; folds && i < rounds.size(); ++i)
        folds = result.folds(static_cast<int>(i)).first_day() == ordered[rounds[i].first]
            && result.folds(static_cast<int>(i)).candidates_size() == strategies
            && result.folds(static_cast<int>(i)).training_first_day()
                == ordered[rounds[i].first - input.walk_forward().training_days()]
            && result.folds(static_cast<int>(i)).training_last_day() == ordered[rounds[i].first - 1]
            && result.folds(static_cast<int>(i)).last_day() == ordered[rounds[i].end - 1]
            && result.folds(static_cast<int>(i)).training_days()
                == input.walk_forward().training_days()
            && result.folds(static_cast<int>(i)).validation_days()
                == rounds[i].end - rounds[i].first;
    bool contracts = result.account().contracts_size() == paper.contracts_size();
    for (int c = 0; contracts && c < paper.contracts_size(); ++c)
        contracts = result.account().contracts(c).contract().SerializeAsString()
                == paper.contracts(c).dataset().contract().SerializeAsString()
            && result.account().contracts(c).costs().SerializeAsString()
                == protocol::encode_costs(
                    protocol::backtest_settlement_costs(
                        input,
                        static_cast<std::size_t>(c),
                        last_day)
                        .values)
                       .SerializeAsString()
            && result.account().contracts(c).cost_schedule().SerializeAsString()
                == paper.contracts(c).cost_schedule().SerializeAsString();
    if (result.dataset_revision() != input.dataset_revision() || result.account().cursor() != bars
        || result.account().total() != bars || result.equity_size() != static_cast<int>(bars) + days
        || result.settlements_size() != days || !contracts || result.candidates_size() != compared
        || !folds || result.account().risk().SerializeAsString() != paper.risk().SerializeAsString()
        || !result.has_max_drawdown() || result.account().recovery_required())
        throw std::invalid_argument("incomplete or mismatched task result");
    static_cast<void>(protocol::decode_backtest_result(result));
    auto peak = Decimal::from_raw(input.paper().deposit().units());
    Decimal drawdown;
    for (int i = 0; i < result.equity_size(); ++i) {
        const auto& point = result.equity(i);
        if (!point.has_equity()
            || (i > 0 && point.timestamp_ns() < result.equity(i - 1).timestamp_ns()))
            throw std::invalid_argument("result curve does not match input timeline");
        const auto equity = Decimal::from_raw(point.equity().units());
        peak = std::max(peak, equity);
        drawdown = std::max(drawdown, peak - equity);
    }
    if (result.equity().rbegin()->equity().units() != result.account().equity().units()
        || drawdown.raw() != result.max_drawdown().units()
        || result.account().frozen().units() != 0)
        throw std::invalid_argument("inconsistent result summary");
    for (const auto& order : result.account().orders())
        if (order.state() == protocol::proto::ACCEPTED
            || order.state() == protocol::proto::PARTIALLY_FILLED)
            throw std::invalid_argument("result retains active orders");
    std::vector<const protocol::proto::Bar*> timeline;
    for (const auto& contract : paper.contracts())
        for (const auto& bar : contract.dataset().bars())
            timeline.push_back(&bar);
    std::ranges::stable_sort(timeline, {}, [](const auto* bar) {
        return bar->timestamp_ns();
    });
    std::set<std::string> rejected_intents;
    for (const auto& rejected : result.target_rejections()) {
        if (rejected.source_index() >= timeline.size()
            || rejected.boundary_ns() != timeline[rejected.source_index()]->timestamp_ns()
            || !rejected_intents.insert(rejected.intent_id()).second)
            throw std::invalid_argument("target rejection differs from original replay boundary");
    }
    // A mark at every bar, and each day's settlement after its bars: at the
    // last of them, or where the day has none, at the close of its session.
    int point_index = 0, settlement_index = 0;
    std::size_t next = 0;
    for (const auto& trading_day : trading_days) {
        const auto check_point =
            [&](asterion::backtest::proto::EquityEvent event,
                std::int64_t at) -> const asterion::backtest::proto::EquityPoint& {
            if (point_index >= result.equity_size())
                throw std::invalid_argument("result curve does not match input timeline");
            const auto& point = result.equity(point_index++);
            if (point.timestamp_ns() != at || point.event() != event)
                throw std::invalid_argument("result curve does not match input timeline");
            return point;
        };
        auto settled_at = trading_day_close_ns(trading_day);
        for (; next < timeline.size() && timeline[next]->trading_day() == trading_day; ++next) {
            settled_at = timeline[next]->timestamp_ns();
            check_point(asterion::backtest::proto::TRADE_MARK, settled_at);
        }
        const auto& point = check_point(asterion::backtest::proto::DAILY_SETTLEMENT, settled_at);
        const auto& day = result.settlements(settlement_index++);
        if (day.timestamp_ns() != point.timestamp_ns()
            || day.equity().units() != point.equity().units())
            throw std::invalid_argument("inconsistent result summary");
    }
    if (next != timeline.size() || point_index != result.equity_size())
        throw std::invalid_argument("result curve does not match input timeline");
    // Settlement evidence must name the input's exact dates, contracts and prices.
    int day_index = 0;
    for (const auto& day : trading_days) {
        const auto& settlement = result.settlements(day_index++);
        if (settlement.trading_day() != day)
            throw std::invalid_argument("settlement result does not match input days");
        const auto event = std::ranges::find_if(input.settlement_margins(), [&](const auto& fixed) {
            return fixed.trading_day() == day;
        });
        if (settlement.has_margin_revaluation() != (event != input.settlement_margins().end()))
            throw std::invalid_argument("settlement margin result does not match fixed events");
        if (settlement.has_margin_revaluation()) {
            const auto before =
                costs_on(cost_schedule(paper.contracts(0).cost_schedule()), day).values;
            const auto& change = settlement.margin_revaluation();
            if (change.previous_rate().units() != before.margin_rate.raw()
                || change.rate().units() != event->rate().units()
                || settlement.occupancy().frozen().units() != 0)
                throw std::invalid_argument("settlement margin result does not match fixed events");
        }
        int contract_index = 0;
        for (const auto& contract : paper.contracts()) {
            const auto found =
                std::ranges::find_if(contract.dataset().days(), [&](const auto& value) {
                    return value.trading_day() == day;
                });
            if (found == contract.dataset().days().end())
                continue;
            if (contract_index == settlement.contracts_size())
                throw std::invalid_argument("settlement result does not match input contracts");
            const auto& value = settlement.contracts(contract_index++);
            if (value.venue() != contract.dataset().contract().venue()
                || value.symbol() != contract.dataset().contract().symbol()
                || value.price().units() != found->settlement_price().units())
                throw std::invalid_argument("settlement result does not match input contracts");
        }
        if (contract_index != settlement.contracts_size())
            throw std::invalid_argument("settlement result does not match input contracts");
    }
}

} // namespace asterion::protocol
