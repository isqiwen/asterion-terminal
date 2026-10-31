#include <asterion/futures/performance.hpp>
#include <asterion/base/error.hpp>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace asterion {
namespace {

// Subtract before converting, so small changes of large balances survive.
long double change(Decimal from, Decimal to)
{
    return static_cast<long double>(to.raw() - from.raw()) / static_cast<long double>(from.raw());
}

} // namespace

std::string_view performance_unavailable_name(PerformanceUnavailable reason)
{
    switch (reason) {
        case PerformanceUnavailable::none:
            return "available";
        case PerformanceUnavailable::missing_time:
            return "source_time_not_recorded";
        case PerformanceUnavailable::non_positive_duration:
            return "non_positive_observation_duration";
        case PerformanceUnavailable::insufficient_periods:
            return "fewer_than_two_complete_returns";
        case PerformanceUnavailable::non_positive_equity:
            return "non_positive_equity_path";
        case PerformanceUnavailable::missing_annual_periods:
            return "annual_periods_not_configured";
        case PerformanceUnavailable::missing_risk_free:
            return "risk_free_not_configured";
        case PerformanceUnavailable::zero_deviation:
            return "constant_returns";
        case PerformanceUnavailable::zero_drawdown:
            return "no_drawdown";
        case PerformanceUnavailable::numerical_overflow:
            return "non_finite_statistic";
        case PerformanceUnavailable::shorter_than_one_year:
            return "observation_shorter_than_one_year";
    }
    throw std::logic_error("unknown performance availability reason");
}

Performance performance(
    Decimal deposit,
    std::span<const EquityDay> days,
    std::span<const Decimal> marks,
    const PerformanceContext& context)
{
    if (deposit <= Decimal{} || days.empty())
        throw std::invalid_argument("performance needs a positive deposit and at least one day");
    if (context.begin_ns.has_value() != context.end_ns.has_value()
        || (context.begin_ns && (*context.begin_ns < 0 || *context.end_ns < *context.begin_ns))
        || (context.annual_periods
            && (!std::isfinite(*context.annual_periods) || *context.annual_periods <= 0)))
        throw std::invalid_argument("invalid explicit performance context");
    for (std::size_t i = 1; i < days.size(); ++i)
        if (days[i].day <= days[i - 1].day)
            throw std::invalid_argument("performance days must ascend");
    Performance result{};
    result.context = context;
    result.trading_days = days.size();
    result.total_return = static_cast<double>(change(deposit, days.back().equity));
    auto peak = deposit;
    long double drawdown = 0;
    for (const auto equity : marks) {
        peak = std::max(peak, equity);
        drawdown = std::max(drawdown, -change(peak, equity));
    }
    result.max_drawdown = static_cast<double>(drawdown);
    std::vector<long double> returns;
    auto previous = deposit;
    bool positive = true;
    std::size_t winning = 0;
    for (std::size_t i = 0; i < days.size(); ++i) {
        const auto& day = days[i];
        winning += day.equity > previous;
        if (positive && (i || context.opening_is_full_period))
            returns.push_back(change(previous, day.equity));
        previous = day.equity;
        positive = positive && previous > Decimal{};
    }
    result.return_periods = returns.size();
    result.winning_days = static_cast<double>(winning) / static_cast<double>(days.size());
    if (!positive) {
        result.annual_return_reason = result.annual_volatility_reason = result.sharpe_reason =
            result.calmar_reason = PerformanceUnavailable::non_positive_equity;
        return result;
    }
    if (context.begin_ns) {
        const auto elapsed = *context.end_ns - *context.begin_ns;
        const long double years =
            static_cast<long double>(elapsed) / 1000000000.0L / performance_year_seconds;
        result.short_calendar_span = elapsed && elapsed < performance_annual_span_ns;
        if (!elapsed)
            result.annual_return_reason = PerformanceUnavailable::non_positive_duration;
        // A return over less than a year is not annualized: compounding a short
        // span to a year turns days of noise into meaningless magnitudes.
        else if (result.short_calendar_span)
            result.annual_return_reason = PerformanceUnavailable::shorter_than_one_year;
        else {
            const auto relative = change(deposit, days.back().equity);
            // Small changes need log1p; a very large loss must not round its positive growth to zero.
            const auto log_growth = std::abs(relative) < 0.5L
                ? std::log1p(relative)
                : std::log(
                      static_cast<long double>(days.back().equity.raw())
                      / static_cast<long double>(deposit.raw()));
            // About a year of Decimal-bounded growth keeps the rate finite.
            result.annual_return = static_cast<double>(std::expm1(log_growth / years));
            result.annual_return_reason = PerformanceUnavailable::none;
        }
    }
    if (returns.size() < 2)
        result.annual_volatility_reason = result.sharpe_reason =
            PerformanceUnavailable::insufficient_periods;
    else if (!context.annual_periods)
        result.annual_volatility_reason = result.sharpe_reason =
            PerformanceUnavailable::missing_annual_periods;
    else {
        long double mean = 0, squares = 0;
        for (const auto value : returns)
            mean += value;
        mean /= static_cast<long double>(returns.size());
        for (const auto value : returns)
            squares += (value - mean) * (value - mean);
        const auto deviation = std::sqrt(squares / static_cast<long double>(returns.size() - 1));
        const auto scaling = std::sqrt(static_cast<long double>(*context.annual_periods));
        const auto volatility = static_cast<double>(deviation * scaling);
        if (std::isfinite(volatility)) {
            result.annual_volatility = volatility;
            result.annual_volatility_reason = PerformanceUnavailable::none;
        } else
            result.annual_volatility_reason = PerformanceUnavailable::numerical_overflow;
        if (!context.explicitly_zero_risk_free)
            result.sharpe_reason = PerformanceUnavailable::missing_risk_free;
        else if (deviation == 0)
            result.sharpe_reason = PerformanceUnavailable::zero_deviation;
        else {
            const auto ratio = static_cast<double>(mean / deviation * scaling);
            if (std::isfinite(ratio)) {
                result.sharpe = ratio;
                result.sharpe_reason = PerformanceUnavailable::none;
            } else
                result.sharpe_reason = PerformanceUnavailable::numerical_overflow;
        }
    }
    if (!result.annual_return)
        result.calmar_reason = result.annual_return_reason;
    else if (drawdown == 0)
        result.calmar_reason = PerformanceUnavailable::zero_drawdown;
    else {
        const auto ratio = *result.annual_return / result.max_drawdown;
        if (std::isfinite(ratio)) {
            result.calmar = ratio;
            result.calmar_reason = PerformanceUnavailable::none;
        } else
            result.calmar_reason = PerformanceUnavailable::numerical_overflow;
    }
    return result;
}

