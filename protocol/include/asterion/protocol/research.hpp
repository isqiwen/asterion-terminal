#pragma once
#include <asterion/protocol/trading.hpp>
#include <asterion/v1/research.pb.h>
namespace asterion::protocol {
// File/UI adapters. Cross-process transport remains typed Protobuf.
research::v1::BacktestInput encode_backtest(const Json& input);
Json decode_task(const research::v1::Task& task);
Json decode_task_result(const research::v1::TaskResponse& response, const std::string& id);
Json decode_backtest_result(const research::v1::BacktestResult& result);
Json decode_backtest(const research::v1::BacktestInput& input);
std::string dataset_revision(const v1::PaperInput& input);
} // namespace asterion::protocol
