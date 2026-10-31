#include <asterion/futures/performance.hpp>
#include <asterion/base/error.hpp>
#include <cmath>
#include <gtest/gtest.h>
#include <vector>
using namespace asterion;
using namespace std::chrono;

namespace {

Decimal d(const char* value)
{
    return Decimal::parse(value);
}

// Consecutive calendar days from 2026-01-05 with the given settlement equity.
std::vector<EquityDay> record_of(const std::vector<const char*>& equity)
{
    std::vector<EquityDay> result;
    for (std::size_t i = 0; i < equity.size(); ++i)
        result.push_back(
            {sys_days{year{2026} / January / 5} + std::chrono::days{static_cast<int>(i)},
                d(equity[i])});
    return result;
}

} // namespace

TEST(Performance, MissingContextKeepsRawReturnAndDrawdownWithoutInventingAnnualAssumptions)
{
    const auto record = record_of({"101000", "100000", "102000"});
    const std::vector<Decimal>
        marks{d("100500"), d("101000"), d("99000"), d("100000"), d("102000")};
    const auto result = performance(d("100000"), record, marks);
    EXPECT_EQ(result.trading_days, 3U);
    EXPECT_DOUBLE_EQ(result.total_return, 0.02);
    // The fall from 101000 to 99000 inside the second day, not the day-end 100000.
    EXPECT_NEAR(result.max_drawdown, 2000.0 / 101000, 1e-12);
    EXPECT_DOUBLE_EQ(result.winning_days, 2.0 / 3);
    EXPECT_FALSE(
        result.annual_return || result.annual_volatility || result.sharpe || result.calmar);
}

TEST(Performance, AnnualReturnUsesOriginalSecondsWhileVolatilityUsesTheExplicitPeriodCount)
{
    // Up a quarter and back, ten times: daily returns of +25% and -20%.
    std::vector<const char*> equity;
    for (int i = 0; i < 20; ++i)
        equity.push_back(i % 2 ? "100000" : "125000");
    auto record = record_of(equity);
    std::vector<Decimal> marks;
    for (const auto& day : record)
        marks.push_back(day.equity);
    PerformanceContext context;
    context.begin_ns =
        duration_cast<nanoseconds>((record.front().day - days{1}).time_since_epoch()).count();
    context.end_ns = duration_cast<nanoseconds>(record.back().day.time_since_epoch()).count();
    context.annual_periods = 252;
    context.explicitly_zero_risk_free = true;
    context.opening_is_full_period =
        true; // This fixture explicitly supplies a prior settlement boundary.
    auto result = performance(d("100000"), record, marks, context);
    const double periods =
        252; // Explicit method, independent of this sample's covered calendar span.
    const double deviation = 0.225 * std::sqrt(20.0 / 19);
    EXPECT_DOUBLE_EQ(result.total_return, 0);
    // Twenty days are not annualized; volatility and Sharpe use the explicit A.
    EXPECT_FALSE(result.annual_return || result.calmar);
    EXPECT_EQ(result.annual_return_reason, PerformanceUnavailable::shorter_than_one_year);
    EXPECT_EQ(result.calmar_reason, PerformanceUnavailable::shorter_than_one_year);
    EXPECT_NEAR(*result.annual_volatility, deviation * std::sqrt(periods), 1e-9);
    EXPECT_NEAR(*result.sharpe, 0.025 / deviation * std::sqrt(periods), 1e-9);
    EXPECT_DOUBLE_EQ(result.max_drawdown, 0.2);
    EXPECT_DOUBLE_EQ(result.winning_days, 0.5);
    EXPECT_TRUE(result.short_calendar_span);
    // The same quarter gained over two calendar years is annualized.
    record.push_back({record.back().day + std::chrono::days{1}, d("125000")});
    marks.push_back(d("125000"));
    context.end_ns =
        *context.begin_ns + static_cast<std::int64_t>(performance_year_seconds * 2 * 1000000000.0L);
    result = performance(d("100000"), record, marks, context);
    ASSERT_TRUE(result.annual_return);
    EXPECT_NEAR(*result.annual_return, std::sqrt(1.25) - 1, 1e-9);
    EXPECT_FALSE(result.short_calendar_span);
    EXPECT_NEAR(*result.calmar, *result.annual_return / 0.2, 1e-9);
}