std::vector<MonthlyContribution>
monthly_contributions(Decimal deposit, std::span<const SettlementDay> days)
{
    if (deposit <= Decimal{} || days.empty())
        throw std::invalid_argument(
            "monthly contributions require a positive deposit and settlements");
    std::vector<MonthlyContribution> result;
    auto previous_equity = deposit;
    Decimal previous_fees, opening_fees;
    std::chrono::year_month month{};
    for (std::size_t i = 0; i < days.size(); ++i) {
        const auto& day = days[i];
        if ((i && day.day <= days[i - 1].day) || day.fees < previous_fees)
            throw std::invalid_argument(
                "settlement dates must ascend and cumulative fees must not decrease");
        const std::chrono::year_month_day date{day.day};
        const auto current = date.year() / date.month();
        if (result.empty() || current != month) {
            auto& value = result.emplace_back(MonthlyContribution{});
            value.first_day = day.day;
            value.opening_equity = previous_equity;
            month = current;
            opening_fees = previous_fees;
        }
        auto& value = result.back();
        value.last_day = day.day;
        ++value.trading_days;
        value.closing_equity = day.equity;
        value.net = day.equity - value.opening_equity;
        value.fees = day.fees - opening_fees;
        value.gross = value.net + value.fees;
        value.return_contribution = static_cast<double>(
            static_cast<long double>(value.net.raw()) / static_cast<long double>(deposit.raw()));
        previous_equity = day.equity;
        previous_fees = day.fees;
    }
    return result;
}

