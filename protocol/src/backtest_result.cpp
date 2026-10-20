#include <asterion/protocol/backtest.hpp>
#include <asterion/domain/trading_days.hpp>
#include <algorithm>
#include <set>
#include <stdexcept>

namespace asterion::protocol {

void validate_backtest_result(
    const backtest::proto::BacktestInput& input,
    const backtest::proto::BacktestResult& result)
{
    protocol::validate_message(result);
    // Validate fixed input before comparing its result evidence.
    (void)protocol::decode_backtest(input, protocol::DatasetView::metadata);
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
            && result.folds(static_cast<int>(i)).candidates_size() == strategies;
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
