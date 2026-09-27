#include "replay_schedule.hpp"
#include <algorithm>
#include <stdexcept>
namespace asterion {
PaperReplaySchedule::PaperReplaySchedule(const Instrument& instrument,
                                         const std::vector<TradeTick>& ticks,
                                         std::vector<SettlementDay> days)
    : days_(std::move(days)) {
  if (instrument.asset_class != AssetClass::futures || days_.empty() || days_.size() > 64 ||
      ticks.empty() || ticks.size() > 10000)
    throw std::invalid_argument("scheduled paper replay requires futures, "
                                "1..64 days and 1..10000 events");
  std::string previous_day;
  std::int64_t previous_end = 0;
  for (const auto& day : days_) {
    const auto& sessions = day.schedule.sessions();
    if (day.schedule.trading_day() <= previous_day || sessions.size() > 16 ||
        sessions.front().begin_ns < previous_end || day.settlement_price <= Decimal{} ||
        !day.settlement_price.multiple_of(instrument.price_increment))
      throw std::invalid_argument("invalid ordered paper replay days or settlement price");
    previous_day = day.schedule.trading_day();
    previous_end = sessions.back().end_ns;
  }
  std::vector<std::size_t> counts(days_.size());
  std::size_t day = 0, session = 0;
  std::int64_t previous_tick = -1;
  for (const auto& tick : ticks) {
    tick.validate(instrument);
    if (tick.timestamp_ns < previous_tick || tick.price <= Decimal{})
      throw std::invalid_argument("invalid paper replay event order or price");
    previous_tick = tick.timestamp_ns;
    while (day < days_.size() &&
           tick.timestamp_ns >= days_[day].schedule.sessions()[session].end_ns) {
      if (++session == days_[day].schedule.sessions().size()) {
        ++day;
        session = 0;
      }
    }
    if (day == days_.size() || tick.timestamp_ns < days_[day].schedule.sessions()[session].begin_ns)
      throw std::invalid_argument("paper replay event is outside declared trading sessions");
    events_.push_back({day, session, false, false});
    ++counts[day];
  }
  if (std::find(counts.begin(), counts.end(), 0U) != counts.end())
    throw std::invalid_argument("each declared replay day requires events");
  for (std::size_t index = 0; index < events_.size(); ++index) {
    auto& event = events_[index];
    event.day_end = index + 1 == events_.size() || event.day != events_[index + 1].day;
    event.session_end = event.day_end || event.session != events_[index + 1].session;
  }
}
} // namespace asterion