Trades trades(std::span<const TradedFill> fills, std::span<const Decimal> multipliers)
{
    // What each contract holds now, and what its open trade has paid and received.
    std::vector<Decimal> position(multipliers.size()), cash(multipliers.size()),
        fees(multipliers.size());
    std::vector<TradeContribution> contributions(multipliers.size());
    Trades result{};
    long double won = 0, lost = 0;
    std::vector<std::size_t> first_fill(multipliers.size());
    for (std::size_t index = 0; index < fills.size(); ++index) {
        const auto& fill = fills[index];
        auto& held = position.at(fill.contract);
        auto& paid = cash[fill.contract];
        auto& charged = fees[fill.contract];
        if (held == Decimal{})
            first_fill[fill.contract] = index;
        const auto after = fill.buy ? held + fill.quantity : held - fill.quantity;
        if ((held > Decimal{} && after < Decimal{}) || (held < Decimal{} && after > Decimal{}))
            throw std::invalid_argument("a fill takes a position through flat to the other side");
        const auto amount = fill.price * fill.quantity;
        paid = fill.buy ? paid - amount : paid + amount;
        charged = charged + fill.fee;
        held = after;
        if (held != Decimal{})
            continue;
        const auto gross = paid * multipliers[fill.contract];
        const auto net = gross - charged;
        result.closed.push_back(
            {fill.contract, gross, charged, net, first_fill[fill.contract], index});
        auto& contribution = contributions[fill.contract];
        contribution.contract = fill.contract;
        ++contribution.count;
        contribution.gross = contribution.gross + gross;
        contribution.fees = contribution.fees + charged;
        contribution.net = contribution.net + net;
        const auto profit = static_cast<long double>(net.raw());
        paid = {};
        charged = {};
        if (profit > 0) {
            ++result.winning;
            won += profit;
        } else if (profit < 0) {
            ++result.losing;
            lost += profit;
        }
    }
    for (const auto& contribution : contributions)
        if (contribution.count)
            result.by_contract.push_back(contribution);
    for (std::size_t i = 0; i < fees.size(); ++i) {
        result.open_fees = result.open_fees + fees[i];
        if (position[i] != Decimal{})
            result.open.push_back({i, first_fill[i], position[i], fees[i]});
    }
    // Raw decimals carry eight places.
    if (result.winning)
        result.average_win = static_cast<double>(won / result.winning / 100000000);
    if (result.losing)
        result.average_loss = static_cast<double>(lost / result.losing / 100000000);
    if (result.winning && result.losing)
        result.payoff = *result.average_win / -*result.average_loss;
    return result;
}

DailyDrawdown daily_drawdown(std::span<const EquityDay> days, std::stop_token stop)
{
    if (days.empty())
        throw std::invalid_argument("daily drawdown requires an observed settlement");
    DailyDrawdown result;
    result.points.reserve(days.size());
    std::size_t peak = 0;
    std::optional<std::size_t> active;
    double maximum = 0;
    std::size_t maximum_index = 0;
    bool positive_path = true;
    for (std::size_t i = 0; i < days.size(); ++i) {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "daily drawdown cancelled");
        if (i && days[i].day <= days[i - 1].day)
            throw std::invalid_argument(
                "daily drawdown observations must ascend by original trading day");
        if (days[i].equity >= days[peak].equity) {
            if (active) {
                auto& episode = result.episodes[*active];
                episode.recovery_index = i;
                episode.observed_end_index = i;
                active.reset();
            }
            peak = i;
        } else {
            if (!active) {
                active = result.episodes.size();
                result.episodes.push_back({peak, i, i, std::nullopt});
            }
            auto& episode = result.episodes[*active];
            if (days[i].equity < days[episode.trough_index].equity)
                episode.trough_index = i;
            episode.observed_end_index = i;
        }
        const auto loss = days[peak].equity - days[i].equity;
        std::optional<double> fraction;
        if (days[peak].equity > Decimal{} && days[i].equity > Decimal{}) {
            fraction = static_cast<double>(
                static_cast<long double>(loss.raw())
                / static_cast<long double>(days[peak].equity.raw()));
            if (*fraction > maximum) {
                maximum = *fraction;
                maximum_index = i;
            }
        } else
            positive_path = false;
        result.points.push_back({peak, days[peak].equity, loss, fraction});
        if (loss > result.max_loss) {
            result.max_loss = loss;
            result.max_loss_index = i;
        }
    }
    if (!positive_path)
        result.unavailable = DrawdownUnavailable::non_positive_equity;
    else if (days.size() < 2)
        result.unavailable = DrawdownUnavailable::fewer_than_two_observations;
    else {
        result.max_fraction = maximum;
        result.max_fraction_index = maximum_index;
    }
    return result;
}

} // namespace asterion
