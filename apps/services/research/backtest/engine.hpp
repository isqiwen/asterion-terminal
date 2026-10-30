#pragma once
#include <asterion/protocol/backtest.hpp>
#include <functional>
#include <stop_token>

namespace asterion::backtest {

void validate(const backtest::proto::BacktestInput& input);
backtest::proto::BacktestResult run(
    const backtest::proto::BacktestInput& input,
    std::stop_token stop = {},
    const std::function<void(std::size_t, std::size_t)>& progress = {});

} // namespace asterion::backtest
