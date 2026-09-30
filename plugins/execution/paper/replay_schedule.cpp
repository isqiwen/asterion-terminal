#include "replay_schedule.hpp"
#include <stdexcept>
namespace asterion {
PaperReplaySchedule::PaperReplaySchedule(const Instrument& instrument,
                                         const std::vector<MarketBar>& bars,
                                         std::vector<DaySettlement> days)
    : days_(std::move(days)) {
  if (instrument.asset_class != AssetClass::futures || days_.empty() || bars.empty())
    throw std::invalid_argument("scheduled paper replay requires futures bars and trading days");
  for (std::size_t index = 0; index < days_.size(); ++index) {
    const auto& day = days_[index];
    if ((index && day.trading_day <= days_[index - 1].trading_day) ||
        day.settlement_price <= Decimal{} ||
        !day.settlement_price.multiple_of(instrument.price_increment))
      throw std::invalid_argument("invalid ordered replay days or settlement price");
  }
  std::size_t day = 0;
  for (const auto& bar : bars) {
    while (day < days_.size() && days_[day].trading_day < bar.trading_day)
      ++day;
    if (day == days_.size() || days_[day].trading_day != bar.trading_day)
      throw std::invalid_argument("bar trading day has no settlement: " + bar.trading_day);
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
