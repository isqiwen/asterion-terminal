#pragma once
#include <asterion/domain/market.hpp>
#include <optional>
#include <string>
#include <vector>
namespace asterion {
// Groups a portfolio replay into trading days. Contracts share the same
// trading days, each with its own settlement, unless marked sparse: a sparse
// contract (a month of a dominant series) trades only part of the days and has
// no settlement on the others. Every event's contract must have a settlement
// on the event's day, and every day must have events. Nothing is inferred.
class PaperReplaySchedule final {
public:
  struct Event {
    std::size_t day;
    bool day_end;
  };
  struct Day {
    std::string trading_day;
    // One settlement price per contract, in contract order; empty for a
    // sparse contract that does not trade that day.
    std::vector<std::optional<Decimal>> prices;
  };
  // event_days / event_contracts: trading day and contract of each replay
  // event, in event order. sparse: empty, or one flag per contract.
  PaperReplaySchedule(const std::vector<Instrument>& contracts,
                      const std::vector<std::string>& event_days,
                      const std::vector<std::size_t>& event_contracts,
                      const std::vector<std::vector<DaySettlement>>& settlements,
                      const std::vector<bool>& sparse = {});
  const Event& event(std::size_t index) const { return events_.at(index); }
  const Day& day(std::size_t index) const { return days_.at(index); }
  std::size_t size() const noexcept { return events_.size(); }
  std::size_t days() const noexcept { return days_.size(); }

private:
  std::vector<Day> days_;
  std::vector<Event> events_;
};
} // namespace asterion