TEST(Performance, AFullYearOfDataTrimmedByHolidaysAtItsEndsIsAnnualized)
{
    // A daily backtest over 2025-10-27..2026-10-26 settles from the close of
    // 2025-10-28 to that of 2026-10-26: 363 days, short of 365.2425.
    const auto record = record_of({"100000", "110000"});
    const std::vector<Decimal> marks{d("100000"), d("110000")};
    PerformanceContext context;
    context.begin_ns = 0;
    context.end_ns = 363LL * 86400 * 1000000000;
    auto result = performance(d("100000"), record, marks, context);
    ASSERT_TRUE(result.annual_return);
    EXPECT_NEAR(*result.annual_return, std::pow(1.1, 365.2425 / 363) - 1, 1e-9);
    EXPECT_FALSE(result.short_calendar_span);
    // Two weeks short of a calendar year is still a partial year.
    context.end_ns = performance_annual_span_ns - 1;
    result = performance(d("100000"), record, marks, context);
    EXPECT_FALSE(result.annual_return);
    EXPECT_EQ(result.annual_return_reason, PerformanceUnavailable::shorter_than_one_year);
}

TEST(Performance, UndefinedFiguresStayAbsentAndInvalidRecordsAreRefused)
{
    std::vector<const char*> flat(25, "100000");
    auto record = record_of(flat);
    std::vector<Decimal> marks(25, d("100000"));
    const auto unknown = performance(d("100000"), record, marks);
    EXPECT_FALSE(
        unknown.annual_return || unknown.annual_volatility || unknown.sharpe || unknown.calmar);
    EXPECT_EQ(unknown.annual_volatility_reason, PerformanceUnavailable::missing_annual_periods);
    PerformanceContext context;
    context.begin_ns =
        duration_cast<nanoseconds>((record.front().day - days{1}).time_since_epoch()).count();
    context.end_ns = duration_cast<nanoseconds>(record.back().day.time_since_epoch()).count();
    context.annual_periods = 252;
    context.explicitly_zero_risk_free = true;
    context.opening_is_full_period = true;
    auto result = performance(d("100000"), record, marks, context);
    // Nothing varied and nothing fell: a volatility of zero, no ratio to it.
    EXPECT_DOUBLE_EQ(*result.annual_volatility, 0);
    EXPECT_FALSE(result.sharpe || result.calmar);
    EXPECT_EQ(result.sharpe_reason, PerformanceUnavailable::zero_deviation);
    EXPECT_EQ(result.calmar_reason, PerformanceUnavailable::shorter_than_one_year);
    EXPECT_DOUBLE_EQ(result.winning_days, 0);
    // An account that lost everything has no yearly rate.
    record[10].equity = d("0");
    marks[10] = d("0");
    result = performance(d("100000"), record, marks, context);
    EXPECT_EQ(result.annual_return_reason, PerformanceUnavailable::non_positive_equity);
    EXPECT_DOUBLE_EQ(result.max_drawdown, 1);
    EXPECT_FALSE(result.annual_return || result.sharpe);
    EXPECT_THROW(performance(d("0"), record, marks), std::invalid_argument);
    EXPECT_THROW(performance(d("100000"), {}, marks), std::invalid_argument);
    std::swap(record[3].day, record[4].day);
    EXPECT_THROW(performance(d("100000"), record, marks), std::invalid_argument);
}

