#include <asterion/domain/history_identity.hpp>
#include <asterion/domain/daily_bars.hpp>
#include <algorithm>
#include <array>
#include <charconv>
#include <iomanip>
#include <sstream>
#include <stdexcept>
namespace asterion {
void HistoryIdentity::validate() const {
  constexpr std::array<std::string_view, 6> venues{"SHFE", "DCE", "CZCE", "CFFEX", "INE", "GFEX"};
  if (std::ranges::find(venues, venue) == venues.end() || product.empty() || product.size() > 8 ||
      !std::ranges::all_of(product, [](char c) { return c >= 'a' && c <= 'z'; }) ||
      delivery_month.size() != 7)
    throw std::invalid_argument("invalid historical contract identity");
  (void)parse_trading_date(delivery_month + "-01");
}
std::string HistoryIdentity::key() const {
  validate();
  return venue + "/" + product + "/" + delivery_month;
}
HistoryIdentity HistoryIdentity::parse(std::string_view value) {
  auto first = value.find('/'), last = value.rfind('/');
  if (first == std::string_view::npos || first == last)
    throw std::invalid_argument("invalid historical contract identity");
  HistoryIdentity result{std::string(value.substr(0, first)),
                         std::string(value.substr(first + 1, last - first - 1)),
                         std::string(value.substr(last + 1))};
  result.validate();
  return result;
}
InstrumentId HistoryIdentity::exchange_id() const {
  validate();
  auto symbol = product;
  if (venue == "CZCE" || venue == "CFFEX")
    std::ranges::transform(symbol, symbol.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  symbol += venue == "CZCE" ? delivery_month.substr(3, 1) + delivery_month.substr(5)
                            : delivery_month.substr(2, 2) + delivery_month.substr(5);
  return {venue, symbol};
}
void validate_history_source(std::string_view value) {
  if (value.empty() || value == "." || value == ".." || value.size() > 64 ||
      !std::ranges::all_of(value, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
      }))
    throw std::invalid_argument("invalid historical data source");
}
void HistorySemantics::validate() const {
  validate_history_source(source);
  validate_history_source(normalization);
  if (timezone != "Asia/Shanghai" ||
      (timestamp_semantics != "provider_label" && timestamp_semantics != "bar_end" &&
       timestamp_semantics != "trading_day") ||
      amount_unit != "quote_currency" || quantity_unit != "contracts")
    throw std::invalid_argument("unsupported historical data semantics");
}
std::int64_t parse_shanghai_time(std::string_view text) {
  using namespace std::chrono;
  if (text.size() != 19 || text[10] != ' ' || text[13] != ':' || text[16] != ':')
    throw std::invalid_argument("invalid historical timestamp");
  const auto date = parse_trading_date(text.substr(0, 10));
  auto number = [&](unsigned at) {
    int n = 0;
    auto [p, e] = std::from_chars(text.data() + at, text.data() + at + 2, n);
    if (e != std::errc{} || p != text.data() + at + 2 || n < 0)
      throw std::invalid_argument("invalid historical timestamp");
    return n;
  };
  const auto h = number(11), m = number(14), s = number(17);
  if (h > 23 || m > 59 || s > 59 || date.year() < year(1990) || date.year() > year(2100))
    throw std::invalid_argument("invalid historical timestamp");
  return duration_cast<nanoseconds>(sys_days(date).time_since_epoch() + hours(h - 8) + minutes(m) +
                                    seconds(s))
      .count();
}
std::string format_shanghai_time(std::int64_t ns) {
  using namespace std::chrono;
  const auto t = sys_time<nanoseconds>(nanoseconds(ns)) + hours(8);
  const auto day = floor<days>(t);
  const auto clock = hh_mm_ss(t - day);
  std::ostringstream out;
  out << format_trading_date(year_month_day(day)) << ' ' << std::setfill('0') << std::setw(2)
      << clock.hours().count() << ':' << std::setw(2) << clock.minutes().count() << ':'
      << std::setw(2) << clock.seconds().count();
  return out.str();
}
} // namespace asterion
