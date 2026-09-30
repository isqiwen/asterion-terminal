#pragma once
#include <asterion/domain/history_identity.hpp>
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
  HistoryIdentity instrument;
  std::chrono::year_month_day begin, end; // inclusive provider trading dates
  std::string source;
  std::string source_instrument = {};
};
class HistoricalDailyPort : public Plugin {
public:
  virtual HistorySemantics semantics() const = 0;
  virtual std::vector<HistoricalDailyBar> read(const HistoricalDailyRange&, std::stop_token) = 0;
};
} // namespace asterion
