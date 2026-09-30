#pragma once
#include <asterion/domain/market.hpp>
#include <string>
#include <vector>
namespace asterion {
// Groups a bar replay into trading days. Every bar's day must have exactly one
// settlement and every settlement day must have bars. Nothing is inferred.
class PaperReplaySchedule final {
public:
  struct Event {
    std::size_t day;
    bool day_end;
  };
  PaperReplaySchedule(const Instrument&, const std::vector<MarketBar>&, std::vector<DaySettlement>);
  const Event& event(std::size_t index) const { return events_.at(index); }
  const DaySettlement& day(std::size_t index) const { return days_.at(index); }
  std::size_t size() const noexcept { return events_.size(); }
  std::size_t days() const noexcept { return days_.size(); }

private:
  std::vector<DaySettlement> days_;
  std::vector<Event> events_;
};
} // namespace asterion
