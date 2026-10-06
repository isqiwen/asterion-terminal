#include "tushare.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <iomanip>
#include <map>
#include <regex>
#include <sstream>
#include <stdexcept>
namespace asterion::tushare {
namespace {
constexpr std::int64_t second = 1000000000;
const std::map<std::string, std::string> venues{{"SHF", "SHFE"},  {"DCE", "DCE"}, {"ZCE", "CZCE"},
                                                {"CFX", "CFFEX"}, {"INE", "INE"}, {"GFE", "GFEX"}};
// Preserve numeric lexemes before any binary floating-point conversion. Values
// that exceed Decimal's exact precision/range are rejected, never rounded.
class ExactJson final : public nlohmann::json_sax<Json> {
public:
  Json result;
  struct Frame {
    Json value;
    std::string key;
  };
  std::vector<Frame> frames;
  bool add(Json value) {
    if (frames.empty())
      result = std::move(value);
    else if (frames.back().value.is_array())
      frames.back().value.push_back(std::move(value));
    else
      frames.back().value[frames.back().key] = std::move(value);
    return true;
  }
  bool null() override { return add(nullptr); }
  bool boolean(bool value) override { return add(value); }
  bool number_integer(number_integer_t value) override { return add(std::to_string(value)); }
  bool number_unsigned(number_unsigned_t value) override { return add(std::to_string(value)); }
  bool number_float(number_float_t, const string_t& raw) override { return add(raw); }
  bool string(string_t& value) override { return add(value); }
  bool binary(binary_t&) override { return false; }
  bool start_object(std::size_t) override {
    frames.push_back({Json::object(), {}});
    return frames.size() < 16;
  }
  bool key(string_t& value) override {
    if (frames.back().value.contains(value))
      return false;
    frames.back().key = value;
    return true;
  }
  bool end_object() override { return end(); }
  bool start_array(std::size_t) override {
    frames.push_back({Json::array(), {}});
    return frames.size() < 16;
  }
  bool end_array() override { return end(); }
  bool end() {
    auto value = std::move(frames.back().value);
    frames.pop_back();
    return add(std::move(value));
  }
  bool parse_error(std::size_t, const std::string&, const nlohmann::detail::exception&) override {
    return false;
  }
};
std::string text(const Json& value) {
  if (!value.is_string())
    throw std::invalid_argument("Tushare missing or invalid field");
  return value.get<std::string>();
}
// Expand scientific notation without converting through double.
Decimal decimal(const Json& value, int shift = 0) {
  auto raw = text(value);
  const auto exponent = raw.find_first_of("eE");
  if (exponent != std::string::npos || shift) {
    auto exp = exponent == std::string::npos ? std::string_view("0")
                                             : std::string_view(raw).substr(exponent + 1);
    if (!exp.empty() && exp.front() == '+')
      exp.remove_prefix(1);
    int power = 0;
    const auto [end, error] = std::from_chars(exp.data(), exp.data() + exp.size(), power);
    if (error != std::errc{} || end != exp.data() + exp.size() || power < -32 || power > 32)
      throw std::invalid_argument("Tushare decimal exponent exceeds exact range");
    power += shift;
    auto mantissa = raw.substr(0, exponent);
    const bool negative = !mantissa.empty() && mantissa.front() == '-';
    if (negative)
      mantissa.erase(0, 1);
    auto dot = mantissa.find('.');
    int point = static_cast<int>(dot == std::string::npos ? mantissa.size() : dot) + power;
    if (dot != std::string::npos)
      mantissa.erase(dot, 1);
    if (point <= 0)
      raw = "0." + std::string(-point, '0') + mantissa;
    else if (point >= static_cast<int>(mantissa.size()))
      raw = mantissa + std::string(point - mantissa.size(), '0');
    else
      raw = mantissa.substr(0, point) + "." + mantissa.substr(point);
    // Moving the decimal point can leave zeros before the integer part
    // (0.000100000001 * 10000 -> 00001.00000001). Canonicalize only
    // those generated leading zeros without rounding the fractional digits.
    const auto integer_end = raw.find('.');
    const auto integer_size = integer_end == std::string::npos ? raw.size() : integer_end;
    std::size_t leading = 0;
    while (leading + 1 < integer_size && raw[leading] == '0')
      ++leading;
    raw.erase(0, leading);
    if (negative)
      raw = "-" + raw;
  }
  // Providers often emit .0 for integer quantities; insignificant trailing
  // zeros do not consume fixed-point precision.
  if (raw.find('.') != std::string::npos) {
    while (!raw.empty() && raw.back() == '0')
      raw.pop_back();
    if (!raw.empty() && raw.back() == '.')
      raw.pop_back();
  }
  return Decimal::parse(raw);
}
} // namespace
std::int64_t parse_time(std::string_view text) {
  return parse_shanghai_time(text);
}
std::string format_time(std::int64_t ns) {
  return format_shanghai_time(ns);
}
namespace {
AccessFailure rejection(const Json& response) {
  const auto message = response.value("msg", std::string{});
  if (message.find("token") != std::string::npos &&
      (message.find("不正确") != std::string::npos || message.find("无效") != std::string::npos))
    return AccessFailure::invalid_credential;
  if (message.find("每分钟") != std::string::npos || message.find("频率") != std::string::npos ||
      message.find("每小时") != std::string::npos || message.find("每天最多") != std::string::npos)
    return AccessFailure::rate_limit;
  // Tushare documents 2002 as an access-permission error.
  if (text(response.at("code")) == "2002")
    return AccessFailure::permission;
  return AccessFailure::failed;
}
} // namespace
InstrumentId instrument(std::string_view ts_code) {
  const std::string value(ts_code);
  static const std::regex pattern("^([A-Z]{1,3}[0-9]{3,4})\\.(SHF|DCE|ZCE|CFX|INE|GFE)$");
  std::smatch match;
  if (!std::regex_match(value, match, pattern))
    throw std::invalid_argument(
        "Tushare requires a dated futures contract, not a continuous alias");
  const auto symbol = match[1].str();
  const auto suffix = match[2].str();
  const auto digits = symbol.substr(symbol.find_first_of("0123456789"));
  const auto month = std::stoi(digits.substr(digits.size() - 2));
  if (month < 1 || month > 12 || (suffix != "ZCE" && digits.size() != 4))
    throw std::invalid_argument(
        "Tushare requires a dated futures contract, not a continuous alias");
  return {venues.at(match[2].str()), match[1].str()};
}
std::string code(const HistoryIdentity& identity) {
  auto id = identity.exchange_id();
  std::ranges::transform(id.symbol, id.symbol.begin(),
                         [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  for (const auto& [suffix, venue] : venues)
    if (venue == id.venue) {
      auto value = id.symbol + "." + suffix;
      (void)instrument(value);
      return value;
    }
  throw std::invalid_argument("unsupported Tushare futures venue");
}
void mapping(const HistoryIdentity& identity, const std::string& provider_code) {
  identity.validate();
  const auto provider = instrument(provider_code);
  const auto symbol = provider.symbol;
  const auto digits = symbol.substr(symbol.find_first_of("0123456789"));
  auto product = symbol.substr(0, symbol.find_first_of("0123456789"));
  std::ranges::transform(product, product.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  const auto expected =
      digits.size() == 3 ? identity.delivery_month.substr(3, 1) + identity.delivery_month.substr(5)
                         : identity.delivery_month.substr(2, 2) + identity.delivery_month.substr(5);
  if (provider.venue != identity.venue || product != identity.product || digits != expected)
    throw std::invalid_argument("Tushare contract mapping mismatch");
}
void validate(const HistoricalBarRange& range) {
  mapping(range.instrument, range.source_instrument);
  if (range.interval_minutes != 1 && range.interval_minutes != 5 && range.interval_minutes != 15 &&
      range.interval_minutes != 30 && range.interval_minutes != 60)
    throw std::invalid_argument("unsupported Tushare minute frequency");
  if (range.begin_ns > range.end_ns || range.begin_ns % second || range.end_ns % second ||
      range.begin_ns < parse_time("1990-01-01 00:00:00") ||
      range.end_ns > parse_time("2100-12-31 23:59:59"))
    throw std::invalid_argument("invalid Tushare download interval");
}
Minutes::Minutes(std::string token, Post post) : token_(std::move(token)), post_(std::move(post)) {
  if (token_.empty() || token_.size() > 256 || token_.find_first_of("\r\n\t ") != std::string::npos)
    throw std::invalid_argument("Tushare token is required without whitespace");
}
HistorySemantics Minutes::semantics() const {
  return {"tushare.ft_mins", "tushare.minutes.v3", "Asia/Shanghai", "bar_end"};
}
std::string minute_trading_day(std::int64_t bar_end_ns, const CalendarLookup& open) {
  using namespace std::chrono;
  const auto wall = format_time(bar_end_ns); // YYYY-MM-DD HH:MM:SS
  const auto date = sys_days(parse_trading_date(wall.substr(0, 10)));
  const auto hour = std::stoi(wall.substr(11, 2));
  const auto is_open = [&](sys_days day) {
    const auto value = open(year_month_day(day));
    if (!value)
      throw std::invalid_argument("exchange trading calendar does not cover the minute bar");
    return *value;
  };
  if (hour >= 6 && hour < 18) {
    if (!is_open(date))
      throw std::invalid_argument("minute bar falls on a closed exchange day");
    return format_trading_date(year_month_day(date));
  }
  // Night session: it starts on an open evening and trades for the next open day.
  const auto evening = hour >= 18 ? date : date - days(1);
  if (!is_open(evening))
    throw std::invalid_argument("night minute bar follows a closed exchange day");
  for (auto day = evening + days(1); day <= evening + days(31); day += days(1))
    if (is_open(day))
      return format_trading_date(year_month_day(day));
  throw std::invalid_argument("no open exchange day follows the night session");
}
std::optional<bool> Minutes::open(const std::string& venue, std::chrono::year_month_day day,
                                  std::stop_token stop) {
  using namespace std::chrono;
  const auto year = static_cast<int>(day.year());
  if (calendar_years_.insert({venue, year}).second) {
    const Json request{{"api_name", "trade_cal"},
                       {"token", token_},
                       {"params",
                        {{"exchange", venue},
                         {"start_date", std::to_string(year) + "0101"},
                         {"end_date", std::to_string(year) + "1231"}}},
                       {"fields", "exchange,cal_date,is_open"}};
    const auto raw = post_(request.dump(), stop);
    if (stop.stop_requested())
      throw std::runtime_error("Tushare download cancelled");
    if (raw.size() > 1024 * 1024)
      throw std::runtime_error("Tushare response exceeds size limit");
    try {
      ExactJson parser;
      if (!Json::sax_parse(raw, &parser))
        throw std::runtime_error("invalid Tushare response JSON");
      const auto& root = parser.result;
      if (text(root.at("code")) != "0")
        throw RequestError(rejection(root), "Tushare rejected the trading calendar request");
      const auto& fields = root.at("data").at("fields");
      const auto& items = root.at("data").at("items");
      std::map<std::string, std::size_t> columns;
      for (std::size_t i = 0; i < fields.size(); ++i)
        columns.emplace(text(fields[i]), i);
      if (!items.is_array() || items.size() > 366 || !columns.contains("exchange") ||
          !columns.contains("cal_date") || !columns.contains("is_open"))
        throw std::invalid_argument("invalid Tushare trading calendar");
      auto& days = calendar_[venue];
      for (const auto& row : items) {
        const auto value = text(row.at(columns.at("cal_date")));
        const auto flag = text(row.at(columns.at("is_open")));
        if (text(row.at(columns.at("exchange"))) != venue || value.size() != 8 ||
            (flag != "0" && flag != "1"))
          throw std::invalid_argument("invalid Tushare trading calendar");
        const auto date = parse_trading_date(value.substr(0, 4) + "-" + value.substr(4, 2) + "-" +
                                             value.substr(6, 2));
        if (static_cast<int>(date.year()) != year)
          throw std::invalid_argument("invalid Tushare trading calendar");
        days[sys_days(date)] = flag == "1";
      }
    } catch (const nlohmann::json::exception&) {
      calendar_years_.erase({venue, year});
      throw std::runtime_error("invalid Tushare response schema");
    } catch (...) {
      calendar_years_.erase({venue, year});
      throw;
    }
  }
  const auto& days = calendar_[venue];
  const auto found = days.find(sys_days(day));
  return found == days.end() ? std::nullopt : std::optional(found->second);
}
void Minutes::start() {
  started_ = true;
}
void Minutes::stop() noexcept {
  started_ = false;
}
std::vector<HistoricalBar> Minutes::read(const HistoricalBarRange& range, std::stop_token stop) {
  validate(range);
  if (!started_)
    throw std::logic_error("Tushare data plugin is not started");
  if (stop.stop_requested())
    throw std::runtime_error("Tushare download cancelled");
  if (range.end_ns - range.begin_ns >= 86400 * second)
    throw std::invalid_argument("Tushare minute requests must be shorter than one day");
  const Json request{{"api_name", "ft_mins"},
                     {"token", token_},
                     {"params",
                      {{"ts_code", range.source_instrument},
                       {"freq", std::to_string(range.interval_minutes) + "min"},
                       {"start_date", format_time(range.begin_ns)},
                       {"end_date", format_time(range.end_ns)}}},
                     {"fields", "ts_code,trade_time,open,high,low,close,vol,amount,oi"}};
  const auto raw = post_(request.dump(), stop);
  if (stop.stop_requested())
    throw std::runtime_error("Tushare download cancelled");
  if (raw.size() > 8 * 1024 * 1024)
    throw std::runtime_error("Tushare response exceeds size limit");
  ExactJson parser;
  if (!Json::sax_parse(raw, &parser))
    throw std::runtime_error("invalid Tushare response JSON");
  try {
    const auto& root = parser.result;
    if (text(root.at("code")) != "0") {
      // Provider text can echo a token. Never pass it to logs or the UI.
      throw RequestError(
          rejection(root),
          "Tushare rejected request; verify token, minute-data entitlement and rate limit");
    }
    const auto& fields = root.at("data").at("fields");
    const auto& items = root.at("data").at("items");
    if (!fields.is_array() || fields.size() != 9 || !items.is_array())
      throw std::invalid_argument("invalid Tushare table");
    if (items.size() >= 8000)
      throw std::runtime_error(
          "Tushare row limit reached; refusing a potentially truncated dataset");
    std::map<std::string, std::size_t> columns;
    for (std::size_t i = 0; i < fields.size(); ++i)
      if (!columns.emplace(text(fields[i]), i).second)
        throw std::invalid_argument("duplicate Tushare field");
    for (auto field :
         {"ts_code", "trade_time", "open", "high", "low", "close", "vol", "amount", "oi"})
      if (!columns.contains(field))
        throw std::invalid_argument("missing Tushare field");
    std::vector<HistoricalBar> bars;
    for (const auto& row : items) {
      if (!row.is_array() || row.size() != fields.size())
        throw std::invalid_argument("invalid Tushare row");
      const auto get = [&](const char* key) -> const Json& { return row.at(columns.at(key)); };
      if (text(get("ts_code")) != range.source_instrument)
        throw std::invalid_argument("Tushare returned another contract");
      HistoricalBar bar{parse_time(text(get("trade_time"))),
                        decimal(get("open")),
                        decimal(get("high")),
                        decimal(get("low")),
                        decimal(get("close")),
                        decimal(get("vol")),
                        decimal(get("amount")),
                        decimal(get("oi"))};
      bar.validate();
      if (bar.timestamp_ns < range.begin_ns || bar.timestamp_ns > range.end_ns ||
          bar.timestamp_ns % (60 * second) || bar.volume.raw() % 100000000)
        throw std::invalid_argument("Tushare bar outside interval or invalid minute/volume");
      bars.push_back(bar);
    }
    std::ranges::sort(bars, {}, &HistoricalBar::timestamp_ns);
    for (std::size_t i = 1; i < bars.size(); ++i)
      if (bars[i].timestamp_ns == bars[i - 1].timestamp_ns)
        throw std::invalid_argument("duplicate Tushare bar timestamp");
    const auto venue = range.instrument.venue;
    for (auto& bar : bars) {
      // Commodity exchanges pause 10:15-10:30. A 1 or 5 minute bar labeled
      // 10:30 would have started in that pause, so the labels are not bar ends.
      if (venue != "CFFEX" && range.interval_minutes <= 5 &&
          format_time(bar.timestamp_ns).substr(11) == "10:30:00")
        throw std::invalid_argument("Tushare minute labels are not bar end times");
      bar.trading_day = minute_trading_day(bar.timestamp_ns, [&](std::chrono::year_month_day day) {
        return open(venue, day, stop);
      });
    }
    return bars;
  } catch (const nlohmann::json::exception&) {
    throw std::runtime_error("invalid Tushare response schema");
  }
}

namespace {
std::int64_t listing_date(const std::string& date) {
  if (date.size() != 8 || date.find_first_not_of("0123456789") != std::string::npos)
    throw std::invalid_argument("invalid Tushare contract lifetime");
  return parse_time(date.substr(0, 4) + "-" + date.substr(4, 2) + "-" + date.substr(6, 2) +
                    " 00:00:00");
}
} // namespace
HistoricalBarRange contract_range(const FuturesListing& item, unsigned interval,
                                  std::int64_t cutoff_ns) {
  const auto begin = listing_date(item.list_date);
  const auto last = listing_date(item.delist_date);
  if (last < begin || cutoff_ns < begin)
    throw std::invalid_argument("invalid Tushare contract lifetime");
  HistoricalBarRange range{item.identity,
                           interval,
                           begin,
                           std::min(last + 86400 * second - second, cutoff_ns / second * second),
                           "tushare.ft_mins",
                           item.ts_code};
  validate(range);
  return range;
}
std::vector<FuturesListing> contracts(const std::string& token, const std::string& exchange,
                                      const std::string& product, std::stop_token stop, Post post) {
  if (token.empty() || token.size() > 256 || token.find_first_of("\r\n\t ") != std::string::npos)
    throw std::invalid_argument("Tushare token is required without whitespace");
  if (!std::ranges::any_of(venues, [&](const auto& entry) { return entry.second == exchange; }) ||
      !std::regex_match(product, std::regex("[A-Z]{1,3}")))
    throw std::invalid_argument("invalid Tushare contract query");
  const Json request{{"api_name", "fut_basic"},
                     {"token", token},
                     {"params", {{"exchange", exchange}, {"fut_code", product}, {"fut_type", "1"}}},
                     {"fields", "ts_code,name,exchange,fut_code,list_date,delist_date,multiplier,"
                                "per_unit,trade_unit,quote_unit,d_month"}};
  if (stop.stop_requested())
    throw std::runtime_error("Tushare download cancelled");
  const auto raw = post(request.dump(), stop);
  if (stop.stop_requested())
    throw std::runtime_error("Tushare download cancelled");
  if (raw.size() > 8 * 1024 * 1024)
    throw std::runtime_error("Tushare response exceeds size limit");
  ExactJson parser;
  if (!Json::sax_parse(raw, &parser))
    throw std::runtime_error("invalid Tushare response JSON");
  try {
    const auto& root = parser.result;
    if (text(root.at("code")) != "0")
      throw RequestError(
          rejection(root),
          "Tushare rejected contract query; verify token and contract-data entitlement");
    const auto& fields = root.at("data").at("fields");
    const auto& rows = root.at("data").at("items");
    if (!fields.is_array() || fields.size() != 11 || !rows.is_array() || rows.size() >= 10000)
      throw std::invalid_argument("invalid or truncated Tushare contract catalog");
    std::map<std::string, std::size_t> columns;
    for (std::size_t i = 0; i < fields.size(); ++i)
      if (!columns.emplace(text(fields[i]), i).second)
        throw std::invalid_argument("duplicate Tushare field");
    for (auto key : {"ts_code", "name", "exchange", "fut_code", "list_date", "delist_date",
                     "multiplier", "per_unit", "trade_unit", "quote_unit", "d_month"})
      if (!columns.contains(key))
        throw std::invalid_argument("missing Tushare field");
    std::vector<FuturesListing> result;
    for (const auto& row : rows) {
      if (!row.is_array() || row.size() != fields.size())
        throw std::invalid_argument("invalid Tushare row");
      auto get = [&](const char* key) { return text(row.at(columns.at(key))); };
      FuturesListing item{get("ts_code"),  get("name"),      get("exchange"),
                          get("fut_code"), get("list_date"), get("delist_date")};
      const auto positive = [&](const char* key) -> std::optional<Decimal> {
        const auto& value = row.at(columns.at(key));
        if (value.is_null())
          return {};
        const auto parsed = decimal(value);
        if (parsed.raw() <= 0)
          throw std::invalid_argument("Tushare contract unit must be positive");
        return parsed;
      };
      const auto unit = [&](const char* key) -> std::optional<std::string> {
        const auto& value = row.at(columns.at(key));
        if (value.is_null())
          return {};
        auto parsed = text(value);
        if (parsed.empty() || parsed.size() > 128 ||
            parsed.find_first_of("\r\n\t") != std::string::npos)
          throw std::invalid_argument("invalid Tushare contract unit label");
        return parsed;
      };
      const auto month = get("d_month");
      if (month.size() != 6 || month.find_first_not_of("0123456789") != std::string::npos)
        throw std::invalid_argument("invalid Tushare delivery month");
      auto canonical_product = item.product;
      std::ranges::transform(canonical_product, canonical_product.begin(),
                             [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      item.identity = {item.exchange, canonical_product,
                       month.substr(0, 4) + "-" + month.substr(4)};
      mapping(item.identity, item.ts_code);
      item.multiplier = positive("multiplier");
      item.per_unit = positive("per_unit");
      item.trade_unit = unit("trade_unit");
      item.quote_unit = unit("quote_unit");
      if (item.exchange != exchange || item.product != product || item.name.empty() ||
          item.name.size() > 256 || instrument(item.ts_code).venue != exchange ||
          item.ts_code.substr(0, item.ts_code.find_first_of("0123456789")) != product)
        throw std::invalid_argument("Tushare returned another contract");
      contract_range(item, 1, listing_date(item.delist_date));
      result.push_back(std::move(item));
    }
    std::ranges::sort(result, {}, [](const auto& item) { return item.identity.key(); });
    for (std::size_t i = 1; i < result.size(); ++i)
      if (result[i - 1].identity.key() == result[i].identity.key())
        throw std::invalid_argument("duplicate Tushare contract");
    return result;
  } catch (const nlohmann::json::exception&) {
    throw std::runtime_error("invalid Tushare response schema");
  }
}

namespace {
std::string daily_date(std::chrono::year_month_day day) {
  if (!day.ok() || day.year() < std::chrono::year(1990) || day.year() > std::chrono::year(2100))
    throw std::invalid_argument("invalid Tushare daily date");
  auto value = format_trading_date(day);
  std::erase(value, '-');
  return value;
}
std::chrono::year_month_day daily_date(const std::string& value) {
  if (value.size() != 8 || value.find_first_not_of("0123456789") != std::string::npos)
    throw std::invalid_argument("invalid Tushare daily date");
  const auto day =
      parse_trading_date(value.substr(0, 4) + "-" + value.substr(4, 2) + "-" + value.substr(6, 2));
  (void)daily_date(day);
  return day;
}
} // namespace
Daily::Daily(std::string token, Post post) : token_(std::move(token)), post_(std::move(post)) {
  if (token_.empty() || token_.size() > 256 || token_.find_first_of("\r\n\t ") != std::string::npos)
    throw std::invalid_argument("Tushare token is required without whitespace");
}
HistorySemantics Daily::semantics() const {
  return {"tushare.fut_daily", "tushare.daily.v2", "Asia/Shanghai", "trading_day"};
}
void Daily::start() {
  started_ = true;
}
void Daily::stop() noexcept {
  started_ = false;
}
std::vector<HistoricalDailyBar> Daily::read(const HistoricalDailyRange& range,
                                            std::stop_token stop) {
  const auto first = daily_date(range.begin), last = daily_date(range.end);
  mapping(range.instrument, range.source_instrument);
  const auto ts_code = range.source_instrument;
  if (range.begin > range.end ||
      (std::chrono::sys_days(range.end) - std::chrono::sys_days(range.begin)).count() >= 366)
    throw std::invalid_argument("Tushare daily requests must span 1..366 dates");
  if (!started_)
    throw std::logic_error("Tushare data plugin is not started");
  if (stop.stop_requested())
    throw std::runtime_error("Tushare download cancelled");
  const Json request{
      {"api_name", "fut_daily"},
      {"token", token_},
      {"params", {{"ts_code", ts_code}, {"start_date", first}, {"end_date", last}}},
      {"fields",
       "ts_code,trade_date,pre_close,pre_settle,open,high,low,close,settle,vol,amount,oi"}};
  const auto raw = post_(request.dump(), stop);
  if (stop.stop_requested())
    throw std::runtime_error("Tushare download cancelled");
  if (raw.size() > 8 * 1024 * 1024)
    throw std::runtime_error("Tushare response exceeds size limit");
  ExactJson parser;
  if (!Json::sax_parse(raw, &parser))
    throw std::runtime_error("invalid Tushare response JSON");
  try {
    const auto& root = parser.result;
    if (text(root.at("code")) != "0")
      throw RequestError(
          rejection(root),
          "Tushare rejected daily query; verify token, daily-data entitlement and rate limit");
    const auto& fields = root.at("data").at("fields");
    const auto& items = root.at("data").at("items");
    if (!fields.is_array() || fields.size() != 12 || !items.is_array())
      throw std::invalid_argument("invalid Tushare daily table");
    if (items.size() >= 2000)
      throw std::runtime_error(
          "Tushare row limit reached; refusing a potentially truncated dataset");
    std::map<std::string, std::size_t> columns;
    for (std::size_t i = 0; i < fields.size(); ++i)
      if (!columns.emplace(text(fields[i]), i).second)
        throw std::invalid_argument("duplicate Tushare field");
    for (auto field : {"ts_code", "trade_date", "pre_close", "pre_settle", "open", "high", "low",
                       "close", "settle", "vol", "amount", "oi"})
      if (!columns.contains(field))
        throw std::invalid_argument("missing Tushare field");
    std::vector<HistoricalDailyBar> bars;
    for (const auto& row : items) {
      if (!row.is_array() || row.size() != fields.size())
        throw std::invalid_argument("invalid Tushare row");
      const auto get = [&](const char* key) -> const Json& { return row.at(columns.at(key)); };
      if (text(get("ts_code")) != ts_code)
        throw std::invalid_argument("Tushare returned another contract");
      const auto optional_price = [&](const char* key) -> std::optional<Decimal> {
        return get(key).is_null() ? std::nullopt : std::optional(decimal(get(key)));
      };
      HistoricalDailyBar bar{daily_date(text(get("trade_date"))),
                             decimal(get("open")),
                             decimal(get("high")),
                             decimal(get("low")),
                             decimal(get("close")),
                             decimal(get("vol")),
                             decimal(get("amount"), 4),
                             decimal(get("oi")),
                             optional_price("pre_close"),
                             optional_price("pre_settle"),
                             optional_price("settle")};
      bar.validate();
      if (bar.trading_day < range.begin || bar.trading_day > range.end ||
          bar.volume.raw() % 100000000 || bar.open_interest.raw() % 100000000)
        throw std::invalid_argument("Tushare daily bar outside interval or invalid quantity");
      bars.push_back(std::move(bar));
    }
    std::ranges::sort(bars, {}, &HistoricalDailyBar::trading_day);
    for (std::size_t i = 1; i < bars.size(); ++i)
      if (bars[i].trading_day == bars[i - 1].trading_day)
        throw std::invalid_argument("duplicate Tushare trading date");
    return bars;
  } catch (const nlohmann::json::exception&) {
    throw std::runtime_error("invalid Tushare response schema");
  }
}
} // namespace asterion::tushare
