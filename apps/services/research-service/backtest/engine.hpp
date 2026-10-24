#pragma once
#include <asterion/protocol/backtest.hpp>
#include <functional>
#include <stop_token>

namespace asterion::risk_providers {

class Module;

} // namespace asterion::risk_providers

namespace asterion::backtest {

struct ModelReplayData;

void validate(const backtest::proto::BacktestInput& input);
backtest::proto::BacktestResult run(
    const backtest::proto::BacktestInput& input,
    std::stop_token stop = {},
    const std::function<void(std::size_t, std::size_t)>& progress = {},
    const risk_providers::Module* risk_module = nullptr,
    const ModelReplayData* model = nullptr);

} // namespace asterion::backtest
