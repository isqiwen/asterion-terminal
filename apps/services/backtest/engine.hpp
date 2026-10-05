#pragma once
#include <asterion/protocol/backtest.hpp>
#include <functional>
#include <stop_token>
namespace asterion::risk_providers {
class Module;
}
namespace asterion::backtest {
void validate(const backtest::v1::BacktestInput& input);
backtest::v1::BacktestResult run(const backtest::v1::BacktestInput& input,
                                 std::stop_token stop = {},
                                 const std::function<void(std::size_t, std::size_t)>& progress = {},
                                 const risk_providers::Module* risk_module = nullptr);
} // namespace asterion::backtest
