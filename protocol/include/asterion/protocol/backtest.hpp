#pragma once
#include <asterion/protocol/trading.hpp>
#include <asterion/v1/backtest.pb.h>
namespace asterion::protocol {
// Identity of the backtest semantics (matching, costs, close rules). Stored
// results from another identity are refused, never recomputed or migrated.
inline constexpr const char* backtest_engine_version = "asterion.backtest.v14";
// Fewer development days say too little to choose a strategy by.
inline constexpr std::size_t backtest_development_days = 20;
backtest::v1::BacktestRequest encode_backtest_request(const Json& input);
// Every trading day of the input's contracts, ascending.
std::vector<std::string> backtest_trading_days(const backtest::v1::BacktestInput& input);
// Takes the strategies of a request into an input that already has its
// contracts, with how they are compared: a holdout that begins on the first
// trading day on or after the request's date, or a rolling comparison. One
// strategy takes neither.
void set_backtest_strategies(backtest::v1::BacktestInput& input,
                             const backtest::v1::BacktestRequest& request);
// The rounds of a valid rolling comparison, as trading days by their place in
// backtest_trading_days: a round's validation days are [first, end) and its
// training days the `training_days` before `first`.
struct BacktestRound {
  std::size_t first, end;
};
std::vector<BacktestRound> backtest_folds(const backtest::v1::BacktestInput& input);
// Progress units of a valid input: every bar once, and for each compared
// strategy the bars before the holdout, or every bar of a rolling comparison,
// once more.
std::size_t backtest_work_units(const backtest::v1::BacktestInput& input);
Json decode_backtest_result(const backtest::v1::BacktestResult& result);
Json decode_backtest(const backtest::v1::BacktestInput& input,
                     DatasetView view = DatasetView::full);
} // namespace asterion::protocol
