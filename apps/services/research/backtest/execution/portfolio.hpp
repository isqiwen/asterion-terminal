#pragma once
#include <asterion/futures/paper_execution.hpp>
#include <asterion/futures/replay_schedule.hpp>
#include <asterion/protocol/trading.hpp>

namespace asterion {

// Contracts, bars and settlement days of a paper input, in contract order.
struct PaperPortfolio
{
    std::vector<ContractBars> contracts;
    std::vector<std::vector<DaySettlement>> days;
};

PaperPortfolio paper_portfolio(const protocol::proto::PaperInput& input);
// Day-end schedule of a portfolio in replay order.
// sparse: empty, or one flag per contract for months of a dominant series.
PaperReplaySchedule
replay_schedule(const PaperPortfolio& portfolio, const std::vector<bool>& sparse = {});

} // namespace asterion
