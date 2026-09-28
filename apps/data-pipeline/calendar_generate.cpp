#include "calendar_generate.hpp"
#include <chrono>
#include <cstdio>
#include <stdexcept>
namespace asterion::data_pipeline {
namespace {
constexpr std::string_view input_header = "trading_day,settlement_price,settlement_source";
std::vector<std::string> fields(std::string_view row, std::size_t line) {
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
        throw std::invalid_argument("line " + std::to_string(line) + ": invalid CSV quoting");
    } else {
      while (index < row.size() && row[index] != ',') {
        if (row[index] == '"')
          throw std::invalid_argument("line " + std::to_string(line) +
                                      ": quote in unquoted CSV field");
        value += row[index++];
      }
    }
    result.push_back(std::move(value));
    if (index == row.size())
      break;
    ++index;
  }
  if (result.size() != 3)
    throw std::invalid_argument("line " + std::to_string(line) +
                                ": expected trading_day,settlement_price,settlement_source");
  return result;
}
std::string quote(const std::string& value) {
  if (value.find_first_of(",\"") == std::string::npos)
    return value;
  std::string result = "\"";
  for (const char c : value)
    result += c == '"' ? std::string("\"\"") : std::string(1, c);
  return result + '"';
}
// UTC nanoseconds rendered as local wall clock with an explicit offset.
std::string local_time(std::int64_t utc_ns, int offset_minutes) {
  using namespace std::chrono;
  const auto local =
      sys_seconds{duration_cast<seconds>(nanoseconds{utc_ns})} + minutes{offset_minutes};
  const auto day = floor<days>(local);
  const year_month_day date{day};
  const hh_mm_ss clock{local - day};
  const int offset = offset_minutes < 0 ? -offset_minutes : offset_minutes;
  char text[32];
  std::snprintf(text, sizeof text, "%04d-%02u-%02uT%02d:%02d:%02d%c%02d:%02d",
                static_cast<int>(date.year()), static_cast<unsigned>(date.month()),
                static_cast<unsigned>(date.day()), static_cast<int>(clock.hours().count()),
                static_cast<int>(clock.minutes().count()),
                static_cast<int>(clock.seconds().count()), offset_minutes < 0 ? '-' : '+',
                offset / 60, offset % 60);
  return text;
}
} // namespace
std::string generate_calendar_csv(const sessions::SessionCatalog& catalog, const std::string& venue,
                                  const std::string& product, std::string_view settlements,
                                  const std::optional<std::string>& previous) {
  const auto& session = catalog.find(venue, product);
  std::vector<std::string> days;
  std::vector<std::vector<std::string>> rows;
  std::size_t line = 0, start = 0;
  while (start < settlements.size()) {
    auto end = settlements.find('\n', start);
    if (end == std::string_view::npos)
      end = settlements.size();
    auto row = settlements.substr(start, end - start);
    if (!row.empty() && row.back() == '\r')
      row.remove_suffix(1);
    start = end + 1;
    if (++line == 1) {
      if (row != input_header)
        throw std::invalid_argument("settlement header must be " + std::string(input_header));
      continue;
    }
    if (row.empty())
      throw std::invalid_argument("line " + std::to_string(line) + ": empty row");
    auto values = fields(row, line);
    days.push_back(values[0]);
    rows.push_back(std::move(values));
  }
  if (rows.empty())
    throw std::invalid_argument("no settlement rows");
  const std::string schedule = "template " + venue + "/" + product + ": " + catalog.provenance();
  if (schedule.size() > 256)
    throw std::invalid_argument("session template provenance exceeds 256 bytes");
  const auto generated = sessions::generate(session, days, previous);
  std::string csv =
      "trading_day,session_begin,session_end,settlement_price,schedule_source,settlement_source\n";
  for (std::size_t i = 0; i < generated.size(); ++i)
    for (const auto& interval : generated[i].sessions)
      csv += rows[i][0] + ',' + local_time(interval.begin_ns, session.utc_offset_minutes) + ',' +
             local_time(interval.end_ns, session.utc_offset_minutes) + ',' + quote(rows[i][1]) +
             ',' + quote(schedule) + ',' + quote(rows[i][2]) + '\n';
  return csv;
}
} // namespace asterion::data_pipeline
