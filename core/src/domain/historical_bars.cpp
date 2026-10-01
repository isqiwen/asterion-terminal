#include <asterion/domain/historical_bars.hpp>
#include <asterion/domain/daily_bars.hpp>
#include <stdexcept>
#include <charconv>
#include <iomanip>
#include <sstream>
namespace asterion {
void HistoricalBar::validate() const {
  if (!trading_day.empty())
    (void)parse_trading_date(trading_day);
  if (timestamp_ns < 0 || low > high || open < low || open > high || close < low || close > high ||
      volume < Decimal{} || amount < Decimal{} || open_interest < Decimal{})
    throw std::invalid_argument("invalid historical OHLCV bar");
}
std::chrono::year_month_day parse_trading_date(std::string_view value) {
  if (value.size() != 10 || value[4] != '-' || value[7] != '-')
    throw std::invalid_argument("invalid historical daily trading date");
  const auto number = [&](unsigned offset, unsigned size) {
    unsigned result = 0;
    const auto part = value.substr(offset, size);
    if (part.find_first_not_of("0123456789") != std::string_view::npos)
      throw std::invalid_argument("invalid historical daily trading date");
    const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), result);
    if (error != std::errc{} || end != part.data() + part.size())
      throw std::invalid_argument("invalid historical daily trading date");
    return result;
  };
  const auto day = std::chrono::year(int(number(0, 4))) / number(5, 2) / number(8, 2);
  (void)format_trading_date(day);
  return day;
}
std::string format_trading_date(std::chrono::year_month_day day) {
  if (!day.ok() || day.year() < std::chrono::year(1970) || day.year() > std::chrono::year(9999))
    throw std::invalid_argument("invalid historical daily trading date");
  std::ostringstream value;
  value << std::setfill('0') << std::setw(4) << int(day.year()) << '-' << std::setw(2)
        << unsigned(day.month()) << '-' << std::setw(2) << unsigned(day.day());
  return value.str();
}
void HistoricalDailyBar::validate() const {
  if (!trading_day.ok() || trading_day.year() < std::chrono::year(1970) ||
      trading_day.year() > std::chrono::year(9999))
    throw std::invalid_argument("invalid historical daily trading date");
  HistoricalBar{0, open, high, low, close, volume, amount, open_interest}.validate();
}
} // namespace asterion
