#include "session_calendar.hpp"
#include <charconv>
#include <chrono>
#include <stdexcept>

namespace asterion::sessions {
namespace {
using namespace std::chrono;
int parse_clock(const Json& value) {
  const auto text = value.get<std::string>();
  if (text.size() != 5 || text[2] != ':')
    throw std::invalid_argument("session time must be HH:MM");
  int hours = 0, minutes = 0;
  const auto h = std::from_chars(text.data(), text.data() + 2, hours);
  const auto m = std::from_chars(text.data() + 3, text.data() + 5, minutes);
  if (h.ec != std::errc{} || m.ec != std::errc{} || h.ptr != text.data() + 2 ||
      m.ptr != text.data() + 5 || hours > 23 || minutes > 59)
    throw std::invalid_argument("session time must be HH:MM");
  return hours * 60 + minutes;
}
LocalInterval interval(const Json& value) {
  if (!value.is_array() || value.size() != 2)
    throw std::invalid_argument("session interval must be [begin, end]");
  return {parse_clock(value.at(0)), parse_clock(value.at(1))};
}
int parse_offset(const std::string& text) {
  if (text.size() != 6 || (text[0] != '+' && text[0] != '-') || text[3] != ':')
    throw std::invalid_argument("utc_offset must be +HH:MM or -HH:MM");
  const auto minutes = parse_clock(Json(text.substr(1)));
  if (minutes > 14 * 60)
    throw std::invalid_argument("utc_offset out of range");
  return text[0] == '-' ? -minutes : minutes;
}
sys_days parse_day(const std::string& text) {
  if (text.size() != 10 || text[4] != '-' || text[7] != '-')
    throw std::invalid_argument("trading day must be YYYY-MM-DD");
  int y = 0;
  unsigned m = 0, d = 0;
  const auto a = std::from_chars(text.data(), text.data() + 4, y);
  const auto b = std::from_chars(text.data() + 5, text.data() + 7, m);
  const auto c = std::from_chars(text.data() + 8, text.data() + 10, d);
  const year_month_day date{year{y}, month{m}, day{d}};
  if (a.ec != std::errc{} || b.ec != std::errc{} || c.ec != std::errc{} || !date.ok() ||
      date.year() < year{1970})
    throw std::invalid_argument("invalid trading day " + text);
  return sys_days{date};
}
std::int64_t utc_ns(sys_days date, int local_minute, int offset_minutes) {
  const auto local = date + minutes{local_minute};
  return duration_cast<nanoseconds>((local - minutes{offset_minutes}).time_since_epoch()).count();
}
bool weekend(sys_days date) {
  const weekday day{date};
  return day == Saturday || day == Sunday;
}
} // namespace

SessionCatalog SessionCatalog::parse(const Json& document) {
  require_fields(document,
                 {"version", "source", "utc_offset", "day_sessions", "night_sessions", "products"});
  if (document.at("version") != 1)
    throw std::invalid_argument("unsupported session catalog version");
  const auto& source = document.at("source");
  SessionCatalog catalog;
  catalog.provenance_ = source.at("title").get<std::string>() + ", reviewed " +
                        source.at("reviewed").get<std::string>();
  const int offset = parse_offset(document.at("utc_offset").get<std::string>());
  std::map<std::string, std::vector<LocalInterval>> days;
  for (const auto& [name, value] : document.at("day_sessions").items()) {
    auto& list = days[name];
    int previous = -1;
    for (const auto& item : value) {
      const auto part = interval(item);
      if (part.end_minute <= part.begin_minute || part.begin_minute < previous)
        throw std::invalid_argument("day sessions must be ordered within one day: " + name);
      previous = part.end_minute;
      list.push_back(part);
    }
    if (list.empty())
      throw std::invalid_argument("empty day session group " + name);
  }
  std::map<std::string, LocalInterval> nights;
  for (const auto& [name, value] : document.at("night_sessions").items())
    nights[name] = interval(value);
  for (const auto& [venue, products] : document.at("products").items())
    for (const auto& [product, reference] : products.items()) {
      if (!reference.is_array() || reference.size() != 2)
        throw std::invalid_argument("product must reference [day group, night group|null]");
      SessionTemplate entry{venue, product, {}, std::nullopt, offset};
      const auto day = days.find(reference.at(0).get<std::string>());
      if (day == days.end())
        throw std::invalid_argument("unknown day group for " + venue + "/" + product);
      entry.day = day->second;
      if (!reference.at(1).is_null()) {
        const auto night = nights.find(reference.at(1).get<std::string>());
        if (night == nights.end())
          throw std::invalid_argument("unknown night group for " + venue + "/" + product);
        entry.night = night->second;
      }
      catalog.templates_.emplace(std::pair{venue, product}, std::move(entry));
    }
  return catalog;
}
const SessionTemplate& SessionCatalog::find(const std::string& venue,
                                            const std::string& product) const {
  const auto found = templates_.find({venue, product});
  if (found == templates_.end())
    throw std::invalid_argument("no session template for " + venue + "/" + product);
  return found->second;
}

std::vector<GeneratedDay> generate(const SessionTemplate& session,
                                   const std::vector<std::string>& trading_days,
                                   const std::optional<std::string>& previous) {
  if (trading_days.empty() || trading_days.size() > 64)
    throw std::invalid_argument("between 1 and 64 trading days are required");
  std::vector<GeneratedDay> result;
  // A plain flag and value, not std::optional<sys_days>: GCC 13 reports a
  // false maybe-uninitialized warning for the optional at -O2.
  bool anchored = previous.has_value();
  sys_days prior{};
  if (anchored)
    prior = parse_day(*previous);
  for (const auto& label : trading_days) {
    const auto date = parse_day(label);
    if (weekend(date))
      throw std::invalid_argument("trading day falls on a weekend: " + label);
    if (anchored && prior >= date)
      throw std::invalid_argument("trading days must be strictly increasing");
    GeneratedDay day{label, {}, false};
    if (session.night && anchored) {
      bool only_weekends = true;
      for (auto between = prior + days{1}; between < date; between += days{1})
        only_weekends = only_weekends && weekend(between);
      if (only_weekends) {
        const auto& night = *session.night;
        const auto end_date = night.end_minute <= night.begin_minute ? prior + days{1} : prior;
        day.sessions.push_back({utc_ns(prior, night.begin_minute, session.utc_offset_minutes),
                                utc_ns(end_date, night.end_minute, session.utc_offset_minutes)});
        day.night = true;
      }
    }
    for (const auto& part : session.day)
      day.sessions.push_back({utc_ns(date, part.begin_minute, session.utc_offset_minutes),
                              utc_ns(date, part.end_minute, session.utc_offset_minutes)});
    // The domain model validates ordering and non-overlap.
    static_cast<void>(TradingDaySchedule(label, day.sessions));
    result.push_back(std::move(day));
    prior = date;
    anchored = true;
  }
  return result;
}
} // namespace asterion::sessions
