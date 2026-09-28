#pragma once
#include <asterion/domain/market.hpp>
#include <asterion/domain/settlement_calendar_port.hpp>
namespace asterion {
class CsvSettlementCalendar final : public SettlementCalendarPort {
public:
  CsvSettlementCalendar(Instrument instrument, std::string contents);
  PluginDescriptor descriptor() const override;
  void start() override;
  void stop() noexcept override { running_ = false; }
  std::vector<SettlementDay> read(std::stop_token stop = {}) const override;

private:
  Instrument instrument_;
  std::string contents_;
  bool running_ = false;
};
} // namespace asterion
