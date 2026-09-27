#pragma once
#include <asterion/domain/trading_schedule.hpp>
#include <asterion/foundation/decimal.hpp>
#include <asterion/kernel/plugin.hpp>
#include <stop_token>
namespace asterion {
struct SettlementDay {
  TradingDaySchedule schedule;
  Decimal settlement_price;
  std::string schedule_source;
  std::string settlement_source;
};
// Explicit published observations, never an exchange-calendar inference engine.
class SettlementCalendarPort : public Plugin {
public:
  virtual std::vector<SettlementDay> read(std::stop_token stop = {}) const = 0;
};
} // namespace asterion
