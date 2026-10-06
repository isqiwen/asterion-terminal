#pragma once
#include <asterion/protocol/trading.hpp>
#include <asterion/v1/backtest.pb.h>
namespace asterion::protocol {
// Identity of the backtest semantics (matching, costs, close rules). Stored
// results from another identity are refused, never recomputed or migrated.
inline constexpr const char* backtest_engine_version = "asterion.backtest.sma-long-flat.v10";
backtest::v1::BacktestRequest encode_backtest_request(const Json& input);
Json decode_backtest_result(const backtest::v1::BacktestResult& result);
Json decode_backtest(const backtest::v1::BacktestInput& input,
                     DatasetView view = DatasetView::full);
} // namespace asterion::protocol
