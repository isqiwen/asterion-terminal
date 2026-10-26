#include "research_evaluation.hpp"
#include "performance.hpp"
#include "account_exposure.hpp"
#include "account_reconciliation.hpp"
#include "static_stress.hpp"
#include <algorithm>
#include "program_files.hpp"
#include "task_payload.hpp"
#include <asterion/protocol/research_evaluation.hpp>
#include <asterion/domain/trading_days.hpp>
#include <asterion/foundation/error.hpp>
#include <array>

namespace asterion::tasks {
namespace {
namespace r = research::proto;

r::EvaluationMetric::State state(PerformanceUnavailable value)
{
    switch (value) {
        case PerformanceUnavailable::none:
            return r::EvaluationMetric::AVAILABLE;
        case PerformanceUnavailable::missing_time:
            return r::EvaluationMetric::SOURCE_TIME_NOT_RECORDED;
        case PerformanceUnavailable::non_positive_duration:
            return r::EvaluationMetric::NON_POSITIVE_OBSERVATION_DURATION;
        case PerformanceUnavailable::insufficient_periods:
            return r::EvaluationMetric::FEWER_THAN_TWO_COMPLETE_RETURNS;
        case PerformanceUnavailable::non_positive_equity:
            return r::EvaluationMetric::NON_POSITIVE_EQUITY_PATH;
        case PerformanceUnavailable::missing_annual_periods:
            return r::EvaluationMetric::ANNUAL_PERIODS_NOT_CONFIGURED;
        case PerformanceUnavailable::missing_risk_free:
            return r::EvaluationMetric::RISK_FREE_NOT_CONFIGURED;
        case PerformanceUnavailable::zero_deviation:
            return r::EvaluationMetric::CONSTANT_RETURNS;
        case PerformanceUnavailable::zero_drawdown:
            return r::EvaluationMetric::NO_DRAWDOWN;
        case PerformanceUnavailable::numerical_overflow:
            return r::EvaluationMetric::NON_FINITE_STATISTIC;
        case PerformanceUnavailable::shorter_than_one_year:
            return r::EvaluationMetric::SHORTER_THAN_ONE_YEAR;
    }
    throw std::logic_error("unknown performance availability reason");
}

void metric(r::EvaluationMetric& target, std::optional<double> value, PerformanceUnavailable reason)
{
    target.set_state(state(reason));
    if (value)
        target.set_value(*value);
}

r::EvaluationMetric::State state(ExposureUnavailable reason)
{
    switch (reason) {
        case ExposureUnavailable::none:
            return r::EvaluationMetric::AVAILABLE;
        case ExposureUnavailable::gross_not_recorded:
            return r::EvaluationMetric::GROSS_NOT_RECORDED;
        case ExposureUnavailable::margin_not_recorded:
            return r::EvaluationMetric::MARGIN_NOT_RECORDED;
        case ExposureUnavailable::non_positive_equity:
            return r::EvaluationMetric::NON_POSITIVE_EQUITY_PATH;
        case ExposureUnavailable::zero_gross_notional:
            return r::EvaluationMetric::ZERO_GROSS_NOTIONAL;
    }
    throw std::logic_error("unknown exposure availability reason");
}

void metric(r::EvaluationMetric& target, const ExposureRatio& value)
{
    target.set_state(state(value.unavailable));
    if (value.value)
        target.set_value(*value.value);
}

void stopped(std::stop_token stop)
{
    if (stop.stop_requested())
        throw std::runtime_error("task cancelled");
}

} // namespace

r::EvaluationInput capture_evaluation_input(
    const resource::proto::Owner& owner,
    const r::EvaluationRequest& request,
    const task::proto::TaskResponse& response,
    const std::string& input_digest,
    std::uint64_t input_bytes)
{
    protocol::validate_evaluation_request(request);
    const auto& original = response.result_task();
    if (request.source().owner().SerializeAsString() != owner.SerializeAsString())
        throw Error(ErrorCode::conflict, "evaluation source must belong to this Research service");
    if (!response.has_result_task() || !response.has_backtest() || !original.has_input()
        || original.kind() != task::proto::BACKTEST || original.state() != task::proto::SUCCEEDED
        || original.has_retirement() || original.id() != request.source().id()
        || original.result_digest() != request.source().version())
        throw Error(ErrorCode::conflict, "evaluation source is unavailable or changed");
    if (original.program_artifact().empty())
        throw Error(ErrorCode::unavailable, "evaluation source program is unavailable");
    r::EvaluationInput result;
    *result.mutable_request() = request;
    auto& source = *result.mutable_source();
    source.set_input_sha256(input_digest);
    source.set_input_bytes(input_bytes);
    source.set_attempt(original.attempt());
    source.set_program_artifact(original.program_artifact());
    source.set_settlements(response.backtest().settlements_size());
    const auto range = protocol::evaluation_range(request, original.input(), response.backtest());
    source.set_first_settlement(range.first);
    source.set_end_settlement(range.second);
    protocol::validate_evaluation_source(result, original.input(), response.backtest());
    return result;
}

r::EvaluationResult compute_evaluation(
    const r::EvaluationInput& input,
    const std::filesystem::path& root,
    std::stop_token stop,
    const TaskProgress& progress)
{
    protocol::validate_evaluation_input(input);
    stopped(stop);
    const auto& source = input.source();
    const auto& identity = input.request().source();
    (void)programs::Files(root / "programs", identity.owner())
        .verify(source.program_artifact(), stop);
    backtest::proto::BacktestInput original;
    backtest::proto::BacktestResult evidence;
    payload::parse(
        payload::read(
            root / identity.id() / "input.pb",
            source.input_sha256(),
            payload::max_input_bytes,
            source.input_bytes()),
        original);
    payload::parse(
        payload::read(
            root / identity.id() / "results" / (std::to_string(source.attempt()) + ".pb"),
            identity.version(),
            payload::max_result_bytes),
        evidence);
    protocol::validate_evaluation_source(input, original, evidence);
    stopped(stop);
    const auto first = source.first_settlement(), end = source.end_settlement();
    const auto& last = evidence.settlements(static_cast<int>(end - 1));
    const auto opening = Decimal::from_raw(
        first ? evidence.settlements(static_cast<int>(first - 1)).equity().units()
              : original.paper().deposit().units());
    const auto closing = Decimal::from_raw(last.equity().units());
    r::EvaluationResult result;
    result.set_first_day(evidence.settlements(static_cast<int>(first)).trading_day());
    result.set_last_day(last.trading_day());
    result.set_begin_ns(
        first ? evidence.settlements(static_cast<int>(first - 1)).timestamp_ns()
              : evidence.equity(0).timestamp_ns());
    result.set_end_ns(last.timestamp_ns());
    result.mutable_opening_equity()->set_units(opening.raw());
    result.mutable_closing_equity()->set_units(closing.raw());
    result.mutable_net_profit()->set_units((closing - opening).raw());
    result.mutable_fees()->set_units(
        (Decimal::from_raw(last.fees().units())
            - Decimal::from_raw(
                first ? evidence.settlements(static_cast<int>(first - 1)).fees().units() : 0))
            .raw());
    result.set_currency(original.paper().contracts(0).dataset().contract().currency());
    result.set_trading_days(end - first);
    result.set_opening_is_full_period(first > 0);
    result.set_calendar_year_days(365.2425);
    result.set_short_calendar_span(
        static_cast<long double>(result.end_ns() - result.begin_ns()) / 1000000000.0L
        < performance_year_seconds);
    for (const auto limitation :
        {r::EvaluationResult::CASH_FLOWS_NOT_EVALUATED,
            r::EvaluationResult::BENCHMARK_NOT_EVALUATED,
            (input.request().include_daily_drawdown() || input.request().include_exposure())
                ? r::EvaluationResult::RISK_AND_STRESS_INCOMPLETE
                : r::EvaluationResult::RISK_AND_STRESS_NOT_EVALUATED,
            r::EvaluationResult::INDEPENDENCE_NOT_ESTABLISHED,
            r::EvaluationResult::HISTORICAL_AVAILABILITY_NOT_VERIFIED,
            r::EvaluationResult::HUMAN_CONCLUSION_NOT_RECORDED})
        result.add_limitations(limitation);
    std::vector<EquityDay> days;
    days.reserve(end - first);
    for (auto i = first; i < end; ++i) {
        stopped(stop);
        const auto& entry = evidence.settlements(static_cast<int>(i));
        days.push_back(
            {std::chrono::sys_days(parse_trading_date(entry.trading_day())),
                Decimal::from_raw(entry.equity().units())});
    }
    if (opening <= Decimal{}) {
        for (auto* entry :
            {result.mutable_total_return(),
                result.mutable_max_drawdown(),
                result.mutable_winning_days(),
                result.mutable_annual_return(),
                result.mutable_annual_volatility(),
                result.mutable_sharpe(),
                result.mutable_calmar()})
            metric(*entry, std::nullopt, PerformanceUnavailable::non_positive_equity);
    } else {
        std::vector<Decimal> marks;
        unsigned settled = 0;
        for (const auto& point : evidence.equity()) {
            stopped(stop);
            if (settled >= first && settled < end)
                marks.push_back(Decimal::from_raw(point.equity().units()));
            if (point.event() == backtest::proto::DAILY_SETTLEMENT)
                ++settled;
            if (settled >= end)
                break;
        }
        PerformanceContext context;
        context.begin_ns = result.begin_ns();
        context.end_ns = result.end_ns();
        context.opening_is_full_period = first > 0;
        if (input.request().method().has_annual_periods())
            context.annual_periods = input.request().method().annual_periods();
        context.explicitly_zero_risk_free = input.request().method().risk_free()
            == backtest::proto::PerformanceAssumptions::EXPLICIT_ZERO;
        const auto values = performance(opening, days, marks, context);
        result.set_complete_return_periods(values.return_periods);
        metric(*result.mutable_total_return(), values.total_return, PerformanceUnavailable::none);
        metric(*result.mutable_max_drawdown(), values.max_drawdown, PerformanceUnavailable::none);
        metric(*result.mutable_winning_days(), values.winning_days, PerformanceUnavailable::none);
        metric(*result.mutable_annual_return(), values.annual_return, values.annual_return_reason);
        metric(
            *result.mutable_annual_volatility(),
            values.annual_volatility,
            values.annual_volatility_reason);
        metric(*result.mutable_sharpe(), values.sharpe, values.sharpe_reason);
        metric(*result.mutable_calmar(), values.calmar, values.calmar_reason);
    }
    if (input.request().include_daily_drawdown()) {
        const auto measured = daily_drawdown(days, stop);
        auto& path = *result.mutable_daily_drawdown();
        path.mutable_max_loss()->set_units(measured.max_loss.raw());
        path.set_max_loss_source_index(first + measured.max_loss_index);
        auto* maximum = path.mutable_max_fraction();
        if (measured.max_fraction) {
            maximum->set_state(r::EvaluationMetric::AVAILABLE);
            maximum->set_value(*measured.max_fraction);
            path.set_max_fraction_source_index(first + measured.max_fraction_index.value());
        } else
            maximum->set_state(
                measured.unavailable == DrawdownUnavailable::non_positive_equity
                    ? r::EvaluationMetric::NON_POSITIVE_EQUITY_PATH
                    : r::EvaluationMetric::FEWER_THAN_TWO_OBSERVATIONS);
        for (std::size_t i = 0; i < measured.points.size(); ++i) {
            stopped(stop);
            const auto& original_row = evidence.settlements(static_cast<int>(first + i));
            const auto& measured_row = measured.points[i];
            auto* row = path.add_points();
            row->set_source_index(first + i);
            row->set_trading_day(original_row.trading_day());
            row->set_timestamp_ns(original_row.timestamp_ns());
            *row->mutable_equity() = original_row.equity();
            row->set_peak_source_index(first + measured_row.peak_index);
            row->mutable_peak_equity()->set_units(measured_row.peak_equity.raw());
            row->mutable_loss()->set_units(measured_row.loss.raw());
            auto* fraction = row->mutable_fraction();
            fraction->set_state(
                measured_row.fraction ? r::EvaluationMetric::AVAILABLE
                                      : r::EvaluationMetric::NON_POSITIVE_EQUITY_PATH);
            if (measured_row.fraction)
                fraction->set_value(*measured_row.fraction);
        }
        for (const auto& measured_episode : measured.episodes) {
            stopped(stop);
            auto* episode = path.add_episodes();
            episode->set_peak_source_index(first + measured_episode.peak_index);
            episode->set_trough_source_index(first + measured_episode.trough_index);
            episode->set_observed_end_source_index(first + measured_episode.observed_end_index);
            episode->set_peak_to_trough_intervals(
                measured_episode.trough_index - measured_episode.peak_index);
            episode->set_observed_intervals(
                measured_episode.observed_end_index - measured_episode.peak_index);
            if (measured_episode.recovery_index) {
                episode->set_recovery_source_index(first + *measured_episode.recovery_index);
                episode->set_peak_to_recovery_intervals(
                    *measured_episode.recovery_index - measured_episode.peak_index);
            }
        }
    }
    if (input.request().include_exposure()) {
        std::vector<ExposurePosition> positions;
        std::vector<const protocol::proto::Contract*> specifications;
        positions.reserve(last.contracts_size());
        specifications.reserve(last.contracts_size());
        for (const auto& original_row : last.contracts()) {
            stopped(stop);
            const auto spec =
                std::ranges::find_if(original.paper().contracts(), [&](const auto& contract) {
                    return contract.dataset().contract().venue() == original_row.venue()
                        && contract.dataset().contract().symbol() == original_row.symbol();
                });
            if (spec == original.paper().contracts().end())
                throw Error(
                    ErrorCode::conflict,
                    "exposure source contract is missing from fixed input");
            const auto& contract = spec->dataset().contract();
            specifications.push_back(&contract);
            positions.push_back(
                {Decimal::from_raw(original_row.price().units()),
                    Decimal::from_raw(contract.multiplier().units()),
                    Decimal::from_raw(original_row.position_quantity().units()),
                    original_row.has_gross_quantity() ? std::optional<Decimal>{Decimal::from_raw(
                                                            original_row.gross_quantity().units())}
                                                      : std::nullopt,
                    original_row.has_margin()
                        ? std::optional<Decimal>{Decimal::from_raw(original_row.margin().units())}
                        : std::nullopt});
        }
        const auto margin = last.has_occupancy()
            ? std::optional<Decimal>{Decimal::from_raw(last.occupancy().margin().units())}
            : std::nullopt;
        const auto measured = account_exposure(closing, margin, positions, stop);
        auto& slice = *result.mutable_exposure();
        slice.set_source_index(end - 1);
        slice.set_trading_day(last.trading_day());
        slice.set_timestamp_ns(last.timestamp_ns());
        *slice.mutable_balance() = last.balance();
        if (last.has_occupancy())
            *slice.mutable_funds() = last.occupancy();
        slice.mutable_covered_net_notional()->set_units(measured.covered_net_notional.raw());
        if (measured.covered_gross_notional)
            slice.mutable_covered_gross_notional()->set_units(
                measured.covered_gross_notional->raw());
        metric(*slice.mutable_net_to_equity(), measured.net_to_equity);
        metric(*slice.mutable_gross_to_equity(), measured.gross_to_equity);
        metric(*slice.mutable_margin_to_equity(), measured.margin_to_equity);
        for (int i = 0; i < last.contracts_size(); ++i) {
            stopped(stop);
            const auto& original_row = last.contracts(i);
            const auto& contract = *specifications[i];
            const auto& derived = measured.positions[i];
            auto* row = slice.add_positions();
            row->set_source_contract_index(i);
            row->set_venue(original_row.venue());
            row->set_symbol(original_row.symbol());
            row->set_product(contract.product());
            row->set_delivery_month(contract.delivery_month());
            *row->mutable_price() = original_row.price();
            *row->mutable_multiplier() = contract.multiplier();
            *row->mutable_quantity_increment() = contract.quantity_increment();
            *row->mutable_net_quantity() = original_row.position_quantity();
            if (original_row.has_gross_quantity())
                *row->mutable_gross_quantity() = original_row.gross_quantity();
            if (original_row.has_margin())
                *row->mutable_margin() = original_row.margin();
            if (derived.long_quantity)
                row->mutable_long_quantity()->set_units(derived.long_quantity->raw());
            if (derived.short_quantity)
                row->mutable_short_quantity()->set_units(derived.short_quantity->raw());
            row->mutable_net_notional()->set_units(derived.net_notional.raw());
            if (derived.gross_notional)
                row->mutable_gross_notional()->set_units(derived.gross_notional->raw());
            metric(*row->mutable_net_to_equity(), derived.net_to_equity);
            metric(*row->mutable_gross_to_equity(), derived.gross_to_equity);
            metric(*row->mutable_margin_to_equity(), derived.margin_to_equity);
            metric(*row->mutable_covered_gross_share(), derived.covered_gross_share);
        }
    }

    if (input.request().has_static_stress()) {
        const auto& slice = result.exposure();
        const auto& position = slice.positions(0);
        const auto& scenario = input.request().static_stress();
        const auto estimate = static_stress(
            Decimal::from_raw(slice.balance().units()),
            closing,
            Decimal::from_raw(slice.funds().frozen().units()),
            ExposurePosition{
                Decimal::from_raw(position.price().units()),
                Decimal::from_raw(position.multiplier().units()),
                Decimal::from_raw(position.net_quantity().units()),
                Decimal::from_raw(position.gross_quantity().units()),
                Decimal::from_raw(position.margin().units())
            },
            Decimal::from_raw(scenario.price().units()),
            Decimal::from_raw(scenario.margin_rate().units()),
            stop);
        auto& stress = *result.mutable_static_stress();
        stress.mutable_mark_change()->set_units(estimate.mark_change.raw());
        stress.mutable_equity()->set_units(estimate.equity.raw());
        stress.mutable_margin()->set_units(estimate.margin.raw());
        stress.mutable_available()->set_units(estimate.available.raw());
        stress.mutable_gap()->set_units(estimate.gap.raw());
    }
    if (input.request().include_ledger()) {
        const auto audit = reconcile_backtest(original, evidence, stop);
        if (audit.state != AccountReconciliation::State::available)
            throw std::invalid_argument(
                "ledger attribution requires a full-account single-contract daily source with recorded fill days");
        auto& ledger = *result.mutable_ledger();
        ledger.set_venue(audit.venue);
        ledger.set_symbol(audit.symbol);
        ledger.mutable_multiplier()->set_units(audit.multiplier.raw());
        ledger.mutable_deposit()->set_units(audit.deposit.raw());
        bool matched = true;
        const auto copy_checks = [&](const auto& checks, auto* target) {
            for (const auto& source : checks) {
                auto* check = target->Add();
                check->set_metric(source.metric);
                check->mutable_expected()->set_units(source.expected.raw());
                check->mutable_recorded()->set_units(source.recorded.raw());
                const auto residual = source.recorded - source.expected;
                check->mutable_residual()->set_units(residual.raw());
                matched = matched && residual == Decimal{};
            }
        };
        Decimal gross, fees;
        for (std::size_t i = 0; i < audit.days.size(); ++i) {
            stopped(stop);
            const auto& day = audit.days[i];
            const auto& original_day = evidence.settlements(static_cast<int>(i));
            auto* row = ledger.add_days();
            row->set_source_index(static_cast<unsigned>(i));
            row->set_trading_day(day.day);
            row->set_timestamp_ns(original_day.timestamp_ns());
            row->mutable_opening_quantity()->set_units(day.opening_quantity.raw());
            row->mutable_quantity_change()->set_units(day.quantity_change.raw());
            row->mutable_closing_quantity()->set_units(day.closing_quantity.raw());
            row->mutable_opening_balance()->set_units(day.opening_balance.raw());
            row->mutable_gross()->set_units(day.gross.raw());
            row->mutable_fees()->set_units(day.fees.raw());
            row->mutable_expected_balance()->set_units(day.expected_balance.raw());
            row->mutable_recorded_balance()->set_units(day.recorded_balance.raw());
            *row->mutable_recorded_equity() = original_day.equity();
            const auto residual = day.recorded_balance - day.expected_balance;
            row->mutable_cumulative_residual()->set_units(residual.raw());
            matched = matched && residual == Decimal{};
            for (const auto& fill : day.fills) {
                auto* target = row->add_fills();
                target->set_id(fill.id);
                target->set_order_id(fill.order_id);
                target->set_buy(fill.buy);
                target->mutable_quantity()->set_units(fill.quantity.raw());
                target->mutable_price()->set_units(fill.price.raw());
                target->mutable_fee()->set_units(fill.fee.raw());
            }
            copy_checks(day.checks, row->mutable_checks());
            gross = gross + day.gross;
            fees = fees + day.fees;
        }
        ledger.mutable_gross()->set_units(gross.raw());
        ledger.mutable_fees()->set_units(fees.raw());
        ledger.mutable_explained_net()->set_units((gross - fees).raw());
        *ledger.mutable_recorded_net() = result.net_profit();
        const auto residual = Decimal::from_raw(result.net_profit().units()) - (gross - fees);
        ledger.mutable_residual()->set_units(residual.raw());
        matched = matched && residual == Decimal{};
        Decimal net, gross_quantity;
        for (const auto& lot : evidence.account().positions()) {
            const auto quantity = Decimal::from_raw(lot.quantity().units());
            gross_quantity = gross_quantity + quantity;
            net = net + (lot.side() == protocol::proto::BUY ? quantity : Decimal{} - quantity);
        }
        ledger.mutable_closing_net_quantity()->set_units(net.raw());
        ledger.mutable_closing_gross_quantity()->set_units(gross_quantity.raw());
        copy_checks(audit.final_checks, ledger.mutable_final_checks());
        ledger.set_reconciled(matched);
    }
    if (input.request().include_contributions()) {
        const auto audit = contract_contributions(original, evidence, stop);
        auto& out = *result.mutable_contributions();
        out.mutable_deposit()->set_units(audit.deposit.raw());
        out.mutable_gross()->set_units(audit.gross.raw());
        out.mutable_fees()->set_units(audit.fees.raw());
        out.mutable_explained_net()->set_units(audit.explained_net.raw());
        out.mutable_recorded_net()->set_units(audit.recorded_net.raw());
        out.mutable_residual()->set_units(audit.residual.raw());
        out.set_reconciled(audit.reconciled);
        const auto copy_check = [](const AccountCheck& source, auto* target) {
            target->set_metric(source.metric);
            target->mutable_expected()->set_units(source.expected.raw());
            target->mutable_recorded()->set_units(source.recorded.raw());
            target->mutable_residual()->set_units((source.recorded - source.expected).raw());
        };
        const auto copy_checks = [&](const auto& source, auto* target) {
            for (const auto& check : source)
                copy_check(check, target->Add());
        };
        for (const auto& c : audit.contracts) {
            stopped(stop);
            auto* contract = out.add_contracts();
            contract->set_venue(c.venue);
            contract->set_symbol(c.symbol);
            contract->mutable_multiplier()->set_units(c.multiplier.raw());
            contract->mutable_gross()->set_units(c.gross.raw());
            contract->mutable_fees()->set_units(c.fees.raw());
            contract->mutable_net()->set_units(c.net.raw());
            copy_check(c.final_position, contract->mutable_final_position());
            for (const auto& day : c.days) {
                stopped(stop);
                auto* row = contract->add_days();
                row->set_source_index(static_cast<unsigned>(day.source_index));
                row->set_trading_day(day.day);
                row->set_timestamp_ns(
                    evidence.settlements(static_cast<int>(day.source_index)).timestamp_ns());
                row->mutable_settlement()->set_units(day.settlement.raw());
                row->mutable_opening_quantity()->set_units(day.opening_quantity.raw());
                row->mutable_quantity_change()->set_units(day.quantity_change.raw());
                row->mutable_closing_quantity()->set_units(day.closing_quantity.raw());
                row->mutable_gross()->set_units(day.gross.raw());
                row->mutable_fees()->set_units(day.fees.raw());
                for (const auto& f : day.fills) {
                    auto* fill = row->add_fills();
                    fill->set_id(f.id);
                    fill->set_order_id(f.order_id);
                    fill->set_buy(f.buy);
                    fill->mutable_quantity()->set_units(f.quantity.raw());
                    fill->mutable_price()->set_units(f.price.raw());
                    fill->mutable_fee()->set_units(f.fee.raw());
                }
                copy_checks(day.checks, row->mutable_checks());
            }
        }
        for (const auto& day : audit.days) {
            stopped(stop);
            auto* row = out.add_days();
            row->set_source_index(static_cast<unsigned>(day.source_index));
            row->set_trading_day(day.day);
            row->set_timestamp_ns(
                evidence.settlements(static_cast<int>(day.source_index)).timestamp_ns());
            row->mutable_gross()->set_units(day.gross.raw());
            row->mutable_fees()->set_units(day.fees.raw());
            row->mutable_expected_balance()->set_units(day.expected_balance.raw());
            row->mutable_recorded_balance()->set_units(day.recorded_balance.raw());
            row->mutable_recorded_equity()->set_units(day.recorded_equity.raw());
            row->mutable_cumulative_residual()->set_units(day.cumulative_residual.raw());
            copy_checks(day.checks, row->mutable_checks());
        }
        copy_checks(audit.final_checks, out.mutable_final_checks());
    }
    stopped(stop);
    protocol::validate_evaluation_result(input, result);
    if (progress)
        progress(end - first, end - first);
    return result;
}

} // namespace asterion::tasks
