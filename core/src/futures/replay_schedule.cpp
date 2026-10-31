#include <asterion/futures/replay_schedule.hpp>
#include <set>
#include <stdexcept>

namespace asterion {

PaperReplaySchedule::PaperReplaySchedule(
    const std::vector<Instrument>& contracts,
    const std::vector<std::string>& event_days,
    const std::vector<std::size_t>& event_contracts,
    const std::vector<std::vector<DaySettlement>>& settlements,
    const std::vector<bool>& sparse)
{
    if (contracts.empty() || contracts.size() != settlements.size() || event_days.empty()
        || event_days.size() != event_contracts.size()
        || (!sparse.empty() && sparse.size() != contracts.size()))
        throw std::invalid_argument("scheduled paper replay requires contracts, events and days");
    std::set<std::string> all;
    for (std::size_t c = 0; c < contracts.size(); ++c) {
        if (settlements[c].empty())
            throw std::invalid_argument(
                "scheduled paper replay requires contracts, events and days");
        for (std::size_t d = 0; d < settlements[c].size(); ++d) {
            const auto& settlement = settlements[c][d];
            if ((d && settlement.trading_day <= settlements[c][d - 1].trading_day)
                || settlement.settlement_price <= Decimal{})
                throw std::invalid_argument("invalid ordered replay days or settlement price");
            all.insert(settlement.trading_day);
        }
    }
    for (const auto& trading_day : all)
        days_.push_back({trading_day, std::vector<std::optional<Decimal>>(contracts.size())});
    for (std::size_t c = 0; c < contracts.size(); ++c) {
        if ((sparse.empty() || !sparse[c]) && settlements[c].size() != days_.size())
            throw std::invalid_argument(
                "portfolio contracts must share the same trading days; " "narrow the trading days to their common range");
        std::size_t day = 0;
        for (const auto& settlement : settlements[c]) {
            while (days_[day].trading_day != settlement.trading_day)
                ++day;
            days_[day].prices[c] = settlement.settlement_price;
        }
    }
    std::size_t day = 0;
    for (std::size_t index = 0; index < event_days.size(); ++index) {
        const auto& trading_day = event_days[index];
        while (day < days_.size() && days_[day].trading_day < trading_day)
            ++day;
        if (day == days_.size() || days_[day].trading_day != trading_day
            || event_contracts[index] >= contracts.size()
            || !days_[day].prices[event_contracts[index]])
            throw std::invalid_argument("bar trading day has no settlement: " + trading_day);
        events_.push_back({day});
    }
}

} // namespace asterion
