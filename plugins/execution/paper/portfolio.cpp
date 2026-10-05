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
PaperReplaySchedule replay_schedule(const PaperPortfolio& portfolio,
                                    const std::vector<bool>& sparse) {
  std::vector<Instrument> contracts;
  std::vector<const std::vector<MarketBar>*> series;
  for (const auto& contract : portfolio.contracts) {
    contracts.push_back(contract.terms.instrument);
    series.push_back(&contract.bars);
  }
  std::vector<std::string> event_days;
  std::vector<std::size_t> event_contracts;
  for (const auto& event : replay_order(series)) {
    event_days.push_back(portfolio.contracts[event.contract].bars[event.bar].trading_day);
    event_contracts.push_back(event.contract);
  }
  return PaperReplaySchedule(contracts, event_days, event_contracts, portfolio.days, sparse);
}
} // namespace asterion