TEST(Performance, OpeningStubIsExcludedAndAnExplicitZeroRiskFreeIsDistinctFromMissing)
{
    const auto record = record_of({"11000", "12100", "10890"});
    const std::vector<Decimal> marks{d("11000"), d("12100"), d("10890")};
    PerformanceContext context;
    context.begin_ns =
        duration_cast<nanoseconds>(record.front().day.time_since_epoch() + hours{9}).count();
    context.end_ns =
        duration_cast<nanoseconds>(record.back().day.time_since_epoch() + hours{15}).count();
    context.annual_periods = 252;
    const auto missing = performance(d("10000"), record, marks, context);
    ASSERT_TRUE(missing.annual_volatility);
    EXPECT_EQ(missing.return_periods, 2U);
    EXPECT_NEAR(*missing.annual_volatility, std::sqrt(0.02) * std::sqrt(252.0), 1e-12);
    EXPECT_FALSE(missing.sharpe);
    EXPECT_EQ(missing.sharpe_reason, PerformanceUnavailable::missing_risk_free);
    EXPECT_DOUBLE_EQ(missing.total_return, 0.089);
    context.explicitly_zero_risk_free = true;
    const auto zero = performance(d("10000"), record, marks, context);
    ASSERT_TRUE(zero.sharpe);
    EXPECT_NEAR(*zero.sharpe, 0.0, 1e-12);
    EXPECT_EQ(zero.sharpe_reason, PerformanceUnavailable::none);
    context.opening_is_full_period = true;
    context.begin_ns =
        duration_cast<nanoseconds>((record.front().day - days{1}).time_since_epoch() + hours{15})
            .count();
    const auto full = performance(d("10000"), record, marks, context);
    EXPECT_EQ(full.return_periods, 3U);
    ASSERT_TRUE(full.sharpe);
    EXPECT_NEAR(*full.sharpe, (1.0 / 30) / std::sqrt(1.0 / 75) * std::sqrt(252.0), 1e-12);
    context.end_ns = context.begin_ns;
    const auto no_time = performance(d("10000"), record, marks, context);
    EXPECT_FALSE(no_time.annual_return);
    EXPECT_EQ(no_time.annual_return_reason, PerformanceUnavailable::non_positive_duration);
    EXPECT_TRUE(no_time.annual_volatility);
}

TEST(Performance, InadequateCompletePeriodsAndOverflowKeepIndependentUnavailableReasons)
{
    const auto record = record_of({"11000"});
    const std::vector<Decimal> marks{d("11000")};
    PerformanceContext context;
    context.begin_ns = 0;
    context.end_ns = 1;
    context.annual_periods = 252;
    context.explicitly_zero_risk_free = true;
    const auto result = performance(d("10000"), record, marks, context);
    EXPECT_FALSE(result.annual_return || result.annual_volatility || result.sharpe);
    EXPECT_EQ(result.annual_return_reason, PerformanceUnavailable::shorter_than_one_year);
    EXPECT_EQ(result.sharpe_reason, PerformanceUnavailable::insufficient_periods);
    EXPECT_EQ(result.return_periods, 0U);
    const auto small = record_of({"0.00000001"});
    const std::vector<Decimal> small_marks{d("0.00000001")};
    context.end_ns = static_cast<std::int64_t>(performance_year_seconds * 10 * 1000000000.0L);
    const auto large_decline = performance(d("100000000"), small, small_marks, context);
    ASSERT_TRUE(large_decline.annual_return);
    EXPECT_NEAR(*large_decline.annual_return, std::pow(1e-16, 0.1) - 1, 1e-12);
    context.annual_periods = 0;
    EXPECT_THROW(performance(d("10000"), record, marks, context), std::invalid_argument);
}

TEST(MonthlyContributions, PartialMonthsAndYearBoundariesReconcileExactEquityAndFees)
{
    const auto deposit = d("90000000");
    const std::vector<SettlementDay> days{
        {sys_days{2025y / December / 30}, d("90000100.00000001"), d("1.00000001")},
        {sys_days{2025y / December / 31}, d("90000080"), d("3.00000001")},
        {sys_days{2026y / January / 5}, d("89999999.99999999"), d("4.00000002")},
        // No February evidence: no synthetic February row.
        {sys_days{2026y / March / 2}, d("90000020"), d("4.00000002")},
    };
    const auto months = monthly_contributions(deposit, days);
    ASSERT_EQ(months.size(), 3U);
    // Compare day offsets: libc++'s chrono printer requires macOS 13.3.
    EXPECT_EQ((months[0].first_day - days[0].day).count(), 0);
    EXPECT_EQ((months[0].last_day - days[1].day).count(), 0);
    EXPECT_EQ(months[0].trading_days, 2U);
    EXPECT_EQ(months[0].opening_equity, deposit);
    EXPECT_EQ(months[0].closing_equity, d("90000080"));
    EXPECT_EQ(months[0].net, d("80"));
    EXPECT_EQ(months[0].fees, d("3.00000001"));
    EXPECT_EQ(months[0].gross, d("83.00000001"));
    EXPECT_EQ((months[1].first_day - days[2].day).count(), 0);
    EXPECT_EQ((months[1].last_day - days[2].day).count(), 0);
    EXPECT_EQ(months[1].trading_days, 1U);
    EXPECT_EQ(months[1].opening_equity, months[0].closing_equity);
    EXPECT_EQ(months[1].net, d("-80.00000001"));
    EXPECT_EQ(months[1].fees, d("1.00000001"));
    EXPECT_EQ(months[1].gross, d("-79"));
    EXPECT_EQ(months[2].net, d("20.00000001"));
    EXPECT_EQ(months[2].fees, Decimal{});
    Decimal net, fees, gross;
    double contribution = 0;
    for (const auto& month : months) {
        net = net + month.net;
        fees = fees + month.fees;
        gross = gross + month.gross;
        contribution += month.return_contribution;
    }
    EXPECT_EQ(net, days.back().equity - deposit);
    EXPECT_EQ(fees, days.back().fees);
    EXPECT_EQ(gross, net + fees);
    EXPECT_NEAR(contribution, 20.0 / 90000000, 1e-20);
}

