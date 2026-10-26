#pragma once
#include <asterion/foundation/decimal.hpp>
#include <chrono>
#include <cstdint>
#include <string_view>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>

namespace asterion {

// Missing assumptions remain missing; source boundaries are original account observations.
struct PerformanceContext
{
    std::optional<std::int64_t> begin_ns, end_ns;
    std::optional<double> annual_periods;
    bool explicitly_zero_risk_free = false;
    bool opening_is_full_period = false;
};

enum class PerformanceUnavailable
{
    none,
    missing_time,
    non_positive_duration,
    insufficient_periods,
    non_positive_equity,
    missing_annual_periods,
    missing_risk_free,
    zero_deviation,
    zero_drawdown,
    numerical_overflow,
    shorter_than_one_year
};
std::string_view performance_unavailable_name(PerformanceUnavailable);
inline constexpr long double performance_year_seconds = 31556952.0L; // 365.2425 days

// What a day-by-day equity record says about a strategy. These are statistics
// for judging a result, not ledger values.
struct Performance
{
    std::size_t trading_days;
    // Final equity over the deposit, less one.
    double total_return;
    // The largest fall from an earlier peak, as a share of that peak.
    double max_drawdown;
    // The share of days that ended above the day before.
    double winning_days;
    std::optional<double> annual_return, annual_volatility, sharpe, calmar;
    PerformanceContext context;
    std::size_t return_periods = 0;
    bool short_calendar_span = false;
    PerformanceUnavailable annual_return_reason = PerformanceUnavailable::missing_time;
    PerformanceUnavailable annual_volatility_reason =
        PerformanceUnavailable::missing_annual_periods;
    PerformanceUnavailable sharpe_reason = PerformanceUnavailable::missing_risk_free;
    PerformanceUnavailable calmar_reason = PerformanceUnavailable::missing_time;
};

struct EquityDay
{
    std::chrono::sys_days day;
    Decimal equity; // at that day's settlement
};

// `days` ascend and follow a positive `deposit`; `marks` is every equity
// observation in order, settlements included, and decides the drawdown.
Performance performance(
    Decimal deposit,
    std::span<const EquityDay> days,
    std::span<const Decimal> marks,
    const PerformanceContext& context = {});

// Fixed day-end account evidence. Fees are cumulative since the initial deposit.
struct SettlementDay
{
    std::chrono::sys_days day;
    Decimal equity, fees;
};

struct MonthlyContribution
{
    std::chrono::sys_days first_day, last_day;
    std::size_t trading_days;
    Decimal opening_equity, closing_equity, gross, fees, net;
    // Net P&L divided by the original deposit: additive contributions to the
    // full result's return, not compounded monthly returns or shares of profit.
    double return_contribution;
};

// Group recorded trading dates by calendar month, including partial months.
// Missing months are not filled. All positions contribute through settlement
// equity, whether closed or still held; gross adds back fees, not slippage.
// A backtest has no deposits/withdrawals after its initial deposit.
std::vector<MonthlyContribution>
monthly_contributions(Decimal deposit, std::span<const SettlementDay> days);

// What the closed positions of a result say. A trade is one contract's
// position from flat to flat again, however many fills built and closed it.
// Gross profit is sells less buys times the multiplier. Net profit subtracts
// the fees the account charged on every fill of the round. A position still
// open at the end is no closed trade; its fees remain separate.
struct ClosedTrade
{
    std::size_t contract;
    Decimal gross, fees, net;
    std::size_t first_fill, last_fill;
};

struct OpenTrade
{
    std::size_t contract, first_fill;
    Decimal quantity, fees;
};

struct TradeContribution
{
    std::size_t contract, count;
    Decimal gross, fees, net;
};

struct Trades
{
    std::vector<ClosedTrade> closed;
    std::vector<OpenTrade> open;
    // Closed rounds only, in contract order; no unrealized P&L or open fees.
    std::vector<TradeContribution> by_contract;
    Decimal open_fees;
    // Net winners and losers; a net break-even round is neither.
    std::size_t winning, losing;
    // The average profit of the winning trades and the average loss, a negative
    // amount, of the losing ones; absent without any.
    std::optional<double> average_win, average_loss;
    // The average win over the size of the average loss; absent unless there
    // are both.
    std::optional<double> payoff;
};

struct TradedFill
{
    std::size_t contract; // An index into the multipliers.
    bool buy;
    Decimal quantity, price, fee;
};

// `fills` are in the order they happened. A fill that takes a position
// through flat to the other side is refused: closing and opening are
// separate orders.
Trades trades(std::span<const TradedFill> fills, std::span<const Decimal> multipliers);

enum class DrawdownUnavailable
{
    none,
    fewer_than_two_observations,
    non_positive_equity
};

struct DailyDrawdownPoint
{
    std::size_t peak_index;
    Decimal peak_equity, loss;
    std::optional<double> fraction;
};

struct DailyDrawdownEpisode
{
    std::size_t peak_index, trough_index, observed_end_index;
    std::optional<std::size_t> recovery_index;
};

struct DailyDrawdown
{
    std::vector<DailyDrawdownPoint> points;
    std::vector<DailyDrawdownEpisode> episodes;
    Decimal max_loss;
    std::size_t max_loss_index = 0;
    std::optional<double> max_fraction;
    std::optional<std::size_t> max_fraction_index;
    DrawdownUnavailable unavailable = DrawdownUnavailable::none;
};

// Original day-end observations only: no deposit, missing days or intraday
// peaks are inserted. All indices refer to this fixed observation sequence.
// A missing recovery is still underwater; durations use index differences,
// not an inferred calendar or exchange-session count.
DailyDrawdown daily_drawdown(std::span<const EquityDay>, std::stop_token = {});

} // namespace asterion
