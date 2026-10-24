#pragma once
#include <asterion/protocol/trading.hpp>
#include <asterion/backtest.pb.h>

namespace asterion::protocol {

// Fewer development days say too little to choose a strategy by.
inline constexpr std::size_t backtest_development_days = 20;
void validate_performance_assumptions(const backtest::proto::PerformanceAssumptions&);
backtest::proto::PerformanceAssumptions encode_performance_assumptions(const Json&);
Json decode_performance_assumptions(const backtest::proto::PerformanceAssumptions&);
void require_backtest_scoring_assumptions(
    std::size_t strategies,
    const backtest::proto::PerformanceAssumptions&);
void apply_historical_margins(
    backtest::proto::BacktestInput&,
    const data::proto::HistoricalMargins&);
backtest::proto::BacktestRequest encode_backtest_request(const Json& input);
backtest::proto::BacktestInput
backtest_slippage_variant(const backtest::proto::BacktestInput&, unsigned ticks);
backtest::proto::BacktestInput
backtest_fee_variant(const backtest::proto::BacktestInput&, Decimal multiplier);
// Every trading day of the input's contracts, ascending.
std::vector<std::string> backtest_trading_days(const backtest::proto::BacktestInput& input);
// The fixed model in force after this day's settlement, including an explicit
// margin event when present. Input validation owns event/schedule constraints.
FuturesCostVersion backtest_settlement_costs(
    const backtest::proto::BacktestInput&,
    std::size_t contract,
    const std::string& day);
// Takes the strategies of a request into an input that already has its
// contracts, with how they are compared: a holdout that begins on the first
// trading day on or after the request's date, or a rolling comparison. One
// strategy takes neither.
void set_backtest_strategies(
    backtest::proto::BacktestInput& input,
    const backtest::proto::BacktestRequest& request);

// The rounds of a valid rolling comparison, as trading days by their place in
// backtest_trading_days: a round's validation days are [first, end) and its
// training days the `training_days` before `first`.
struct BacktestRound
{
    std::size_t first, end;
};

std::vector<BacktestRound> backtest_folds(const backtest::proto::BacktestInput& input);
// Progress units of a valid input: every bar once, and for each compared
// strategy the bars before the holdout, or every bar of a rolling comparison,
// once more.
std::size_t backtest_work_units(const backtest::proto::BacktestInput& input);
// Verify stored result evidence against its fixed input without running the engine.
void validate_backtest_result(
    const backtest::proto::BacktestInput&,
    const backtest::proto::BacktestResult&);
Json decode_backtest_result(const backtest::proto::BacktestResult& result);
Json decode_backtest(
    const backtest::proto::BacktestInput& input,
    DatasetView view = DatasetView::full);

} // namespace asterion::protocol