TEST(MonthlyContributions, NonpositiveEquityDoesNotMakeDepositBasedContributionsUndefined)
{
    const std::vector<SettlementDay> days{
        {sys_days{2026y / January / 30}, d("-10"), d("2")},
        {sys_days{2026y / February / 2}, d("0"), d("3")},
        {sys_days{2026y / March / 2}, d("20"), d("4")},
    };
    const auto months = monthly_contributions(d("100"), days);
    ASSERT_EQ(months.size(), 3U);
    EXPECT_DOUBLE_EQ(months[0].return_contribution, -1.1);
    EXPECT_DOUBLE_EQ(months[1].return_contribution, 0.1);
    EXPECT_DOUBLE_EQ(months[2].return_contribution, 0.2);
    EXPECT_EQ(months[1].opening_equity, d("-10"));
    EXPECT_EQ(months[2].opening_equity, d("0"));
}

TEST(MonthlyContributions, InvalidSettlementEvidenceIsRejected)
{
    std::vector<SettlementDay> days{
        {sys_days{2026y / January / 5}, d("100"), d("2")},
        {sys_days{2026y / January / 6}, d("100"), d("1")},
    };
    EXPECT_THROW(monthly_contributions(d("100"), days), std::invalid_argument);
    days[1].fees = d("2");
    days[1].day = days[0].day;
    EXPECT_THROW(monthly_contributions(d("100"), days), std::invalid_argument);
    EXPECT_THROW(monthly_contributions(d("100"), {}), std::invalid_argument);
    EXPECT_THROW(monthly_contributions(d("0"), std::span(days).first(1)), std::invalid_argument);
}

TEST(Trades, ATradeIsOneContractsPositionFromFlatToFlatAgain)
{
    const std::vector<Decimal> multipliers{d("10"), d("5")};
    const std::vector<TradedFill> fills{
        // The first contract: two lots bought and sold in two fills, a profit
        // of (110 + 120 - 200) x 10.
        {0, true, d("2"), d("100")},
        // The second contract opens meanwhile and never closes: no trade.
        {1, true, d("1"), d("50")},
        {0, false, d("1"), d("110")},
        {0, false, d("1"), d("120")},
        // Short one lot at 120 and covered at 125: a loss of 5 x 10.
        {0, false, d("1"), d("120")},
        {0, true, d("1"), d("125")},
        // In and out at one price: a trade that is neither a win nor a loss.
        {0, true, d("3"), d("130")},
        {0, false, d("3"), d("130")},
    };
    const auto result = trades(fills, multipliers);
    EXPECT_EQ(result.closed.size(), 3U);
    EXPECT_EQ(result.closed[0].first_fill, 0U);
    EXPECT_EQ(result.closed[0].last_fill, 3U);
    EXPECT_EQ(result.closed[1].first_fill, 4U);
    EXPECT_EQ(result.closed[1].last_fill, 5U);
    ASSERT_EQ(result.open.size(), 1U);
    EXPECT_EQ(result.open[0].contract, 1U);
    EXPECT_EQ(result.open[0].first_fill, 1U);
    EXPECT_EQ(result.winning, 1U);
    EXPECT_EQ(result.losing, 1U);
    EXPECT_DOUBLE_EQ(*result.average_win, 300);
    EXPECT_DOUBLE_EQ(*result.average_loss, -50);
    EXPECT_DOUBLE_EQ(*result.payoff, 6);
}

