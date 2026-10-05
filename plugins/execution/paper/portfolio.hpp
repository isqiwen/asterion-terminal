#pragma once
#include "paper_execution.hpp"
#include "replay_schedule.hpp"
#include <asterion/protocol/trading.hpp>
namespace asterion {
// The single event order of a portfolio replay: bar time, then contract order.
// PaperExecution, its schedule and strategy replays all consume this order.
std::vector<PaperExecution::Event>
replay_order(const std::vector<const std::vector<MarketBar>*>& contracts);
// Contracts, bars and settlement days of a paper input, in contract order.
struct PaperPortfolio {
  std::vector<ContractBars> contracts;
  std::vector<std::vector<DaySettlement>> days;
};
PaperPortfolio paper_portfolio(const protocol::v1::PaperInput& input);
// Day-end schedule of a portfolio in replay order.
// sparse: empty, or one flag per contract for months of a dominant series.
PaperReplaySchedule replay_schedule(const PaperPortfolio& portfolio,
                                    const std::vector<bool>& sparse = {});
} // namespace asterion
