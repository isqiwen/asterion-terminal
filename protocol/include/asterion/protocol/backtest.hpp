#pragma once
#include <asterion/protocol/trading.hpp>
#include <asterion/v1/backtest.pb.h>
namespace asterion::protocol {
// Identity of the backtest semantics (matching, costs, close rules). Stored
// results from another identity are refused, never recomputed or migrated.
inline constexpr const char* backtest_engine_version = "asterion.backtest.v13";
// Fewer development days say too little to choose a strategy by.
inline constexpr std::size_t backtest_development_days = 20;
backtest::v1::BacktestRequest encode_backtest_request(const Json& input);
// Every trading day of the input's contracts, ascending.
std::vector<std::string> backtest_trading_days(const backtest::v1::BacktestInput& input);
// Fixes where the holdout of a comparison begins: the first trading day on or
// after `from`. One strategy has no holdout and takes no date.
void set_backtest_holdout(backtest::v1::BacktestInput& input, const std::string& from);
// Progress units of a valid input: every bar once, and the bars before the
// holdout once more for each compared strategy.
std::size_t backtest_work_units(const backtest::v1::BacktestInput& input);
Json decode_backtest_result(const backtest::v1::BacktestResult& result);
Json decode_backtest(const backtest::v1::BacktestInput& input,
                     DatasetView view = DatasetView::full);
} // namespace asterion::protocol