TEST(Trades, ActualFillFeesDetermineNetWinsAndStayWithUnclosedPositions)
{
    const std::vector<Decimal> multipliers{d("10"), d("5")};
    const std::vector<TradedFill> fills{
        {0, true, d("2"), d("100"), d("4")},
        // A different contract remains open. None of its fees belongs to a closed round.
        {1, true, d("1"), d("50"), d("7")},
        {0, false, d("1"), d("100.2"), d("3")},
        {0, false, d("1"), d("100.4"), d("3")},
        // Another short round makes 20 gross and 15 net.
        {0, false, d("1"), d("102"), d("2")},
        {0, true, d("1"), d("100"), d("3")},
        {0, true, d("1"), d("100"), d("2")},
    };
    const auto result = trades(fills, multipliers);
    ASSERT_EQ(result.closed.size(), 2U);
    EXPECT_EQ(result.closed[0].gross, d("6"));
    EXPECT_EQ(result.closed[0].fees, d("10"));
    EXPECT_EQ(result.closed[0].net, d("-4"));
    EXPECT_EQ(result.closed[1].net, d("15"));
    EXPECT_EQ(result.winning, 1U);
    EXPECT_EQ(result.losing, 1U);
    EXPECT_DOUBLE_EQ(*result.average_win, 15);
    EXPECT_DOUBLE_EQ(*result.average_loss, -4);
    EXPECT_DOUBLE_EQ(*result.payoff, 3.75);
    EXPECT_EQ(result.open_fees, d("9"));
    EXPECT_EQ(result.closed[0].fees + result.closed[1].fees + result.open_fees, d("24"));
}

TEST(Trades, ContractContributionsReconcileClosedRoundsWithoutOpenFees)
{
    const std::vector<Decimal> multipliers{d("10"), d("5"), d("1")};
    const std::vector<TradedFill> fills{
        {0, true, d("1"), d("100"), d("0.00000001")},
        {1, false, d("1"), d("50"), d("2")},
        {0, false, d("1"), d("101"), d("1")},
        {1, true, d("1"), d("51"), d("3")},
        {0, false, d("1"), d("102"), d("1")},
        {0, true, d("1"), d("100"), d("1")},
        {0, true, d("1"), d("100"), d("7")},
        {2, true, d("1"), d("10"), d("8")},
    };
    const auto result = trades(fills, multipliers);
    ASSERT_EQ(result.by_contract.size(), 2U);
    const auto& first = result.by_contract[0];
    EXPECT_EQ(first.contract, 0U);
    EXPECT_EQ(first.count, 2U);
    EXPECT_EQ(first.gross, d("30"));
    EXPECT_EQ(first.fees, d("3.00000001"));
    EXPECT_EQ(first.net, d("26.99999999"));
    const auto& second = result.by_contract[1];
    EXPECT_EQ(second.contract, 1U);
    EXPECT_EQ(second.count, 1U);
    EXPECT_EQ(second.gross, d("-5"));
    EXPECT_EQ(second.fees, d("5"));
    EXPECT_EQ(second.net, d("-10"));
    Decimal gross, fees, net;
    for (const auto& trade : result.closed) {
        gross = gross + trade.gross;
        fees = fees + trade.fees;
        net = net + trade.net;
    }
    EXPECT_EQ(first.gross + second.gross, gross);
    EXPECT_EQ(first.fees + second.fees, fees);
    EXPECT_EQ(first.net + second.net, net);
    EXPECT_EQ(result.open_fees, d("15"));
}

TEST(Trades, AveragesNeedTradesOfTheirKindAndAFillNeverCrossesFlat)
{
    const std::vector<Decimal> multipliers{d("10")};
    // Nothing closed: no trade and nothing to average.
    const std::vector<TradedFill> open{{0, true, d("1"), d("100")}};
    auto result = trades(open, multipliers);
    EXPECT_EQ(result.closed.size(), 0U);
    EXPECT_TRUE(result.by_contract.empty());
    EXPECT_FALSE(result.average_win || result.average_loss || result.payoff);
    // Two wins, of 10 and of 30: an average, and no ratio without a loss.
    const std::vector<TradedFill> wins{
        {0, true, d("1"), d("100")},
        {0, false, d("1"), d("101")},
        {0, false, d("2"), d("103")},
        {0, true, d("2"), d("101.5")}
    };
    result = trades(wins, multipliers);
    EXPECT_EQ(result.winning, 2U);
    EXPECT_DOUBLE_EQ(*result.average_win, 20);
    EXPECT_FALSE(result.average_loss || result.payoff);
    // Selling two while holding one would close and open in one fill.
    const std::vector<TradedFill> crossing{
        {0, true, d("1"), d("100")},
        {0, false, d("2"), d("101")}
    };
    EXPECT_THROW(trades(crossing, multipliers), std::invalid_argument);
}

