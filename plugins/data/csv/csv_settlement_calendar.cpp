#include "csv_settlement_calendar.hpp"
#include <asterion/foundation/serialization.hpp>
#include <chrono>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string_view>
namespace asterion {
namespace {
constexpr std::string_view header =
    "trading_day,session_begin,session_end,settlement_price,schedule_source,"
    "settlement_source";
std::vector<std::string> fields(std::string_view row) {
  std::vector<std::string> result;
  std::size_t index = 0;
  while (true) {
    std::string value;
    if (index < row.size() && row[index] == '"') {
      ++index;
      bool closed = false;
      while (index < row.size()) {
        const char c = row[index++];
        if (c != '"')
          value += c;
        else if (index < row.size() && row[index] == '"') {
          value += '"';
          ++index;
        } else {
          closed = true;
          break;
        }
      }
      if (!closed || (index < row.size() && row[index] != ','))
        throw std::invalid_argument("invalid CSV quoting; multiline fields are not supported");
    } else {
      while (index < row.size() && row[index] != ',') {
        if (row[index] == '"')
          throw std::invalid_argument("quote in unquoted CSV field");
        value += row[index++];
      }
    }
    result.push_back(std::move(value));
    if (result.size() > 6)
      throw std::invalid_argument("expected six settlement CSV fields");
    if (index == row.size())
      break;
    ++index;
  }
  if (result.size() != 6)
    throw std::invalid_argument("expected six settlement CSV fields");
  return result;
}
std::int64_t timestamp(std::string_view value) {
  if ((value.size() != 20 && value.size() != 25) || value[4] != '-' || value[7] != '-' ||
      value[10] != 'T' || value[13] != ':' || value[16] != ':' ||
      (value.size() == 20 ? value[19] != 'Z'
                          : ((value[19] != '+' && value[19] != '-') || value[22] != ':')))
    throw std::invalid_argument(
        "expected timestamp YYYY-MM-DDTHH:MM:SSZ or explicit +/-HH:MM offset");
  const auto number = [&](std::size_t start, std::size_t size) {
    unsigned result = 0;
    for (std::size_t i = start; i < start + size; ++i) {
      if (value[i] < '0' || value[i] > '9')
        throw std::invalid_argument("invalid timestamp digits");
      result = result * 10 + static_cast<unsigned>(value[i] - '0');
    }
    return result;
  };
  const auto date = std::chrono::year(static_cast<int>(number(0, 4))) /
                    std::chrono::month(number(5, 2)) / std::chrono::day(number(8, 2));
  const auto hour = number(11, 2), minute = number(14, 2), second = number(17, 2);
  if (!date.ok() || hour > 23 || minute > 59 || second > 59)
    throw std::invalid_argument("invalid timestamp date or time");
  std::int64_t offset = 0;
  if (value.size() == 25) {
    const auto hours = number(20, 2), minutes = number(23, 2);
    if (hours > 14 || minutes > 59 || (hours == 14 && minutes != 0))
      throw std::invalid_argument("invalid UTC offset");
    offset = static_cast<std::int64_t>(hours * 3600 + minutes * 60) * (value[19] == '+' ? 1 : -1);
  }
  const auto seconds = std::chrono::sys_days(date).time_since_epoch().count() * 86400LL +
                       hour * 3600LL + minute * 60LL + second - offset;
  if (seconds < 0 || seconds > std::numeric_limits<std::int64_t>::max() / 1000000000LL)
    throw std::invalid_argument("timestamp outside nonnegative int64 nanoseconds");
  return seconds * 1000000000LL;
}
void source(const std::string& value) {
  if (value.empty() || value.size() > 256 || value.find_first_not_of(' ') == std::string::npos)
    throw std::invalid_argument("source must contain 1..256 UTF-8 bytes");
  for (unsigned char c : value)
    if (c < 32 || c == 127)
      throw std::invalid_argument("source contains control bytes");
  static_cast<void>(Json(value).dump()); // Validate UTF-8 without rewriting it.
}
} // namespace
CsvSettlementCalendar::CsvSettlementCalendar(Instrument instrument, std::string contents)
    : instrument_(std::move(instrument)), contents_(std::move(contents)) {
  instrument_.validate();
  if (instrument_.asset_class != AssetClass::futures || contents_.empty() ||
      contents_.size() > 1024 * 1024)
    throw std::invalid_argument("settlement CSV requires futures instrument and at most 1 MiB");
}
PluginDescriptor CsvSettlementCalendar::descriptor() const {
  return {"asterion.data.csv-settlement", PluginKind::data, plugin_contract_version, {}};
}
void CsvSettlementCalendar::start() {
  if (running_)
    throw std::logic_error("settlement source is already running");
  running_ = true;
}
std::vector<SettlementDay> CsvSettlementCalendar::read(std::stop_token stop) const {
  if (!running_)
    throw std::logic_error("settlement source is stopped");
  std::istringstream input(contents_);
  std::string line;
  std::getline(input, line);
  if (!line.empty() && line.back() == '\r')
    line.pop_back();
  if (line != header)
    throw std::invalid_argument("unsupported settlement CSV header");
  std::vector<SettlementDay> result;
  std::size_t line_number = 1;
  while (std::getline(input, line)) {
    if (stop.stop_requested())
      throw std::runtime_error("settlement import cancelled");
    ++line_number;
    try {
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      if (line.empty() || line.size() > 4096 || line_number > 1025)
        throw std::invalid_argument("invalid settlement CSV row or row limit");
      const auto row = fields(line);
      source(row[4]);
      source(row[5]);
      const auto price = Decimal::parse(row[3]);
      if (price.str() != row[3])
        throw std::invalid_argument("settlement price must be canonical decimal");
      TradingSession interval{timestamp(row[1]), timestamp(row[2])};
      if (!result.empty() && result.back().schedule.trading_day() == row[0]) {
        auto& day = result.back();
        if (day.settlement_price != price || day.schedule_source != row[4] ||
            day.settlement_source != row[5])
          throw std::invalid_argument(
              "one trading day has conflicting settlement or source metadata");
        auto sessions = day.schedule.sessions();
        if (sessions.size() == 16)
          throw std::invalid_argument("at most 16 sessions per day");
        sessions.push_back(interval);
        day.schedule = TradingDaySchedule(row[0], std::move(sessions));
      } else {
        TradingDaySchedule schedule(row[0], {interval});
        if (result.size() == 64 ||
            (!result.empty() &&
             (row[0] <= result.back().schedule.trading_day() ||
              interval.begin_ns < result.back().schedule.sessions().back().end_ns)))
          throw std::invalid_argument("calendar days must increase without "
                                      "interval overlap, at most 64 days");
        result.push_back({std::move(schedule), price, row[4], row[5]});
      }
    } catch (const std::exception& error) {
      throw std::runtime_error("settlement CSV line " + std::to_string(line_number) + ": " +
                               error.what());
    }
  }
  if (stop.stop_requested())
    throw std::runtime_error("settlement import cancelled");
  if (result.empty())
    throw std::invalid_argument("settlement CSV has no days");
  return result;
}
} // namespace asterion
