#include <asterion/protocol/research_evaluation.hpp>
#include <asterion/protocol/backtest.hpp>
#include <asterion/protocol/trading.hpp>
#include <asterion/base/id.hpp>
#include <asterion/base/decimal.hpp>
#include <asterion/futures/trading_days.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <set>
#include <optional>

namespace asterion::protocol {
namespace {
namespace r = research::proto;

bool digest(const std::string& value)
{
    return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string::npos;
}

std::string scope_name(r::EvaluationRequest::Scope scope)
{
    switch (scope) {
        case r::EvaluationRequest::FULL_ACCOUNT:
            return "full_account";
        case r::EvaluationRequest::DEVELOPMENT:
            return "development";
        case r::EvaluationRequest::HOLDOUT:
            return "holdout";
        case r::EvaluationRequest::OUT_OF_SAMPLE:
            return "out_of_sample";
        default:
            throw std::invalid_argument("invalid evaluation scope");
    }
}

Json source_object(const resource::proto::Object& value)
{
    return {
        {"node", value.owner().node_id()},
        {"service", value.owner().service_id()},
        {"domain", value.owner().domain()},
        {"kind", value.kind()},
        {"id", value.id()},
        {"version", value.version()}
    };
}

Json stress_scenario_json(const r::EvaluationRequest& value)
{
    if (!value.has_static_stress())
        return nullptr;
    const auto& scenario = value.static_stress();
    return {
        {"purpose", scenario.purpose()},
        {"price", Decimal::from_raw(scenario.price().units()).str()},
        {"margin_rate", Decimal::from_raw(scenario.margin_rate().units()).str()},
        {"funds_model", "paper_balance_equity_minimum"}
    };
}

Json stress_result_json(const r::EvaluationResult& value)
{
    if (!value.has_static_stress())
        return nullptr;
    const auto& stress = value.static_stress();
    return {
        {"mark_change", Decimal::from_raw(stress.mark_change().units()).str()},
        {"equity", Decimal::from_raw(stress.equity().units()).str()},
        {"margin", Decimal::from_raw(stress.margin().units()).str()},
        {"available", Decimal::from_raw(stress.available().units()).str()},
        {"gap", Decimal::from_raw(stress.gap().units()).str()}
    };
}

Json ledger_json(const r::EvaluationLedger& value)
{
    const auto money = [](const auto& amount) {
        return Decimal::from_raw(amount.units()).str();
    };
    const auto checks = [&](const auto& rows) {
        Json out = Json::array();
        for (const auto& c : rows)
            out.push_back(
                {{"metric", c.metric()},
                    {"expected", money(c.expected())},
                    {"recorded", money(c.recorded())},
                    {"residual", money(c.residual())}});
        return out;
    };
    Json days = Json::array();
    for (const auto& row : value.days()) {
        Json fills = Json::array();
        for (const auto& f : row.fills())
            fills.push_back(
                {{"id", f.id()},
                    {"order_id", f.order_id()},
                    {"buy", f.buy()},
                    {"quantity", money(f.quantity())},
                    {"price", money(f.price())},
                    {"fee", money(f.fee())}});
        days.push_back(
            {{"source_index", row.source_index()},
                {"trading_day", row.trading_day()},
                {"timestamp_ns", std::to_string(row.timestamp_ns())},
                {"opening_quantity", money(row.opening_quantity())},
                {"quantity_change", money(row.quantity_change())},
                {"closing_quantity", money(row.closing_quantity())},
                {"opening_balance", money(row.opening_balance())},
                {"gross", money(row.gross())},
                {"fees", money(row.fees())},
                {"expected_balance", money(row.expected_balance())},
                {"recorded_balance", money(row.recorded_balance())},
                {"recorded_equity", money(row.recorded_equity())},
                {"cumulative_residual", money(row.cumulative_residual())},
                {"fills", std::move(fills)},
                {"checks", checks(row.checks())}});
    }
    return {
        {"venue", value.venue()},
        {"symbol", value.symbol()},
        {"multiplier", money(value.multiplier())},
        {"deposit", money(value.deposit())},
        {"gross", money(value.gross())},
        {"fees", money(value.fees())},
        {"explained_net", money(value.explained_net())},
        {"recorded_net", money(value.recorded_net())},
        {"residual", money(value.residual())},
        {"closing_net_quantity", money(value.closing_net_quantity())},
        {"closing_gross_quantity", money(value.closing_gross_quantity())},
        {"days", std::move(days)},
        {"final_checks", checks(value.final_checks())},
        {"reconciled", value.reconciled()}
    };
}

Json contributions_json(const r::EvaluationContributions& value)
{
    const auto money = [](const auto& v) {
        return Decimal::from_raw(v.units()).str();
    };
    const auto one_check = [&](const auto& c) -> Json {
        return {
            {"metric", c.metric()},
            {"expected", money(c.expected())},
            {"recorded", money(c.recorded())},
            {"residual", money(c.residual())}
        };
    };
    const auto checks = [&](const auto& rows) {
        Json out = Json::array();
        for (const auto& c : rows)
            out.push_back(one_check(c));
        return out;
    };
    Json contracts = Json::array(), days = Json::array();
    for (const auto& c : value.contracts()) {
        Json rows = Json::array();
        for (const auto& r : c.days()) {
            Json fills = Json::array();
            for (const auto& f : r.fills())
                fills.push_back(
                    {{"id", f.id()},
                        {"order_id", f.order_id()},
                        {"buy", f.buy()},
                        {"quantity", money(f.quantity())},
                        {"price", money(f.price())},
                        {"fee", money(f.fee())}});
            rows.push_back(
                {{"source_index", r.source_index()},
                    {"trading_day", r.trading_day()},
                    {"timestamp_ns", std::to_string(r.timestamp_ns())},
                    {"settlement", money(r.settlement())},
                    {"opening_quantity", money(r.opening_quantity())},
                    {"quantity_change", money(r.quantity_change())},
                    {"closing_quantity", money(r.closing_quantity())},
                    {"gross", money(r.gross())},
                    {"fees", money(r.fees())},
                    {"fills", std::move(fills)},
                    {"checks", checks(r.checks())}});
        }
        contracts.push_back(
            {{"venue", c.venue()},
                {"symbol", c.symbol()},
                {"multiplier", money(c.multiplier())},
                {"gross", money(c.gross())},
                {"fees", money(c.fees())},
                {"net", money(c.net())},
                {"days", std::move(rows)},
                {"final_position", one_check(c.final_position())}});
    }
    for (const auto& d : value.days())
        days.push_back(
            {{"source_index", d.source_index()},
                {"trading_day", d.trading_day()},
                {"timestamp_ns", std::to_string(d.timestamp_ns())},
                {"gross", money(d.gross())},
                {"fees", money(d.fees())},
                {"expected_balance", money(d.expected_balance())},
                {"recorded_balance", money(d.recorded_balance())},
                {"recorded_equity", money(d.recorded_equity())},
                {"cumulative_residual", money(d.cumulative_residual())},
                {"checks", checks(d.checks())}});
    return {
        {"deposit", money(value.deposit())},
        {"gross", money(value.gross())},
        {"fees", money(value.fees())},
        {"explained_net", money(value.explained_net())},
        {"recorded_net", money(value.recorded_net())},
        {"residual", money(value.residual())},
        {"contracts", std::move(contracts)},
        {"days", std::move(days)},
        {"final_checks", checks(value.final_checks())},
        {"reconciled", value.reconciled()}
    };
}

Json request_json(const r::EvaluationRequest& value)
{
    Json result{
        {"source", source_object(value.source())},
        {"question", value.question()},
        {"scope", scope_name(value.scope())},
        {"method", decode_performance_assumptions(value.method())},
        {"include_daily_drawdown", value.include_daily_drawdown()},
        {"include_exposure", value.include_exposure()},
        {"static_stress", stress_scenario_json(value)}
    };
    if (value.include_ledger())
        result["include_ledger"] = true;
    if (value.include_contributions())
        result["include_contributions"] = true;
    return result;
}

[[noreturn]] void invalid_result()
{
    throw std::invalid_argument("evaluation result does not match its fixed input");
}

void ledger_shape(const r::EvaluationInput& input, const r::EvaluationResult& value)
{
    if (input.request().include_ledger() != value.has_ledger())
        invalid_result();
    if (!value.has_ledger())
        return;
    const auto& ledger = value.ledger();
    const auto dec = [](const auto& amount) {
        return Decimal::from_raw(amount.units());
    };
    if (input.request().scope() != r::EvaluationRequest::FULL_ACCOUNT || ledger.venue().empty()
        || ledger.symbol().empty() || !ledger.has_multiplier() || !ledger.has_deposit()
        || !ledger.has_gross() || !ledger.has_fees() || !ledger.has_explained_net()
        || !ledger.has_recorded_net() || !ledger.has_residual()
        || !ledger.has_closing_net_quantity() || !ledger.has_closing_gross_quantity()
        || ledger.days_size() != static_cast<int>(value.trading_days()) || ledger.days().empty()
        || dec(ledger.multiplier()) <= Decimal{} || dec(ledger.closing_gross_quantity()) < Decimal{}
        || dec(ledger.closing_net_quantity()) > dec(ledger.closing_gross_quantity())
        || dec(ledger.closing_net_quantity()) < Decimal{} - dec(ledger.closing_gross_quantity())
        || ledger.deposit().units() != value.opening_equity().units()
        || ledger.fees().units() != value.fees().units()
        || ledger.recorded_net().units() != value.net_profit().units()
        || dec(ledger.explained_net()) != dec(ledger.gross()) - dec(ledger.fees())
        || dec(ledger.residual()) != dec(ledger.recorded_net()) - dec(ledger.explained_net()))
        invalid_result();
    bool matched = ledger.residual().units() == 0;
    const auto checks = [&](const auto& rows, const auto& metrics) {
        if (rows.size() != static_cast<int>(metrics.size()))
            invalid_result();
        int index = 0;
        for (const auto& check : rows) {
            if (check.metric() != metrics[index++])
                invalid_result();
            if (check.metric().empty() || !check.has_expected() || !check.has_recorded()
                || !check.has_residual()
                || dec(check.residual()) != dec(check.recorded()) - dec(check.expected()))
                invalid_result();
            matched = matched && check.residual().units() == 0;
        }
    };
    Decimal gross, fees, balance = dec(ledger.deposit()), quantity;
    for (int i = 0; i < ledger.days_size(); ++i) {
        const auto& row = ledger.days(i);
        (void)parse_trading_date(row.trading_day());
        if (row.source_index() != static_cast<unsigned>(i)
            || (i && row.trading_day() <= ledger.days(i - 1).trading_day())
            || row.timestamp_ns() < 0 || !row.has_opening_quantity() || !row.has_quantity_change()
            || !row.has_closing_quantity() || !row.has_opening_balance() || !row.has_gross()
            || !row.has_fees() || !row.has_expected_balance() || !row.has_recorded_balance()
            || !row.has_recorded_equity() || !row.has_cumulative_residual()
            || row.checks_size() != 6 || dec(row.opening_quantity()) != quantity
            || dec(row.opening_balance()) != balance
            || dec(row.closing_quantity())
                != dec(row.opening_quantity()) + dec(row.quantity_change())
            || dec(row.expected_balance()) != balance + dec(row.gross()) - dec(row.fees())
            || dec(row.cumulative_residual())
                != dec(row.recorded_balance()) - dec(row.expected_balance()))
            invalid_result();
        Decimal fill_fees, change;
        for (const auto& fill : row.fills()) {
            if (fill.id().empty() || fill.order_id().empty() || !fill.has_quantity()
                || !fill.has_price() || !fill.has_fee() || dec(fill.quantity()) <= Decimal{}
                || dec(fill.fee()) < Decimal{})
                invalid_result();
            fill_fees = fill_fees + dec(fill.fee());
            change =
                change + (fill.buy() ? dec(fill.quantity()) : Decimal{} - dec(fill.quantity()));
        }
        if (fill_fees != dec(row.fees()) || change != dec(row.quantity_change()))
            invalid_result();
        checks(
            row.checks(),
            std::array{"settlement", "position", "fees", "balance", "equity", "realized"});
        matched = matched && row.cumulative_residual().units() == 0;
        gross = gross + dec(row.gross());
        fees = fees + dec(row.fees());
        balance = dec(row.expected_balance());
        quantity = dec(row.closing_quantity());
    }
    if (ledger.final_checks_size() != 9 || gross != dec(ledger.gross())
        || fees != dec(ledger.fees()) || ledger.days(0).trading_day() != value.first_day()
        || ledger.days(ledger.days_size() - 1).trading_day() != value.last_day())
        invalid_result();
    checks(
        ledger.final_checks(),
        std::array{
            "position",
            "fees",
            "balance",
            "equity",
            "realized",
            "unrealized",
            "frozen",
            "margin",
            "available"
        });
    if (matched != ledger.reconciled())
        invalid_result();
}

void contributions_shape(const r::EvaluationInput& input, const r::EvaluationResult& value)
{
    if (input.request().include_contributions() != value.has_contributions())
        invalid_result();
    if (!value.has_contributions())
        return;
    const auto& a = value.contributions();
    const auto dec = [](const auto& v) {
        return Decimal::from_raw(v.units());
    };
    if (input.request().scope() != r::EvaluationRequest::FULL_ACCOUNT || a.contracts().empty()
        || a.days_size() != static_cast<int>(value.trading_days()) || a.days().empty()
        || !a.has_deposit() || !a.has_gross() || !a.has_fees() || !a.has_explained_net()
        || !a.has_recorded_net() || !a.has_residual()
        || a.deposit().units() != value.opening_equity().units()
        || a.fees().units() != value.fees().units()
        || a.recorded_net().units() != value.net_profit().units()
        || dec(a.explained_net()) != dec(a.gross()) - dec(a.fees())
        || dec(a.residual()) != dec(a.recorded_net()) - dec(a.explained_net()))
        invalid_result();
    bool matched = a.residual().units() == 0;
    const auto one_check = [&](const auto& c, const char* name) {
        if (c.metric() != name || !c.has_expected() || !c.has_recorded() || !c.has_residual()
            || dec(c.residual()) != dec(c.recorded()) - dec(c.expected()))
            invalid_result();
        matched = matched && c.residual().units() == 0;
    };
    const auto checks = [&](const auto& rows, const auto& names) {
        if (rows.size() != static_cast<int>(names.size()))
            invalid_result();
        int i = 0;
        for (const auto& c : rows)
            one_check(c, names[i++]);
    };
    std::vector<Decimal> day_gross(a.days_size()), day_fees(a.days_size());
    std::set<std::pair<std::string, std::string>> identities;
    std::set<std::string> fill_ids;
    Decimal gross, fees;
    for (const auto& c : a.contracts()) {
        if (c.venue().empty() || c.symbol().empty()
            || !identities.emplace(c.venue(), c.symbol()).second || !c.has_multiplier()
            || dec(c.multiplier()) <= Decimal{} || !c.has_gross() || !c.has_fees() || !c.has_net()
            || !c.has_final_position() || dec(c.net()) != dec(c.gross()) - dec(c.fees()))
            invalid_result();
        Decimal cg, cf, quantity;
        std::optional<unsigned> previous;
        for (const auto& row : c.days()) {
            const auto index = row.source_index();
            if (index >= static_cast<unsigned>(a.days_size()) || (previous && index <= *previous)
                || row.trading_day() != a.days(static_cast<int>(index)).trading_day()
                || row.timestamp_ns() != a.days(static_cast<int>(index)).timestamp_ns()
                || !row.has_settlement() || !row.has_opening_quantity()
                || !row.has_quantity_change() || !row.has_closing_quantity() || !row.has_gross()
                || !row.has_fees() || dec(row.opening_quantity()) != quantity
                || dec(row.closing_quantity()) != quantity + dec(row.quantity_change()))
                invalid_result();
            previous = index;
            Decimal ff, change;
            for (const auto& f : row.fills()) {
                if (f.id().empty() || f.order_id().empty() || !fill_ids.insert(f.id()).second
                    || !f.has_quantity() || dec(f.quantity()) <= Decimal{} || !f.has_price()
                    || !f.has_fee() || dec(f.fee()) < Decimal{})
                    invalid_result();
                ff = ff + dec(f.fee());
                change = change + (f.buy() ? dec(f.quantity()) : Decimal{} - dec(f.quantity()));
            }
            if (ff != dec(row.fees()) || change != dec(row.quantity_change()))
                invalid_result();
            checks(row.checks(), std::array{"settlement", "position"});
            if (dec(row.checks(0).recorded()) != dec(row.settlement())
                || dec(row.checks(1).expected()) != dec(row.closing_quantity()))
                invalid_result();
            quantity = dec(row.closing_quantity());
            cg = cg + dec(row.gross());
            cf = cf + dec(row.fees());
            day_gross[index] = day_gross[index] + dec(row.gross());
            day_fees[index] = day_fees[index] + dec(row.fees());
        }
        one_check(c.final_position(), "position");
        if (dec(c.final_position().expected()) != quantity || cg != dec(c.gross())
            || cf != dec(c.fees()))
            invalid_result();
        gross = gross + cg;
        fees = fees + cf;
    }
    Decimal balance = dec(a.deposit()), cumulative_gross, cumulative_fees;
    for (int i = 0; i < a.days_size(); ++i) {
        const auto& d = a.days(i);
        (void)parse_trading_date(d.trading_day());
        if (d.source_index() != static_cast<unsigned>(i) || d.timestamp_ns() < 0
            || (i && d.trading_day() <= a.days(i - 1).trading_day()) || !d.has_gross()
            || !d.has_fees() || !d.has_expected_balance() || !d.has_recorded_balance()
            || !d.has_recorded_equity() || !d.has_cumulative_residual()
            || dec(d.gross()) != day_gross[i] || dec(d.fees()) != day_fees[i]
            || dec(d.expected_balance()) != balance + dec(d.gross()) - dec(d.fees())
            || dec(d.cumulative_residual())
                != dec(d.recorded_balance()) - dec(d.expected_balance()))
            invalid_result();
        balance = dec(d.expected_balance());
        cumulative_gross = cumulative_gross + dec(d.gross());
        cumulative_fees = cumulative_fees + dec(d.fees());
        checks(d.checks(), std::array{"fees", "balance", "equity", "realized"});
        if (dec(d.checks(0).expected()) != cumulative_fees || dec(d.checks(1).expected()) != balance
            || dec(d.checks(1).recorded()) != dec(d.recorded_balance())
            || dec(d.checks(2).expected()) != balance
            || dec(d.checks(2).recorded()) != dec(d.recorded_equity())
            || dec(d.checks(3).expected()) != cumulative_gross)
            invalid_result();
        matched = matched && d.cumulative_residual().units() == 0;
    }
    checks(a.final_checks(), std::array{"fees", "balance", "equity", "realized"});
    if (gross != dec(a.gross()) || fees != dec(a.fees())
        || a.days(0).trading_day() != value.first_day()
        || a.days(a.days_size() - 1).trading_day() != value.last_day() || matched != a.reconciled()
        || dec(a.final_checks(0).expected()) != fees || dec(a.final_checks(1).expected()) != balance
        || dec(a.final_checks(2).expected()) != balance
        || dec(a.final_checks(3).expected()) != gross
        || a.final_checks(2).recorded().units() != value.closing_equity().units())
        invalid_result();
}

void metric_shape(const r::EvaluationMetric& value)
{
    if (value.state() < r::EvaluationMetric::AVAILABLE
        || value.state() > r::EvaluationMetric::SHORTER_THAN_ONE_YEAR
        || value.has_value() != (value.state() == r::EvaluationMetric::AVAILABLE)
        || (value.has_value() && !std::isfinite(value.value())))
        invalid_result();
}

Json metric(const r::EvaluationMetric& value)
{
    return {
        {"state", r::EvaluationMetric::State_Name(value.state())},
        {"value", value.has_value() ? Json(value.value()) : Json(nullptr)}
    };
}

void validate_exposure(const r::EvaluationInput& input, const r::EvaluationResult& value)
{
    if (input.request().include_exposure() != value.has_exposure())
        invalid_result();
    if (!value.has_exposure())
        return;
    const auto& slice = value.exposure();
    if (slice.source_index() != input.source().end_settlement() - 1
        || slice.trading_day() != value.last_day() || slice.timestamp_ns() != value.end_ns()
        || !slice.has_balance() || slice.positions().empty() || !slice.has_covered_net_notional())
        invalid_result();
    if (slice.has_funds()
        && (!slice.funds().has_margin() || !slice.funds().has_available()
            || !slice.funds().has_frozen() || slice.funds().margin().units() < 0
            || slice.funds().frozen().units() < 0))
        invalid_result();
    const bool positive = value.closing_equity().units() > 0;
    const auto expected = [&](bool known, r::EvaluationMetric::State missing) {
        return !known  ? missing
            : positive ? r::EvaluationMetric::AVAILABLE
                       : r::EvaluationMetric::NON_POSITIVE_EQUITY_PATH;
    };
    const auto check = [&](const r::EvaluationMetric& metric,
                           r::EvaluationMetric::State state,
                           bool nonnegative = false) {
        metric_shape(metric);
        if (metric.state() != state || (nonnegative && metric.has_value() && metric.value() < 0))
            invalid_result();
    };
    bool all_gross = true;
    for (int i = 0; i < slice.positions_size(); ++i) {
        const auto& row = slice.positions(i);
        validate_id(row.venue());
        validate_id(row.symbol());
        if (row.source_contract_index() != static_cast<unsigned>(i) || row.product().empty()
            || row.delivery_month().empty() || !row.has_price() || !row.has_multiplier()
            || row.multiplier().units() <= 0 || !row.has_quantity_increment()
            || row.quantity_increment().units() <= 0 || !row.has_net_quantity()
            || !row.has_net_notional() || row.has_gross_quantity() != row.has_gross_notional()
            || row.has_gross_quantity() != row.has_long_quantity()
            || row.has_gross_quantity() != row.has_short_quantity()
            || (row.has_margin() && row.margin().units() < 0))
            invalid_result();
        if (row.has_gross_quantity()) {
            if (row.gross_quantity().units() < 0 || row.long_quantity().units() < 0
                || row.short_quantity().units() < 0 || row.gross_notional().units() < 0
                || (Decimal::from_raw(row.long_quantity().units())
                       + Decimal::from_raw(row.short_quantity().units()))
                        .raw()
                    != row.gross_quantity().units()
                || (Decimal::from_raw(row.long_quantity().units())
                       - Decimal::from_raw(row.short_quantity().units()))
                        .raw()
                    != row.net_quantity().units())
                invalid_result();
        } else
            all_gross = false;
        check(row.net_to_equity(), expected(true, r::EvaluationMetric::NON_POSITIVE_EQUITY_PATH));
        check(
            row.gross_to_equity(),
            expected(row.has_gross_notional(), r::EvaluationMetric::GROSS_NOT_RECORDED),
            true);
        check(
            row.margin_to_equity(),
            expected(row.has_margin(), r::EvaluationMetric::MARGIN_NOT_RECORDED),
            true);
    }
    if (slice.has_covered_gross_notional() != all_gross
        || (all_gross && slice.covered_gross_notional().units() < 0))
        invalid_result();
    check(slice.net_to_equity(), expected(true, r::EvaluationMetric::NON_POSITIVE_EQUITY_PATH));
    check(
        slice.gross_to_equity(),
        expected(all_gross, r::EvaluationMetric::GROSS_NOT_RECORDED),
        true);
    check(
        slice.margin_to_equity(),
        expected(slice.has_funds(), r::EvaluationMetric::MARGIN_NOT_RECORDED),
        true);
    const auto concentration = !all_gross             ? r::EvaluationMetric::GROSS_NOT_RECORDED
        : slice.covered_gross_notional().units() == 0 ? r::EvaluationMetric::ZERO_GROSS_NOTIONAL
                                                      : r::EvaluationMetric::AVAILABLE;
    for (const auto& row : slice.positions()) {
        check(row.covered_gross_share(), concentration, true);
        if (row.covered_gross_share().has_value() && row.covered_gross_share().value() > 1)
            invalid_result();
    }
}

Json exposure_json(const r::EvaluationResult& value)
{
    if (!value.has_exposure())
        return nullptr;
    const auto& slice = value.exposure();
    const auto decimal = [](const auto& value) {
        return Decimal::from_raw(value.units()).str();
    };
    Json positions = Json::array();
    for (const auto& row : slice.positions())
        positions.push_back(
            {{"source_contract_index", row.source_contract_index()},
                {"venue", row.venue()},
                {"symbol", row.symbol()},
                {"product", row.product()},
                {"delivery_month", row.delivery_month()},
                {"price", decimal(row.price())},
                {"multiplier", decimal(row.multiplier())},
                {"quantity_increment", decimal(row.quantity_increment())},
                {"net_quantity", decimal(row.net_quantity())},
                {"gross_quantity",
                    row.has_gross_quantity() ? Json(decimal(row.gross_quantity())) : Json(nullptr)},
                {"margin", row.has_margin() ? Json(decimal(row.margin())) : Json(nullptr)},
                {"long_quantity",
                    row.has_long_quantity() ? Json(decimal(row.long_quantity())) : Json(nullptr)},
                {"short_quantity",
                    row.has_short_quantity() ? Json(decimal(row.short_quantity())) : Json(nullptr)},
                {"net_notional", decimal(row.net_notional())},
                {"gross_notional",
                    row.has_gross_notional() ? Json(decimal(row.gross_notional())) : Json(nullptr)},
                {"net_to_equity", metric(row.net_to_equity())},
                {"gross_to_equity", metric(row.gross_to_equity())},
                {"margin_to_equity", metric(row.margin_to_equity())},
                {"covered_gross_share", metric(row.covered_gross_share())}});
    Json funds = nullptr;
    if (slice.has_funds())
        funds = {
            {"margin", decimal(slice.funds().margin())},
            {"available", decimal(slice.funds().available())},
            {"frozen", decimal(slice.funds().frozen())}
        };
    return {
        {"source_index", slice.source_index()},
        {"trading_day", slice.trading_day()},
        {"timestamp_ns",
            slice.timestamp_ns() ? Json(std::to_string(slice.timestamp_ns())) : Json(nullptr)},
        {"balance", decimal(slice.balance())},
        {"funds", std::move(funds)},
        {"positions", std::move(positions)},
        {"covered_net_notional", decimal(slice.covered_net_notional())},
        {"covered_gross_notional",
            slice.has_covered_gross_notional() ? Json(decimal(slice.covered_gross_notional()))
                                               : Json(nullptr)},
        {"net_to_equity", metric(slice.net_to_equity())},
        {"gross_to_equity", metric(slice.gross_to_equity())},
        {"margin_to_equity", metric(slice.margin_to_equity())}
    };
}

void validate_loss_path(const r::EvaluationInput& input, const r::EvaluationResult& value)
{
    if (input.request().include_daily_drawdown() != value.has_daily_drawdown())
        invalid_result();
    if (!value.has_daily_drawdown())
        return;
    const auto first = input.source().first_settlement();
    const auto end = input.source().end_settlement();
    const auto within = [&](unsigned index) {
        return index >= first && index < end;
    };
    const auto& path = value.daily_drawdown();
    if (path.points_size() != static_cast<int>(end - first) || !path.has_max_loss()
        || path.max_loss().units() < 0 || !within(path.max_loss_source_index()))
        invalid_result();
    bool nonpositive = false;
    std::string previous;
    for (int i = 0; i < path.points_size(); ++i) {
        const auto& row = path.points(i);
        (void)parse_trading_date(row.trading_day());
        if (row.source_index() != first + static_cast<unsigned>(i)
            || !within(row.peak_source_index()) || row.peak_source_index() > row.source_index()
            || (!previous.empty() && row.trading_day() <= previous) || row.timestamp_ns() < 0
            || !row.has_equity() || !row.has_peak_equity() || !row.has_loss()
            || row.loss().units() < 0
            || row.loss().units()
                != (Decimal::from_raw(row.peak_equity().units())
                    - Decimal::from_raw(row.equity().units()))
                    .raw())
            invalid_result();
        previous = row.trading_day();
        metric_shape(row.fraction());
        const bool positive = row.peak_equity().units() > 0 && row.equity().units() > 0;
        if ((positive
                && (row.fraction().state() != r::EvaluationMetric::AVAILABLE
                    || row.fraction().value() < 0))
            || (!positive
                && row.fraction().state() != r::EvaluationMetric::NON_POSITIVE_EQUITY_PATH))
            invalid_result();
        nonpositive |= !positive;
    }
    if (path.points(0).trading_day() != value.first_day()
        || path.points(path.points_size() - 1).trading_day() != value.last_day())
        invalid_result();
    metric_shape(path.max_fraction());
    const auto state = nonpositive ? r::EvaluationMetric::NON_POSITIVE_EQUITY_PATH
        : path.points_size() < 2   ? r::EvaluationMetric::FEWER_THAN_TWO_OBSERVATIONS
                                   : r::EvaluationMetric::AVAILABLE;
    if (path.max_fraction().state() != state
        || path.has_max_fraction_source_index() != path.max_fraction().has_value()
        || (path.has_max_fraction_source_index()
            && (!within(path.max_fraction_source_index()) || path.max_fraction().value() < 0)))
        invalid_result();
    unsigned previous_end = first;
    for (int i = 0; i < path.episodes_size(); ++i) {
        const auto& episode = path.episodes(i);
        if (!within(episode.peak_source_index()) || !within(episode.trough_source_index())
            || !within(episode.observed_end_source_index())
            || episode.peak_source_index() < previous_end
            || episode.peak_source_index() >= episode.trough_source_index()
            || episode.trough_source_index() > episode.observed_end_source_index()
            || episode.peak_to_trough_intervals()
                != episode.trough_source_index() - episode.peak_source_index()
            || episode.observed_intervals()
                != episode.observed_end_source_index() - episode.peak_source_index()
            || episode.has_recovery_source_index() != episode.has_peak_to_recovery_intervals())
            invalid_result();
        if (episode.has_recovery_source_index()) {
            if (episode.recovery_source_index() != episode.observed_end_source_index()
                || episode.recovery_source_index() <= episode.trough_source_index()
                || episode.peak_to_recovery_intervals()
                    != episode.recovery_source_index() - episode.peak_source_index())
                invalid_result();
        } else if (i != path.episodes_size() - 1 || episode.observed_end_source_index() != end - 1)
            invalid_result();
        previous_end = episode.observed_end_source_index();
    }
}

Json loss_path_json(const r::EvaluationResult& value)
{
    if (!value.has_daily_drawdown())
        return nullptr;
    const auto& path = value.daily_drawdown();
    Json points = Json::array(), episodes = Json::array();
    for (const auto& row : path.points())
        points.push_back(
            {{"source_index", row.source_index()},
                {"trading_day", row.trading_day()},
                {"timestamp_ns",
                    row.timestamp_ns() ? Json(std::to_string(row.timestamp_ns())) : Json(nullptr)},
                {"equity", Decimal::from_raw(row.equity().units()).str()},
                {"peak_source_index", row.peak_source_index()},
                {"peak_equity", Decimal::from_raw(row.peak_equity().units()).str()},
                {"loss", Decimal::from_raw(row.loss().units()).str()},
                {"fraction", metric(row.fraction())}});
    for (const auto& episode : path.episodes())
        episodes.push_back(
            {{"peak_source_index", episode.peak_source_index()},
                {"trough_source_index", episode.trough_source_index()},
                {"recovery_source_index",
                    episode.has_recovery_source_index() ? Json(episode.recovery_source_index())
                                                        : Json(nullptr)},
                {"observed_end_source_index", episode.observed_end_source_index()},
                {"peak_to_trough_intervals", episode.peak_to_trough_intervals()},
                {"peak_to_recovery_intervals",
                    episode.has_peak_to_recovery_intervals()
                        ? Json(episode.peak_to_recovery_intervals())
                        : Json(nullptr)},
                {"observed_intervals", episode.observed_intervals()}});
    return {
        {"points", std::move(points)},
        {"episodes", std::move(episodes)},
        {"max_loss", Decimal::from_raw(path.max_loss().units()).str()},
        {"max_loss_source_index", path.max_loss_source_index()},
        {"max_fraction", metric(path.max_fraction())},
        {"max_fraction_source_index",
            path.has_max_fraction_source_index() ? Json(path.max_fraction_source_index())
                                                 : Json(nullptr)}
    };
}

} // namespace

void validate_evaluation_request(const r::EvaluationRequest& value)
{
    validate_message(value);
    const auto& source = value.source();
    validate_id(source.owner().node_id());
    validate_id(source.owner().service_id());
    validate_id(source.id());
    if (source.owner().domain() != "research" || source.kind() != "backtest_result"
        || !digest(source.version()) || source.id() == "." || source.id() == ".."
        || source.id().find_first_of("/\\") != std::string::npos || value.question().empty()
        || value.question().size() > 4096)
        throw std::invalid_argument("invalid fixed evaluation request");
    (void)scope_name(value.scope());
    validate_performance_assumptions(value.method());
    if (value.has_static_stress()) {
        const auto& scenario = value.static_stress();
        if (!value.include_exposure() || scenario.purpose().empty()
            || scenario.purpose().size() > 4096 || !scenario.has_price()
            || !scenario.has_margin_rate() || scenario.margin_rate().units() < 0
            || scenario.funds_model() != r::EvaluationStressScenario::PAPER_BALANCE_EQUITY_MINIMUM)
            throw std::invalid_argument(
                "static stress requires an explicit scenario and original exposure");
    }
}

void validate_evaluation_input(const r::EvaluationInput& value)
{
    validate_message(value);
    validate_evaluation_request(value.request());
    const auto& source = value.source();
    if (!digest(source.input_sha256()) || !digest(source.program_artifact())
        || !source.input_bytes() || !source.attempt() || !source.settlements()
        || source.first_settlement() >= source.end_settlement()
        || source.end_settlement() > source.settlements())
        throw std::invalid_argument("invalid captured evaluation source");
}

std::pair<unsigned, unsigned> evaluation_range(
    const r::EvaluationRequest& request,
    const backtest::proto::BacktestInput& input,
    const backtest::proto::BacktestResult& result)
{
    validate_evaluation_request(request);
    const auto end = static_cast<unsigned>(result.settlements_size());
    if (!end)
        throw std::invalid_argument("evaluation source has no settlements");
    if (request.scope() == r::EvaluationRequest::FULL_ACCOUNT)
        return {0, end};
    if (request.scope() == r::EvaluationRequest::OUT_OF_SAMPLE && input.has_walk_forward()) {
        const auto rounds = backtest_folds(input);
        if (!rounds.empty() && rounds.front().first < end)
            return {static_cast<unsigned>(rounds.front().first), end};
    }
    if ((request.scope() == r::EvaluationRequest::DEVELOPMENT
            || request.scope() == r::EvaluationRequest::HOLDOUT)
        && !input.holdout_day().empty()) {
        const auto split =
            static_cast<unsigned>(std::ranges::count_if(result.settlements(), [&](const auto& day) {
                return day.trading_day() < input.holdout_day();
            }));
        if (split && split < end)
            return request.scope() == r::EvaluationRequest::DEVELOPMENT
                ? std::pair<unsigned, unsigned>{0, split}
                : std::pair<unsigned, unsigned>{split, end};
    }
    throw std::invalid_argument("evaluation scope is not present in the fixed backtest");
}

void validate_evaluation_source(
    const r::EvaluationInput& value,
    const backtest::proto::BacktestInput& input,
    const backtest::proto::BacktestResult& result)
{
    validate_evaluation_input(value);
    validate_backtest_result(input, result);
    const auto range = evaluation_range(value.request(), input, result);
    if (value.source().settlements() != static_cast<unsigned>(result.settlements_size())
        || value.source().first_settlement() != range.first
        || value.source().end_settlement() != range.second)
        throw std::invalid_argument("evaluation source range differs from its captured role");
    if (value.request().include_ledger()
        && (value.request().scope() != r::EvaluationRequest::FULL_ACCOUNT
            || input.paper().contracts_size() != 1
            || input.paper().contracts(0).dataset().interval_minutes() != 0
            || result.fill_days_size() != result.account().fills_size()))
        throw std::invalid_argument(
            "ledger attribution requires a full-account single-contract daily source with recorded fill days");
    if (value.request().include_contributions()
        && (value.request().scope() != r::EvaluationRequest::FULL_ACCOUNT
            || std::ranges::any_of(
                input.paper().contracts(),
                [](const auto& c) {
                    return c.dataset().interval_minutes() != 0;
                })
            || result.fill_days_size() != result.account().fills_size()))
        throw std::invalid_argument(
            "contract contributions require a full-account daily source with recorded fill days");
    const auto& currency = input.paper().contracts(0).dataset().contract().currency();
    if (currency.empty()
        || std::ranges::any_of(input.paper().contracts(), [&](const auto& contract) {
               return contract.dataset().contract().currency() != currency;
           }))
        throw std::invalid_argument("evaluation requires one explicit account currency");
    if (value.request().has_static_stress()) {
        const auto& last = result.settlements(static_cast<int>(range.second - 1));
        if (last.contracts_size() != 1 || !last.has_occupancy()
            || !last.contracts(0).has_gross_quantity())
            throw std::invalid_argument(
                "static stress requires one original contract and recorded gross lots and funds");
    }
}

void validate_evaluation_result(const r::EvaluationInput& input, const r::EvaluationResult& value)
{
    validate_evaluation_input(input);
    validate_message(value);
    ledger_shape(input, value);
    contributions_shape(input, value);
    const auto days = input.source().end_settlement() - input.source().first_settlement();
    (void)parse_trading_date(value.first_day());
    (void)parse_trading_date(value.last_day());
    if (value.first_day() > value.last_day() || value.trading_days() != days
        || value.currency().empty() || value.begin_ns() < 0 || value.end_ns() < value.begin_ns()
        || value.opening_is_full_period() != (input.source().first_settlement() > 0)
        || value.complete_return_periods() > days - (value.opening_is_full_period() ? 0U : 1U)
        || value.calendar_year_days() != 365.2425
        || value.net_profit().units()
            != (Decimal::from_raw(value.closing_equity().units())
                - Decimal::from_raw(value.opening_equity().units()))
                .raw())
        invalid_result();
    for (const auto* entry :
        {&value.total_return(),
            &value.max_drawdown(),
            &value.winning_days(),
            &value.annual_return(),
            &value.annual_volatility(),
            &value.sharpe(),
            &value.calmar()})
        metric_shape(*entry);
    if ((!input.request().method().has_annual_periods()
            && (value.annual_volatility().has_value() || value.sharpe().has_value()))
        || (input.request().method().risk_free()
                != backtest::proto::PerformanceAssumptions::EXPLICIT_ZERO
            && value.sharpe().has_value()))
        invalid_result();
    validate_loss_path(input, value);
    validate_exposure(input, value);
    if (input.request().has_static_stress() != value.has_static_stress())
        invalid_result();
    if (value.has_static_stress()) {
        const auto& stress = value.static_stress();
        if (!value.has_exposure() || value.exposure().positions_size() != 1
            || !value.exposure().has_funds() || !value.exposure().positions(0).has_gross_quantity()
            || !stress.has_mark_change() || !stress.has_equity() || !stress.has_margin()
            || !stress.has_available() || !stress.has_gap() || stress.margin().units() < 0
            || stress.gap().units() < 0
            || stress.equity().units()
                != (Decimal::from_raw(value.closing_equity().units())
                    + Decimal::from_raw(stress.mark_change().units()))
                    .raw()
            || stress.available().units()
                != (std::min(
                        Decimal::from_raw(value.exposure().balance().units()),
                        Decimal::from_raw(stress.equity().units()))
                    - Decimal::from_raw(stress.margin().units())
                    - Decimal::from_raw(value.exposure().funds().frozen().units()))
                    .raw()
            || stress.gap().units()
                != (stress.available().units() < 0
                        ? (Decimal{} - Decimal::from_raw(stress.available().units())).raw()
                        : 0))
            invalid_result();
    }
    const std::array limits{
        r::EvaluationResult::CASH_FLOWS_NOT_EVALUATED,
        r::EvaluationResult::BENCHMARK_NOT_EVALUATED,
        (input.request().include_daily_drawdown() || input.request().include_exposure())
            ? r::EvaluationResult::RISK_AND_STRESS_INCOMPLETE
            : r::EvaluationResult::RISK_AND_STRESS_NOT_EVALUATED,
        r::EvaluationResult::INDEPENDENCE_NOT_ESTABLISHED,
        r::EvaluationResult::HISTORICAL_AVAILABILITY_NOT_VERIFIED,
        r::EvaluationResult::HUMAN_CONCLUSION_NOT_RECORDED
    };
    if (value.limitations_size() != static_cast<int>(limits.size()))
        invalid_result();
    for (std::size_t i = 0; i < limits.size(); ++i)
        if (value.limitations(static_cast<int>(i)) != limits[i])
            invalid_result();
}

r::EvaluationRequest encode_evaluation_request(const Json& value)
{
    auto required = value;
    required.erase("include_ledger");
    required.erase("include_contributions");
    require_fields(
        required,
        {"source",
            "question",
            "scope",
            "method",
            "include_daily_drawdown",
            "include_exposure",
            "static_stress"});
    const auto& object = value.at("source");
    require_fields(object, {"node", "service", "domain", "kind", "id", "version"});
    r::EvaluationRequest result;
    auto& source = *result.mutable_source();
    source.mutable_owner()->set_node_id(object.at("node").get<std::string>());
    source.mutable_owner()->set_service_id(object.at("service").get<std::string>());
    source.mutable_owner()->set_domain(object.at("domain").get<std::string>());
    source.set_kind(object.at("kind").get<std::string>());
    source.set_id(object.at("id").get<std::string>());
    source.set_version(object.at("version").get<std::string>());
    result.set_question(value.at("question").get<std::string>());
    if (value.contains("include_ledger"))
        result.set_include_ledger(value.at("include_ledger").get<bool>());
    if (value.contains("include_contributions"))
        result.set_include_contributions(value.at("include_contributions").get<bool>());
    result.set_include_daily_drawdown(value.at("include_daily_drawdown").get<bool>());
    result.set_include_exposure(value.at("include_exposure").get<bool>());
    if (!value.at("static_stress").is_null()) {
        const auto& scenario = value.at("static_stress");
        require_fields(scenario, {"purpose", "price", "margin_rate", "funds_model"});
        if (scenario.at("funds_model") != "paper_balance_equity_minimum")
            throw std::invalid_argument(
                "static stress requires an explicit scenario and original exposure");
        auto& target = *result.mutable_static_stress();
        target.set_purpose(scenario.at("purpose").get<std::string>());
        target.mutable_price()->set_units(
            Decimal::parse(scenario.at("price").get<std::string>()).raw());
        target.mutable_margin_rate()->set_units(
            Decimal::parse(scenario.at("margin_rate").get<std::string>()).raw());
        target.set_funds_model(r::EvaluationStressScenario::PAPER_BALANCE_EQUITY_MINIMUM);
    }
    const auto scope = value.at("scope").get<std::string>();
    bool matched = false;
    for (const auto option :
        {r::EvaluationRequest::FULL_ACCOUNT,
            r::EvaluationRequest::DEVELOPMENT,
            r::EvaluationRequest::HOLDOUT,
            r::EvaluationRequest::OUT_OF_SAMPLE})
        if (scope_name(option) == scope) {
            result.set_scope(option);
            matched = true;
            break;
        }
    if (!matched)
        throw std::invalid_argument("invalid evaluation scope");
    *result.mutable_method() = encode_performance_assumptions(value.at("method"));
    validate_evaluation_request(result);
    return result;
}

Json decode_evaluation_input(const r::EvaluationInput& value)
{
    validate_evaluation_input(value);
    const auto& source = value.source();
    return {
        {"request", request_json(value.request())},
        {"source",
            {{"input_sha256", source.input_sha256()},
                {"input_bytes", std::to_string(source.input_bytes())},
                {"attempt", source.attempt()},
                {"program_artifact", source.program_artifact()},
                {"settlements", source.settlements()},
                {"first_settlement", source.first_settlement()},
                {"end_settlement", source.end_settlement()}}}
    };
}

Json decode_evaluation_result(const r::EvaluationInput& input, const r::EvaluationResult& value)
{
    validate_evaluation_result(input, value);
    Json limits = Json::array();
    for (const auto limitation : value.limitations())
        limits.push_back(r::EvaluationResult::Limitation_Name(limitation));
    Json result{
        {"first_day", value.first_day()},
        {"last_day", value.last_day()},
        {"begin_ns", std::to_string(value.begin_ns())},
        {"end_ns", std::to_string(value.end_ns())},
        {"opening_equity", Decimal::from_raw(value.opening_equity().units()).str()},
        {"closing_equity", Decimal::from_raw(value.closing_equity().units()).str()},
        {"fees", Decimal::from_raw(value.fees().units()).str()},
        {"net_profit", Decimal::from_raw(value.net_profit().units()).str()},
        {"currency", value.currency()},
        {"trading_days", value.trading_days()},
        {"complete_return_periods", value.complete_return_periods()},
        {"opening_is_full_period", value.opening_is_full_period()},
        {"short_calendar_span", value.short_calendar_span()},
        {"calendar_year_days", value.calendar_year_days()},
        {"total_return", metric(value.total_return())},
        {"max_drawdown", metric(value.max_drawdown())},
        {"winning_days", metric(value.winning_days())},
        {"annual_return", metric(value.annual_return())},
        {"annual_volatility", metric(value.annual_volatility())},
        {"sharpe", metric(value.sharpe())},
        {"calmar", metric(value.calmar())},
        {"limitations", std::move(limits)},
        {"daily_drawdown", loss_path_json(value)},
        {"exposure", exposure_json(value)},
        {"static_stress", stress_result_json(value)}
    };
    if (value.has_ledger())
        result["ledger"] = ledger_json(value.ledger());
    if (value.has_contributions())
        result["contributions"] = contributions_json(value.contributions());
    return result;
}

} // namespace asterion::protocol
