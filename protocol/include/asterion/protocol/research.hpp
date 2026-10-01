#pragma once
#include <asterion/protocol/trading.hpp>
#include <asterion/v1/research.pb.h>
namespace asterion::protocol {
// Identity of the backtest semantics (matching, costs, close rules). Stored
// results from another identity are refused, never recomputed or migrated.
inline constexpr const char* backtest_engine_version = "asterion.backtest.sma-long-flat.v7";
// File/UI adapters. Cross-process transport remains typed Protobuf.
research::v1::BacktestInput encode_backtest(const Json& input);
research::v1::BacktestRequest encode_backtest_request(const Json& input);
Json decode_task(const research::v1::Task& task);
Json decode_task_result(const research::v1::TaskResponse& response, const std::string& id);
Json decode_backtest_result(const research::v1::BacktestResult& result);
Json decode_backtest(const research::v1::BacktestInput& input);
} // namespace asterion::protocol
