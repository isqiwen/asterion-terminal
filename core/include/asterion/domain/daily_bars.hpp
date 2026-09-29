#pragma once
#include <asterion/domain/market.hpp>
#include <asterion/kernel/plugin.hpp>
#include <chrono>
#include <optional>
#include <stop_token>
#include <vector>
#include <string_view>
namespace asterion {
std::chrono::year_month_day parse_trading_date(std::string_view); // strict YYYY-MM-DD
std::string format_trading_date(std::chrono::year_month_day);
// The provider assigns a trading date. It is not an intraday timestamp and
// supplies neither exchange sessions nor an executable intrabar price path.
struct HistoricalDailyBar {
  std::chrono::year_month_day trading_day;
  Decimal open, high, low, close, volume, amount, open_interest;
  // Amount uses the instrument's quote-currency unit; quantities use native units.
  // Missing reference prices remain missing, including on first listing days.
  std::optional<Decimal> previous_close, previous_settlement, settlement;
  auto operator<=>(const HistoricalDailyBar&) const = default;
  void validate() const;
};
struct HistoricalDailyRange {
  InstrumentId instrument;
  std::chrono::year_month_day begin, end; // inclusive provider trading dates
};
class HistoricalDailyPort : public Plugin {
public:
  virtual std::vector<HistoricalDailyBar> read(const HistoricalDailyRange&, std::stop_token) = 0;
};
} // namespace asterion
