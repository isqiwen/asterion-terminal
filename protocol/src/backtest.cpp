#include <asterion/domain/trading_days.hpp>
#include <asterion/foundation/decimal.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/protocol/data.hpp>
#include <asterion/protocol/factor.hpp>
#include <asterion/protocol/backtest.hpp>
#include <charconv>
#include <cmath>
#include <set>
#include <stdexcept>

namespace asterion::protocol {
namespace {

// Index lists: every index names a contract, none twice across all lists.
template <class Lists, class Indexes>
void distinct_contracts(const Lists& lists, std::size_t contracts, Indexes indexes)
{
    std::vector<bool> used(contracts);
    for (const auto& list : lists)
        for (const auto index : indexes(list)) {
            if (index >= contracts || used[index])
                throw std::invalid_argument("invalid dominant series contracts");
            used[index] = true;
        }
}

void validate_series(const backtest::proto::BacktestInput& input)
{
    std::vector<const proto::Contract*> contracts;
    for (const auto& contract : input.paper().contracts())
        contracts.push_back(&contract.dataset().contract());
    // No month belongs to two series.
    std::vector<bool> used(contracts.size());
    for (const auto& series : input.series()) {
        validate_dominant_schedule(series, contracts);
        for (const auto& roll : series.rolls()) {
            if (used[roll.contract()])
                throw std::invalid_argument("invalid dominant series schedule");
            used[roll.contract()] = true;
        }
    }
    // What the portfolio trades at a time: each contract outside a series, and
    // each series as one, however many months it brings.
    const auto months = static_cast<std::size_t>(std::ranges::count(used, true));
    if (contracts.size() - months + static_cast<std::size_t>(input.series_size())
        > max_portfolio_contracts)
        throw std::invalid_argument("a portfolio holds at most 20 contracts and dominant series");
}

void validate_settlement_margins(const backtest::proto::BacktestInput& input)
{
    if (input.settlement_margins().empty())
        return;
    if (input.paper().contracts_size() != 1 || !input.series().empty()
        || input.paper().contracts(0).dataset().interval_minutes() != 0
        || input.strategies_size() != 1 || !input.holdout_day().empty() || input.has_walk_forward())
        throw std::invalid_argument(
            "settlement margin requires a single-contract full-sample daily backtest");
    const auto& contract = input.paper().contracts(0);
    const auto costs = cost_schedule(contract.cost_schedule());
    const auto& days = contract.dataset().days();
    std::string previous;
    for (const auto& event : input.settlement_margins()) {
        (void)parse_trading_date(event.trading_day());
        const auto day = std::ranges::find_if(days, [&](const auto& candidate) {
            return candidate.trading_day() == event.trading_day();
        });
        if (event.trading_day() <= previous || day == days.end())
            throw std::invalid_argument(
                "settlement margin dates must be ascending unique input trading days");
        if (!event.has_rate() || event.rate().units() <= 0
            || event.rate().units() >= Decimal::parse("1").raw()
            || costs_on(costs, event.trading_day()).values.margin_per_lot != Decimal{})
            throw std::invalid_argument(
                "settlement margin requires a single-contract percentage model");
        if (event.source().empty() || event.source().size() > 256
            || event.source().find_first_not_of(" \t\r\n") == std::string::npos)
            throw std::invalid_argument(
                "settlement margin requires an explanation of at most 256 bytes");
        const auto next = std::next(day);
        if (next != days.end() && std::ranges::none_of(costs, [&](const auto& version) {
                return version.effective_from == next->trading_day();
            }))
            throw std::invalid_argument(
                "settlement margin requires explicit costs for the next trading day");
        previous = event.trading_day();
    }
}

void validate_historical_margin_input(const backtest::proto::BacktestInput& input)
{
    if (!input.has_historical_margins())
        return;
    const auto& fixed = input.historical_margins();
    (void)decode_historical_margins(fixed);
    if (input.paper().contracts_size() != 1)
        throw std::invalid_argument(
            "historical margin evidence does not match backtest parameters");
    const auto& contract = input.paper().contracts(0);
    if (history_identity(contract.dataset().contract()).key() != fixed.contract()
        || contract.dataset().days_size() != fixed.days_size()
        || input.settlement_margins_size() != fixed.days_size()
        || contract.cost_schedule().versions_size() != fixed.days_size())
        throw std::invalid_argument(
            "historical margin evidence does not match backtest parameters");
    for (int i = 0; i < fixed.days_size(); ++i) {
        const auto& day = fixed.days(i);
        const auto& cost = contract.cost_schedule().versions(i);
        const auto& event = input.settlement_margins(i);
        if (contract.dataset().days(i).trading_day() != day.trading_day()
            || cost.effective_from() != day.trading_day()
            || cost.values().margin_per_lot().units() != 0
            || cost.values().margin_rate().units() != day.day_start_rate().units()
            || event.trading_day() != day.trading_day()
            || event.rate().units() != day.settlement_rate().units()
            || event.source() != fixed.source())
            throw std::invalid_argument(
                "historical margin evidence does not match backtest parameters");
    }
}

} // namespace

void apply_historical_margins(
    backtest::proto::BacktestInput& input,
    const data::proto::HistoricalMargins& fixed)
{
    (void)decode_historical_margins(fixed);
    if (input.paper().contracts_size() != 1 || !input.settlement_margins().empty())
        throw std::invalid_argument(
            "historical margins require one contract and no manual settlement events");
    auto* contract = input.mutable_paper()->mutable_contracts(0);
    const auto original = contract->cost_schedule();
    (void)cost_schedule(original);
    contract->clear_cost_schedule();
    for (const auto& day : fixed.days()) {
        const proto::CostVersion* selected = nullptr;
        for (const auto& version : original.versions())
            if (version.effective_from() <= day.trading_day())
                selected = &version;
        if (!selected || selected->values().margin_per_lot().units() != 0)
            throw std::invalid_argument(
                "historical margins require a percentage cost schedule covering every day");
        auto* cost = contract->mutable_cost_schedule()->add_versions();
        *cost = *selected;
        cost->set_effective_from(day.trading_day());
        *cost->mutable_values()->mutable_margin_rate() = day.day_start_rate();
        auto* event = input.add_settlement_margins();
        event->set_trading_day(day.trading_day());
        *event->mutable_rate() = day.settlement_rate();
        event->set_source(fixed.source());
    }
    *input.mutable_historical_margins() = fixed;
    validate_historical_margin_input(input);
}

backtest::proto::BacktestRequest encode_backtest_request(const Json& input)
{
    auto required = input;
    required.erase("settlement_margins");
    required.erase("margin_snapshot");
    require_fields(
        required,
        {"contracts", "deposit", "risk", "strategies", "series", "holdout_from", "walk_forward"});
    if (!input.at("strategies").is_array() || input.at("strategies").empty()
        || input.at("strategies").size() > 32 || !input.at("holdout_from").is_string())
        throw std::invalid_argument("backtest requires 1 to 32 strategies");
    // A product of the daily table brings its own months: with one, the request
    // need list no contract.
    const auto product = [](const Json& series) {
        return series.is_object();
    };
    if (!input.at("series").is_array() || !input.at("contracts").is_array()
        || input.at("contracts").size() > max_account_contracts
        || (input.at("contracts").empty() && std::ranges::none_of(input.at("series"), product)))
        throw std::invalid_argument("backtest requires 1 to 256 contracts");
    const auto slippage = [](const Json& traded) {
        if (!traded.at("slippage_ticks").is_number_integer() || traded.at("slippage_ticks") < 0
            || traded.at("slippage_ticks") > max_slippage_ticks)
            throw std::invalid_argument("slippage is 0 to 100 price increments");
        return traded.at("slippage_ticks").get<unsigned>();
    };
    backtest::proto::BacktestRequest result;
    if (input.contains("margin_snapshot")) {
        const auto snapshot = input.at("margin_snapshot").get<std::string>();
        if (snapshot.size() != 64
            || snapshot.find_first_not_of("0123456789abcdef") != std::string::npos)
            throw std::invalid_argument("historical margin snapshot is unavailable");
        result.set_margin_snapshot(snapshot);
    }
    if (input.contains("settlement_margins")) {
        const auto& events = input.at("settlement_margins");
        if (!events.is_array())
            throw std::invalid_argument("settlement margin events must be a list");
        for (const auto& event : events) {
            require_fields(event, {"trading_day", "rate", "source"});
            auto* fixed = result.add_settlement_margins();
            fixed->set_trading_day(event.at("trading_day").get<std::string>());
            fixed->mutable_rate()->set_units(
                Decimal::parse(event.at("rate").get<std::string>()).raw());
            fixed->set_source(event.at("source").get<std::string>());
        }
    }
    for (const auto& contract : input.at("contracts")) {
        require_fields(contract, {"data", "cost_schedule", "slippage_ticks"});
        auto* item = result.add_contracts();
        item->set_slippage_ticks(slippage(contract));
        *item->mutable_data() = encode_bar_dataset_request(contract.at("data"));
        *item->mutable_cost_schedule() = encode_cost_schedule(contract.at("cost_schedule"));
    }
    for (const auto& members : input.at("series")) {
        auto* series = result.add_series();
        if (product(members)) {
            require_fields(members, {"product", "cost_schedule", "slippage_ticks"});
            auto* traded = series->mutable_product();
            traded->set_slippage_ticks(slippage(members));
            *traded->mutable_data() = encode_product_bars_request(members.at("product"));
            *traded->mutable_cost_schedule() = encode_cost_schedule(members.at("cost_schedule"));
            continue;
        }
        if (!members.is_array() || members.size() < 2)
            throw std::invalid_argument("a dominant series requires at least two month contracts");
        for (const auto& index : members) {
            if (!index.is_number_unsigned())
                throw std::invalid_argument("invalid dominant series contracts");
            series->add_contracts(index.get<unsigned>());
        }
    }
    distinct_contracts(
        result.series(),
        static_cast<std::size_t>(result.contracts_size()),
        [](const auto& series) {
            return series.contracts();
        });
    result.mutable_deposit()->set_units(
        Decimal::parse(input.at("deposit").get<std::string>()).raw());
    *result.mutable_risk() = encode_risk(input.at("risk"));
    for (const auto& strategy : input.at("strategies"))
        *result.add_strategies() = encode_strategy(strategy);
    result.set_holdout_from(input.at("holdout_from").get<std::string>());
    if (const auto& rolling = input.at("walk_forward"); !rolling.is_null()) {
        require_fields(rolling, {"training_days", "validation_days"});
        if (!rolling.at("training_days").is_number_unsigned()
            || !rolling.at("validation_days").is_number_unsigned())
            throw std::invalid_argument("a rolling comparison counts whole trading days");
        result.mutable_walk_forward()->set_training_days(
            rolling.at("training_days").get<unsigned>());
        result.mutable_walk_forward()->set_validation_days(
            rolling.at("validation_days").get<unsigned>());
    }
    return result;
}

std::vector<std::string> backtest_trading_days(const backtest::proto::BacktestInput& input)
{
    std::set<std::string> days;
    for (const auto& contract : input.paper().contracts())
        for (const auto& day : contract.dataset().days())
            days.insert(day.trading_day());
    return {days.begin(), days.end()};
}

FuturesCostVersion backtest_settlement_costs(
    const backtest::proto::BacktestInput& input,
    std::size_t contract,
    const std::string& day)
{
    auto cost = costs_on(
        cost_schedule(input.paper().contracts(static_cast<int>(contract)).cost_schedule()),
        day);
    for (const auto& event : input.settlement_margins())
        if (event.trading_day() == day) {
            cost.values.margin_rate = Decimal::from_raw(event.rate().units());
            cost.source += " / " + event.source();
            cost.effective_from = day;
            break;
        }
    return cost;
}

void set_backtest_strategies(
    backtest::proto::BacktestInput& input,
    const backtest::proto::BacktestRequest& request)
{
    *input.mutable_strategies() = request.strategies();
    const auto& from = request.holdout_from();
    if (input.strategies_size() < 2) {
        if (!from.empty() || request.has_walk_forward())
            throw std::invalid_argument("a holdout belongs to a comparison of several strategies");
        return;
    }
    if (request.has_walk_forward()) {
        if (!from.empty())
            throw std::invalid_argument("strategies are compared before a holdout or by rolling");
        *input.mutable_walk_forward() = request.walk_forward();
        return;
    }
    static_cast<void>(parse_trading_date(from));
    const auto days = backtest_trading_days(input);
    const auto first = std::ranges::lower_bound(days, from);
    if (first == days.end())
        throw std::invalid_argument("the holdout begins after the last trading day");
    input.set_holdout_day(*first);
}

std::vector<BacktestRound> backtest_folds(const backtest::proto::BacktestInput& input)
{
    const auto days = backtest_trading_days(input).size();
    const std::size_t training = input.walk_forward().training_days();
    const std::size_t validation = input.walk_forward().validation_days();
    std::vector<BacktestRound> result;
    for (auto first = training; first < days; first += validation)
        result.push_back({first, std::min(days, first + validation)});
    return result;
}

std::size_t backtest_work_units(const backtest::proto::BacktestInput& input)
{
    std::size_t bars = 0, development = 0;
    for (const auto& contract : input.paper().contracts())
        for (const auto& bar : contract.dataset().bars()) {
            ++bars;
            development += bar.trading_day() < input.holdout_day();
        }
    const auto compared = static_cast<std::size_t>(input.strategies_size());
    if (compared < 2)
        return bars;
    return bars + compared * (input.has_walk_forward() ? bars : development);
}

Json decode_backtest(const backtest::proto::BacktestInput& input, DatasetView view)
{
    validate_message(input);
    if (!input.has_paper() || input.strategies().empty() || input.strategies_size() > 32)
        throw std::invalid_argument("incomplete backtest input");
    if (input.dataset_revision() != dataset_revision(input.paper()))
        throw std::invalid_argument("dataset revision does not match input snapshot");
    // Evaluated before the braced initializer: GCC < 13 leaks already-built
    // initializer_list elements when a later element throws (PR66139).
    auto paper = decode_input(input.paper(), view);
    Json strategies = Json::array();
    std::set<std::string> distinct;
    for (const auto& strategy : input.strategies()) {
        for (const auto& contract : input.paper().contracts())
            validate_strategy(
                strategy,
                Decimal::from_raw(contract.dataset().contract().quantity_increment().units()));
        if (!distinct.insert(strategy.SerializeAsString()).second)
            throw std::invalid_argument("compared strategies must differ");
        strategies.push_back(decode_strategy(strategy));
    }
    const auto days = backtest_trading_days(input);
    const auto holdout = std::ranges::find(days, input.holdout_day());
    if (input.has_walk_forward()) {
        // Rounds score by a yearly ratio, which fewer than 20 days do not have.
        const auto& rolling = input.walk_forward();
        if (input.strategies_size() == 1 || !input.holdout_day().empty()
            || rolling.training_days() < backtest_development_days || !rolling.validation_days()
            || rolling.training_days() >= days.size() || backtest_folds(input).size() > 100)
            throw std::invalid_argument(
                "a rolling comparison of several strategies requires at least 20 training days, " "validation days after them and at most 100 rounds, and no holdout");
    } else if (input.strategies_size() == 1 ? !input.holdout_day().empty()
                                            : holdout == days.end()
                || static_cast<std::size_t>(holdout - days.begin()) < backtest_development_days)
        throw std::invalid_argument(
            "comparing strategies requires a holdout that begins on a trading day after at least 20 " "others; one strategy takes none");
    validate_series(input);
    validate_settlement_margins(input);
    validate_historical_margin_input(input);
    Json result{{"dataset_revision", input.dataset_revision()},
        {"paper", std::move(paper)},
        {"strategies", std::move(strategies)},
        {"holdout_day", input.holdout_day()},
        {"walk_forward",
            input.has_walk_forward()
                ? Json{{"training_days", input.walk_forward().training_days()},
                      {"validation_days", input.walk_forward().validation_days()}}
                : Json(nullptr)}};
    if (input.series_size()) {
        Json all = Json::array();
        for (const auto& series : input.series())
            all.push_back(decode_dominant_schedule(series));
        result["series"] = std::move(all);
    }
    if (input.has_historical_margins())
        result["historical_margins"] = decode_historical_margins(input.historical_margins());
    if (!input.settlement_margins().empty()) {
        auto events = Json::array();
        for (const auto& event : input.settlement_margins())
            events.push_back(
                {{"trading_day", event.trading_day()},
                    {"rate", Decimal::from_raw(event.rate().units()).str()},
                    {"source", event.source()}});
        result["settlement_margins"] = std::move(events);
    }
    return result;
}

Json decode_backtest_result(const backtest::proto::BacktestResult& result)
{
    validate_message(result);
    if (!result.has_account() || !result.has_max_drawdown() || result.equity().empty()
        || result.settlements().empty()
        || static_cast<std::size_t>(result.settlements_size()) > max_dataset_bars)
        throw std::invalid_argument("incomplete backtest result");
    if (result.fill_days_size() && result.fill_days_size() != result.account().fills_size())
        throw std::invalid_argument("invalid backtest fill-day evidence");
    Json fill_days = Json::array();
    std::string previous_day;
    for (int i = 0; i < result.fill_days_size(); ++i) {
        const auto& observed = result.fill_days(i);
        (void)parse_trading_date(observed.trading_day());
        if (observed.fill_id() != result.account().fills(i).id()
            || observed.trading_day() < previous_day)
            throw std::invalid_argument("invalid backtest fill-day evidence");
        previous_day = observed.trading_day();
        fill_days.push_back(
            {{"fill_id", observed.fill_id()}, {"trading_day", observed.trading_day()}});
    }
    Json curve = Json::array(), settlements = Json::array();
    for (const auto& point : result.equity()) {
        if (!point.has_equity() || point.timestamp_ns() < 0
            || (point.event() != backtest::proto::TRADE_MARK
                && point.event() != backtest::proto::DAILY_SETTLEMENT))
            throw std::invalid_argument("invalid backtest equity event");
        curve.push_back(
            {{"timestamp_ns", std::to_string(point.timestamp_ns())},
                {"equity", Decimal::from_raw(point.equity().units()).str()},
                {"event", point.event() == backtest::proto::TRADE_MARK ? "trade" : "settlement"}});
    }
    for (const auto& day : result.settlements()) {
        if (!day.has_balance() || !day.has_equity() || !day.has_realized() || !day.has_fees()
            || day.contracts().empty())
            throw std::invalid_argument("incomplete settlement result");
        Json contracts = Json::array();
        Decimal recorded_margin;
        for (const auto& c : day.contracts()) {
            if (!c.has_price() || !c.has_position_quantity())
                throw std::invalid_argument("incomplete settlement result");
            InstrumentId{c.venue(), c.symbol()}.validate();
            if (c.has_gross_quantity() != day.has_occupancy()
                || c.has_margin() != day.has_occupancy())
                throw std::invalid_argument("invalid settlement occupancy evidence");
            if (day.has_occupancy()) {
                const auto gross = Decimal::from_raw(c.gross_quantity().units());
                const auto net = Decimal::from_raw(c.position_quantity().units());
                const auto margin = Decimal::from_raw(c.margin().units());
                if (gross < (net < Decimal{} ? Decimal{} - net : net) || margin < Decimal{})
                    throw std::invalid_argument("invalid settlement occupancy evidence");
                recorded_margin = recorded_margin + margin;
            }
            contracts.push_back(
                {{"venue", c.venue()},
                    {"symbol", c.symbol()},
                    {"price", Decimal::from_raw(c.price().units()).str()},
                    {"position_quantity", Decimal::from_raw(c.position_quantity().units()).str()}});
            if (day.has_occupancy()) {
                contracts.back()["gross_quantity"] =
                    Decimal::from_raw(c.gross_quantity().units()).str();
                contracts.back()["margin"] = Decimal::from_raw(c.margin().units()).str();
            }
        }
        if (day.has_occupancy()) {
            const auto& funds = day.occupancy();
            if (!funds.has_margin() || !funds.has_available() || !funds.has_frozen()
                || Decimal::from_raw(funds.margin().units()) != recorded_margin
                || funds.frozen().units() < 0
                || Decimal::from_raw(funds.available().units())
                    != std::min(
                           Decimal::from_raw(day.balance().units()),
                           Decimal::from_raw(day.equity().units()))
                        - recorded_margin - Decimal::from_raw(funds.frozen().units()))
                throw std::invalid_argument("invalid settlement occupancy evidence");
        }
        settlements.push_back(
            {{"trading_day", day.trading_day()},
                {"timestamp_ns", std::to_string(day.timestamp_ns())},
                {"balance", Decimal::from_raw(day.balance().units()).str()},
                {"equity", Decimal::from_raw(day.equity().units()).str()},
                {"realized", Decimal::from_raw(day.realized().units()).str()},
                {"fees", Decimal::from_raw(day.fees().units()).str()},
                {"contracts", std::move(contracts)}});
        if (day.has_occupancy())
            settlements.back()["occupancy"] = {
                {"margin", Decimal::from_raw(day.occupancy().margin().units()).str()},
                {"available", Decimal::from_raw(day.occupancy().available().units()).str()},
                {"frozen", Decimal::from_raw(day.occupancy().frozen().units()).str()}
            };
        if (day.has_margin_revaluation()) {
            const auto& change = day.margin_revaluation();
            if (!day.has_occupancy() || !change.has_previous_rate() || !change.has_rate()
                || !change.has_margin_before() || !change.has_available_before()
                || change.previous_rate().units() <= 0 || change.rate().units() <= 0
                || change.previous_rate().units() >= Decimal::parse("1").raw()
                || change.rate().units() >= Decimal::parse("1").raw()
                || change.margin_before().units() < 0
                || Decimal::from_raw(change.available_before().units())
                    != std::min(
                           Decimal::from_raw(day.balance().units()),
                           Decimal::from_raw(day.equity().units()))
                        - Decimal::from_raw(change.margin_before().units())
                        - Decimal::from_raw(day.occupancy().frozen().units()))
                throw std::invalid_argument("invalid settlement margin revaluation evidence");
            settlements.back()["margin_revaluation"] = {
                {"previous_rate", Decimal::from_raw(change.previous_rate().units()).str()},
                {"rate", Decimal::from_raw(change.rate().units()).str()},
                {"margin_before", Decimal::from_raw(change.margin_before().units()).str()},
                {"available_before", Decimal::from_raw(change.available_before().units()).str()}
            };
        }
    }
    auto account = protocol::decode_snapshot(result.account());
    account["mode"] = "backtest";
    account["persistent"] = false;
    // The scores of what was compared, and the highest ratio among them: the
    // first to have it, or none when no score has a ratio.
    const auto scores = [](const auto& compared, Json& shown) {
        std::optional<unsigned> best;
        for (int i = 0; i < compared.size(); ++i) {
            const auto& candidate = compared[i];
            if (!std::isfinite(candidate.total_return()) || !std::isfinite(candidate.max_drawdown())
                || candidate.max_drawdown() < 0
                || (candidate.has_sharpe() && !std::isfinite(candidate.sharpe())))
                throw std::invalid_argument("invalid backtest comparison evidence");
            shown.push_back(
                {{"total_return", candidate.total_return()},
                    {"max_drawdown", candidate.max_drawdown()},
                    {"sharpe", candidate.has_sharpe() ? Json(candidate.sharpe()) : Json(nullptr)}});
            if (candidate.has_sharpe()
                && (!best || candidate.sharpe() > compared[static_cast<int>(*best)].sharpe()))
                best = static_cast<unsigned>(i);
        }
        return best;
    };
    Json candidates = Json::array();
    const auto best = scores(result.candidates(), candidates);
    // The selected strategy is the best of those compared before a holdout.
    if (candidates.empty() ? result.selected() != 0
                           : candidates.size() < 2 || best != result.selected())
        throw std::invalid_argument("invalid backtest comparison evidence");
    Json folds = Json::array();
    for (const auto& fold : result.folds()) {
        static_cast<void>(parse_trading_date(fold.first_day()));
        Json compared = Json::array();
        const auto chosen = scores(fold.candidates(), compared);
        // A round follows its best strategy, or none when none has a ratio.
        if (compared.size() < 2 || !candidates.empty()
            || chosen != (fold.has_selected() ? std::optional(fold.selected()) : std::nullopt))
            throw std::invalid_argument("invalid backtest comparison evidence");
        folds.push_back(
            {{"first_day", fold.first_day()},
                {"candidates", std::move(compared)},
                {"selected", chosen ? Json(*chosen) : Json(nullptr)}});
    }
    Json shown = {
        {"candidates", std::move(candidates)},
        {"selected", result.selected()},
        {"folds", std::move(folds)},
        {"dataset_revision", result.dataset_revision()},
        {"account", account},
        {"equity", curve},
        {"settlements", settlements},
        {"max_drawdown", Decimal::from_raw(result.max_drawdown().units()).str()}
    };
    if (!fill_days.empty())
        shown["fill_days"] = std::move(fill_days);
    return shown;
}

backtest::proto::BacktestInput
backtest_slippage_variant(const backtest::proto::BacktestInput& original, unsigned ticks)
{
    (void)decode_backtest(original);
    if (ticks > max_slippage_ticks)
        throw std::invalid_argument("slippage is 0 to 100 price increments");
    if (original.paper().contracts_size() != 1 || original.series_size() != 0
        || original.paper().contracts(0).dataset().interval_minutes() != 0
        || original.strategies_size() != 1 || !original.strategies(0).has_momentum()
        || !original.holdout_day().empty() || original.has_walk_forward())
        throw std::invalid_argument(
            "slippage sensitivity requires a single-contract full-sample daily momentum backtest");
    if (ticks == original.paper().contracts(0).slippage_ticks())
        throw std::invalid_argument("slippage variant must differ from its baseline");
    auto result = original;
    result.mutable_paper()->mutable_contracts(0)->set_slippage_ticks(ticks);
    return result;
}

} // namespace asterion::protocol