TEST(DailyDrawdown, OriginalFiveObservationsKeepThePeakTroughAndFirstRecovery)
{
    const auto rows = record_of({"100", "110", "99", "105", "112"});
    const auto result = daily_drawdown(rows);
    ASSERT_EQ(result.points.size(), rows.size());
    ASSERT_EQ(result.episodes.size(), 1U);
    EXPECT_DOUBLE_EQ(*result.max_fraction, 0.1);
    EXPECT_EQ(*result.max_fraction_index, 2U);
    EXPECT_EQ(result.max_loss, d("11"));
    EXPECT_EQ(result.episodes[0].peak_index, 1U);
    EXPECT_EQ(result.episodes[0].trough_index, 2U);
    EXPECT_EQ(result.episodes[0].recovery_index, 4U);
    EXPECT_EQ(result.episodes[0].observed_end_index, 4U);
    EXPECT_EQ(result.points[2].peak_equity, d("110"));
    EXPECT_EQ(result.points[4].peak_index, 4U);
    EXPECT_EQ(rows[2].equity, d("99"));
}

TEST(DailyDrawdown, AnUnrecoveredPathHasNoInventedRecoveryOrZeroDuration)
{
    auto rows = record_of({"100", "110", "99", "105"});
    rows[3].day += std::chrono::days{5};
    const auto result = daily_drawdown(rows);
    ASSERT_EQ(result.episodes.size(), 1U);
    const auto& episode = result.episodes[0];
    EXPECT_FALSE(episode.recovery_index);
    EXPECT_EQ(episode.observed_end_index - episode.peak_index, 2U);
    EXPECT_DOUBLE_EQ(*result.max_fraction, 0.1);
    EXPECT_NEAR(*result.points[3].fraction, 5.0 / 110, 1e-12);
}

TEST(DailyDrawdown, EqualPeaksStartANewEpisodeAndEqualTroughsKeepTheirFirstObservation)
{
    const auto result = daily_drawdown(record_of({"100", "110", "105", "110", "99", "99", "110"}));
    ASSERT_EQ(result.episodes.size(), 2U);
    EXPECT_EQ(result.episodes[0].recovery_index, 3U);
    EXPECT_EQ(result.episodes[1].peak_index, 3U);
    EXPECT_EQ(result.episodes[1].trough_index, 4U);
    EXPECT_EQ(result.episodes[1].recovery_index, 6U);
    EXPECT_EQ(*result.max_fraction_index, 4U);
}

TEST(DailyDrawdown, NonpositiveEquityRetainsMoneyAndMakesIncompletePercentCoverageExplicit)
{
    const auto result = daily_drawdown(record_of({"100", "-10", "0", "110", "99"}));
    EXPECT_EQ(result.points[1].loss, d("110"));
    EXPECT_FALSE(result.points[1].fraction);
    EXPECT_FALSE(result.points[2].fraction);
    EXPECT_DOUBLE_EQ(*result.points[4].fraction, 0.1);
    EXPECT_EQ(result.max_loss, d("110"));
    EXPECT_FALSE(result.max_fraction);
    EXPECT_FALSE(result.max_fraction_index);
    EXPECT_EQ(result.unavailable, DrawdownUnavailable::non_positive_equity);
    const auto single = daily_drawdown(record_of({"100"}));
    EXPECT_FALSE(single.max_fraction);
    EXPECT_EQ(single.unavailable, DrawdownUnavailable::fewer_than_two_observations);
}

TEST(DailyDrawdown, InvalidOrderAndCancellationCannotProduceAPartialPublishedPath)
{
    auto rows = record_of({"100", "110"});
    rows[1].day = rows[0].day;
    EXPECT_THROW(daily_drawdown(rows), std::invalid_argument);
    EXPECT_THROW(daily_drawdown({}), std::invalid_argument);
    std::stop_source cancellation;
    cancellation.request_stop();
    EXPECT_THROW(daily_drawdown(record_of({"100", "110"}), cancellation.get_token()), Error);
}
