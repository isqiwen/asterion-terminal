#include "replay_schedule.hpp"
#include <stdexcept>
namespace asterion {
PaperReplaySchedule::PaperReplaySchedule(
    const std::vector<Instrument>& contracts, const std::vector<std::string>& event_days,
    const std::vector<std::vector<DaySettlement>>& settlements) {
  if (contracts.empty() || contracts.size() != settlements.size() || event_days.empty() ||
      settlements.front().empty())
    throw std::invalid_argument("scheduled paper replay requires contracts, events and days");
  for (std::size_t c = 0; c < contracts.size(); ++c) {
    if (contracts[c].asset_class != AssetClass::futures)
      throw std::invalid_argument("scheduled paper replay requires futures contracts");
    if (settlements[c].size() != settlements.front().size())
      throw std::invalid_argument("portfolio contracts must share the same trading days; "
                                  "narrow the trading days to their common range");
  }
  for (std::size_t d = 0; d < settlements.front().size(); ++d) {
    Day day{settlements.front()[d].trading_day, {}};
    if (d && day.trading_day <= days_.back().trading_day)
      throw std::invalid_argument("invalid ordered replay days or settlement price");
    for (std::size_t c = 0; c < contracts.size(); ++c) {
      const auto& settlement = settlements[c][d];
      if (settlement.trading_day != day.trading_day)
        throw std::invalid_argument("portfolio contracts must share the same trading days; "
                                    "narrow the trading days to their common range");
      if (settlement.settlement_price <= Decimal{} ||
          !settlement.settlement_price.multiple_of(contracts[c].price_increment))
        throw std::invalid_argument("invalid ordered replay days or settlement price");
      day.prices.push_back(settlement.settlement_price);
    }
    days_.push_back(std::move(day));
  }
  std::size_t day = 0;
  for (const auto& trading_day : event_days) {
    while (day < days_.size() && days_[day].trading_day < trading_day)
      ++day;
    if (day == days_.size() || days_[day].trading_day != trading_day)
      throw std::invalid_argument("bar trading day has no settlement: " + trading_day);
    if (!events_.empty() && events_.back().day != day && events_.back().day + 1 != day)
      throw std::invalid_argument("a settlement day between replay bars has no bars");
    events_.push_back({day, false});
  }
  if (events_.front().day != 0 || events_.back().day + 1 != days_.size())
    throw std::invalid_argument("each replay settlement day requires bars");
  for (std::size_t index = 0; index < events_.size(); ++index)
    events_[index].day_end =
        index + 1 == events_.size() || events_[index].day != events_[index + 1].day;
}
} // namespace asterion
