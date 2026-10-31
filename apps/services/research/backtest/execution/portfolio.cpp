#include "portfolio.hpp"
#include <asterion/protocol/data.hpp>

namespace asterion {

PaperPortfolio paper_portfolio(const protocol::proto::PaperInput& input)
{
    PaperPortfolio result;
    for (const auto& contract : input.contracts()) {
        result.contracts.push_back(
            {protocol::contract_terms(contract),
                protocol::dataset_bars(contract.dataset()),
                protocol::cost_schedule(contract.cost_schedule()),
                protocol::dataset_price_increment_changes(contract.dataset()),
                contract.slippage_ticks()});
        result.days.push_back(protocol::dataset_days(contract.dataset()));
    }
    return result;
}

PaperReplaySchedule
replay_schedule(const PaperPortfolio& portfolio, const std::vector<bool>& sparse)
{
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
