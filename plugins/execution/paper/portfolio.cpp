#include "portfolio.hpp"
#include <asterion/protocol/data.hpp>
#include <algorithm>
namespace asterion {
std::vector<PaperExecution::Event>
replay_order(const std::vector<const std::vector<MarketBar>*>& contracts) {
  std::vector<PaperExecution::Event> events;
  for (std::size_t c = 0; c < contracts.size(); ++c)
    for (std::size_t b = 0; b < contracts[c]->size(); ++b)
      events.push_back({c, b});
  std::ranges::stable_sort(events, [&](const auto& left, const auto& right) {
    const auto& a = contracts[left.contract]->at(left.bar);
    const auto& b = contracts[right.contract]->at(right.bar);
    return a.timestamp_ns != b.timestamp_ns ? a.timestamp_ns < b.timestamp_ns
                                            : left.contract < right.contract;
  });
  return events;
}
PaperPortfolio paper_portfolio(const protocol::v1::PaperInput& input) {
  PaperPortfolio result;
  for (const auto& contract : input.contracts()) {
    result.contracts.push_back({protocol::contract_terms(contract),
                                protocol::dataset_bars(contract.dataset()),
                                protocol::cost_schedule(contract.cost_schedule())});
    result.days.push_back(protocol::dataset_days(contract.dataset()));
  }
  return result;
}
PaperReplaySchedule replay_schedule(const std::vector<Instrument>& contracts,
                                    const std::vector<std::vector<MarketBar>>& bars,
                                    const std::vector<std::vector<DaySettlement>>& days) {
  std::vector<const std::vector<MarketBar>*> series;
  for (const auto& item : bars)
    series.push_back(&item);
  std::vector<std::string> event_days;
  for (const auto& event : replay_order(series))
    event_days.push_back(bars[event.contract][event.bar].trading_day);
  return PaperReplaySchedule(contracts, event_days, days);
}
PaperReplaySchedule replay_schedule(const PaperPortfolio& portfolio) {
  std::vector<Instrument> contracts;
  std::vector<std::vector<MarketBar>> bars;
  for (const auto& contract : portfolio.contracts) {
    contracts.push_back(contract.terms.instrument);
    bars.push_back(contract.bars);
  }
  return replay_schedule(contracts, bars, portfolio.days);
}
} // namespace asterion
