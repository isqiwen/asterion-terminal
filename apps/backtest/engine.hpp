#pragma once
#include <asterion/protocol/research.hpp>
#include <functional>
#include <stop_token>
namespace asterion::backtest {
void validate(const research::v1::BacktestInput &input);
research::v1::BacktestResult
run(const research::v1::BacktestInput &input, std::stop_token stop = {},
    const std::function<void(std::size_t, std::size_t)> &progress = {});
Json result_json(const research::v1::BacktestResult &result);
} // namespace asterion::backtest
