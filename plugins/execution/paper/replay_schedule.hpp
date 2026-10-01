#pragma once
#include <asterion/domain/market.hpp>
#include <string>
#include <vector>
namespace asterion {
// Groups a portfolio replay into trading days. Every contract has the same
// trading days, each with its own settlement; every event's day must have one
// and every settlement day must have events. Nothing is inferred.
class PaperReplaySchedule final {
public:
  struct Event {
    std::size_t day;
    bool day_end;
  };
  struct Day {
    std::string trading_day;
    // One settlement price per contract, in contract order.
    std::vector<Decimal> prices;
  };
  // event_days: the trading day of each replay event, in event order.
  PaperReplaySchedule(const std::vector<Instrument>& contracts,
                      const std::vector<std::string>& event_days,
                      const std::vector<std::vector<DaySettlement>>& settlements);
  const Event& event(std::size_t index) const { return events_.at(index); }
  const Day& day(std::size_t index) const { return days_.at(index); }
  std::size_t size() const noexcept { return events_.size(); }
  std::size_t days() const noexcept { return days_.size(); }

private:
  std::vector<Day> days_;
  std::vector<Event> events_;
};
} // namespace asterion
