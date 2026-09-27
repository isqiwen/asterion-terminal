#pragma once
#include <asterion/domain/market.hpp>
#include <asterion/domain/settlement_calendar_port.hpp>
namespace asterion {
// Explicit historical simulator policy. Does not infer exchange hours or
// settlement prices, and does not make the data publication rules stricter.
class PaperReplaySchedule final {
public:
  struct Event {
    std::size_t day, session;
    bool session_end, day_end;
  };
  PaperReplaySchedule(const Instrument&, const std::vector<TradeTick>&, std::vector<SettlementDay>);
  const Event& event(std::size_t index) const { return events_.at(index); }
  const SettlementDay& day(std::size_t index) const { return days_.at(index); }
  std::size_t size() const noexcept { return events_.size(); }

private:
  std::vector<SettlementDay> days_;
  std::vector<Event> events_;
};
} // namespace asterion
