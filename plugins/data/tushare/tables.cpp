#include "tables.hpp"
#include "wire.hpp"
#include <asterion/kernel/logger.hpp>
#include <algorithm>
#include <cctype>
namespace asterion::tushare {
namespace {
constexpr TableDefinition definitions[] = {
    {"futures.contracts", "tushare.fut_basic.v1", 1},
    {"futures.calendar", "tushare.trade_cal.v1", 1},
    {"futures.daily", "tushare.fut_daily.v3", 1},
};
// A row before it is put in the order the host asked for.
using Named = std::vector<std::pair<std::string_view, std::optional<std::string>>>;
TableRow ordered(const Named& row, std::span<const std::string> columns) {
  TableRow cells;
  cells.reserve(columns.size());
  for (const auto& column : columns) {
    const auto found = std::ranges::find(row, std::string_view(column),
                                         [](const auto& item) { return item.first; });
    if (found == row.end())
      throw std::logic_error("Tushare table has no such column");
    cells.push_back(found->second);
  }
  return cells;
}
const char* suffix(const std::string& venue) {
  static const std::map<std::string, const char*> suffixes{{"SHFE", ".SHF"}, {"DCE", ".DCE"},
                                                           {"CZCE", ".ZCE"}, {"CFFEX", ".CFX"},
                                                           {"INE", ".INE"},  {"GFEX", ".GFE"}};
  const auto found = suffixes.find(venue);
  if (found == suffixes.end())
    throw std::invalid_argument("unsupported Tushare futures venue");
  return found->second;
}
// Whether a vendor code names a contract the contract identity covers: one to
// three letters and a delivery month. Other codes under an exchange are not
// stored. They are the vendor's own main and continuous series (RB.SHF,
// RBL.SHF), and contracts the identity cannot express yet, such as the monthly
// average price futures of DCE (L2605F.DCE).
bool dated(std::string_view code, std::string_view suffix) {
  if (!code.ends_with(suffix))
    return false;
  code.remove_suffix(suffix.size());
  const auto month = code.find_first_of("0123456789");
  return month >= 1 && month <= 3 && code.size() - month >= 3 && code.size() - month <= 4 &&
         std::ranges::all_of(code.substr(0, month), [](char c) { return c >= 'A' && c <= 'Z'; }) &&
         code.find_first_not_of("0123456789", month) == std::string_view::npos;
}
// YYYYMMDD as the vendor writes dates.
std::string compact(std::string day) {
  std::erase(day, '-');
  return day;
}
std::string dashed(const std::string& value) {
  if (value.size() != 8 || value.find_first_not_of("0123456789") != std::string::npos)
    throw std::invalid_argument("is not a YYYYMMDD date");
  return value.substr(0, 4) + "-" + value.substr(4, 2) + "-" + value.substr(6, 2);
}
// Free text as published, on one line.
std::optional<std::string> label(const Json& value) {
  if (value.is_null())
    return std::nullopt;
  auto result = text(value);
  for (auto& c : result)
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f)
      c = ' ';
  const auto first = result.find_first_not_of(' ');
  if (first == std::string::npos)
    return std::nullopt;
  return result.substr(first, result.find_last_not_of(' ') - first + 1);
}
std::optional<std::string> date(const Json& value) {
  if (value.is_null() || (value.is_string() && value.get_ref<const std::string&>().empty()))
    return std::nullopt;
  return dashed(text(value));
}
std::optional<std::string> number(const Json& value, int shift = 0) {
  return value.is_null() ? std::nullopt : std::optional(plain_decimal(value, shift));
}
const char* refusal(AccessFailure reason) {
  switch (reason) {
  case AccessFailure::invalid_credential:
    return "Tushare did not accept the token: ";
  case AccessFailure::permission:
    return "the Tushare account has no permission for this interface: ";
  case AccessFailure::rate_limit:
    return "the Tushare account reached its request limit: ";
  default:
    return "Tushare reported an error: ";
  }
}
} // namespace
std::span<const TableDefinition> table_definitions() {
  return definitions;
}
struct Tables::Listing {
  std::string code, contract, list_date, delist_date;
  Named row;
};
// One vendor table: where each field is, and the rows.
struct Tables::Answer {
  Json root;
  std::map<std::string, std::size_t> fields;
  const Json& rows() const { return root.at("data").at("items"); }
  const Json& at(const Json& row, const char* field) const { return row.at(fields.at(field)); }
};
Tables::Tables(std::string token, Post post) : token_(std::move(token)), post_(std::move(post)) {
  if (token_.empty() || token_.size() > 256 || token_.find_first_of("\r\n\t ") != std::string::npos)
    throw std::invalid_argument("Tushare token is required without whitespace");
}
Tables::~Tables() = default;
Tables::Answer Tables::ask(const char* interface, Json parameters, const char* fields,
                           std::size_t limit, const std::string& partition, std::stop_token stop) {
  const Json request{{"api_name", interface},
                     {"token", token_},
                     {"params", std::move(parameters)},
                     {"fields", fields}};
  if (stop.stop_requested())
    throw std::runtime_error("Tushare download cancelled");
  const auto raw = post_(request.dump(), stop);
  if (stop.stop_requested())
    throw std::runtime_error("Tushare download cancelled");
  Answer answer;
  try {
    answer.root = parse_response(raw);
    // The vendor's own message can repeat the token: it is never passed on.
    if (text(answer.root.at("code")) != "0") {
      const auto reason = rejection(answer.root);
      throw RequestError(reason, refusal(reason) + std::string(interface));
    }
    const auto& names = answer.root.at("data").at("fields");
    if (!names.is_array() || !answer.rows().is_array())
      throw std::invalid_argument("not a table");
    for (std::size_t i = 0; i < names.size(); ++i)
      if (!answer.fields.emplace(text(names[i]), i).second)
        throw std::invalid_argument("a repeated field");
    for (std::string_view rest = fields; !rest.empty();) {
      const auto comma = rest.find(',');
      if (!answer.fields.contains(std::string(rest.substr(0, comma))))
        throw std::invalid_argument("a missing field");
      rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
    }
    for (const auto& row : answer.rows())
      if (!row.is_array() || row.size() != names.size())
        throw std::invalid_argument("a row of another width");
  } catch (const RequestError&) {
    throw;
  } catch (const std::exception&) {
    throw TableError(partition + ": the answer of " + interface + " is not the expected table");
  }
  // The vendor stops at its limit without saying so.
  if (answer.rows().size() >= limit)
    throw TableError(partition + ": " + interface + " returned its limit of " +
                     std::to_string(limit) + " rows, so the partition may be incomplete");
  return answer;
}
const std::vector<Tables::Listing>& Tables::catalogue(const std::string& venue,
                                                      std::stop_token stop) {
  if (const auto found = catalogues_.find(venue); found != catalogues_.end())
    return found->second;
  const auto partition = "futures.contracts " + venue + " all";
  const auto answer =
      ask("fut_basic", {{"exchange", venue}, {"fut_type", "1"}},
          "ts_code,exchange,name,fut_code,multiplier,trade_unit,per_unit,quote_unit,"
          "quote_unit_desc,d_mode_desc,list_date,delist_date,d_month,last_ddate,trade_time_desc",
          10000, partition, stop);
  std::vector<Listing> listings;
  std::vector<std::string> outside;
  for (const auto& row : answer.rows()) {
    std::string code;
    try {
      code = text(answer.at(row, "ts_code"));
      if (text(answer.at(row, "exchange")) != venue)
        throw std::invalid_argument("belongs to another exchange");
      if (!dated(code, suffix(venue))) {
        outside.push_back(code);
        continue;
      }
      const auto month = text(answer.at(row, "d_month"));
      auto product = text(answer.at(row, "fut_code"));
      std::ranges::transform(product, product.begin(),
                             [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
      if (month.size() != 6)
        throw std::invalid_argument("has no delivery month");
      const HistoryIdentity identity{venue, product, month.substr(0, 4) + "-" + month.substr(4)};
      mapping(identity, code);
      Listing listing{code,
                      identity.key(),
                      dashed(text(answer.at(row, "list_date"))),
                      dashed(text(answer.at(row, "delist_date"))),
                      {}};
      listing.row = {{"contract", listing.contract},
                     {"source_instrument", code},
                     {"name", label(answer.at(row, "name"))},
                     {"list_date", listing.list_date},
                     {"delist_date", listing.delist_date},
                     {"last_delivery_date", date(answer.at(row, "last_ddate"))},
                     {"multiplier", number(answer.at(row, "multiplier"))},
                     {"per_unit", number(answer.at(row, "per_unit"))},
                     {"trade_unit", label(answer.at(row, "trade_unit"))},
                     {"quote_unit", label(answer.at(row, "quote_unit"))},
                     {"tick_description", label(answer.at(row, "quote_unit_desc"))},
                     {"delivery_description", label(answer.at(row, "d_mode_desc"))},
                     {"trading_hours", label(answer.at(row, "trade_time_desc"))}};
      listings.push_back(std::move(listing));
    } catch (const std::invalid_argument& error) {
      throw TableError(partition + ": contract " + (code.empty() ? "without a code" : code) + ": " +
                       error.what());
    }
  }
  if (!outside.empty()) {
    // Left out on purpose, and said so where a missing contract is looked for.
    std::string examples;
    for (std::size_t i = 0; i < outside.size() && i < 8; ++i)
      examples += (i ? " " : "") + outside[i];
    log_process_event("tushare", LogLevel::info, "contracts.outside_identity",
                      {{"venue", venue}, {"count", outside.size()}, {"examples", examples}});
  }
  return catalogues_.emplace(venue, std::move(listings)).first->second;
}
std::vector<TableRow> Tables::read(std::string_view table, std::span<const std::string> columns,
                                   const std::string& venue, const std::string& period,
                                   std::stop_token stop) {
  const auto partition = std::string(table) + " " + venue + " " + period;
  std::vector<TableRow> rows;
  if (table == "futures.contracts") {
    for (const auto& listing : catalogue(venue, stop))
      rows.push_back(ordered(listing.row, columns));
    return rows;
  }
  if (table == "futures.calendar") {
    const auto answer =
        ask("trade_cal",
            {{"exchange", venue}, {"start_date", period + "0101"}, {"end_date", period + "1231"}},
            "exchange,cal_date,is_open,pretrade_date", 2000, partition, stop);
    for (const auto& row : answer.rows()) {
      try {
        if (text(answer.at(row, "exchange")) != venue)
          throw std::invalid_argument("belongs to another exchange");
        rows.push_back(ordered({{"day", dashed(text(answer.at(row, "cal_date")))},
                                {"open", text(answer.at(row, "is_open"))},
                                {"previous_trading_day", date(answer.at(row, "pretrade_date"))}},
                               columns));
      } catch (const std::invalid_argument& error) {
        throw TableError(partition + ": a calendar day " + error.what());
      }
    }
    return rows;
  }
  if (table != "futures.daily")
    throw std::logic_error("Tushare does not provide this table");
  const auto& listings = catalogue(venue, stop);
  const auto answer = ask("fut_daily", {{"trade_date", compact(period)}, {"exchange", venue}},
                          "ts_code,trade_date,pre_close,pre_settle,open,high,low,close,settle,vol,"
                          "amount,oi,oi_chg,delv_settle",
                          2000, partition, stop);
  for (const auto& row : answer.rows()) {
    std::string code;
    try {
      code = text(answer.at(row, "ts_code"));
      if (!code.ends_with(suffix(venue)) || text(answer.at(row, "trade_date")) != compact(period))
        throw std::invalid_argument("belongs to another exchange or day");
      if (!dated(code, suffix(venue)))
        continue;
      // A code names one contract, except where an exchange reuses it a
      // decade later: then the one listed on this day.
      const Listing* contract = nullptr;
      std::size_t matches = 0, listed = 0;
      for (const auto& listing : listings) {
        if (listing.code != code)
          continue;
        ++matches;
        if (listing.list_date <= period && period <= listing.delist_date) {
          ++listed;
          contract = &listing;
        } else if (!contract)
          contract = &listing;
      }
      if (!matches || (matches > 1 && listed != 1))
        throw std::invalid_argument(!matches ? "is not in the contract list"
                                             : "does not name one contract listed on that day");
      // A contract without a trade in continuous matching has no opening,
      // highest or lowest price that day: nobody traded it, or its volume
      // came from delivery and exchange-for-physical transfers. SHFE and INE
      // rows then carry nulls; CZCE, DCE and GFEX rows carry zeros in their
      // place. No price of these markets is zero: it is the absence of one.
      const auto traded_price = [&](const char* field) {
        auto value = number(answer.at(row, field));
        return value == "0" ? std::optional<std::string>() : value;
      };
      rows.push_back(ordered({{"contract", contract->contract},
                              {"open", traded_price("open")},
                              {"high", traded_price("high")},
                              {"low", traded_price("low")},
                              {"close", traded_price("close")},
                              {"settlement", number(answer.at(row, "settle"))},
                              {"previous_close", number(answer.at(row, "pre_close"))},
                              {"previous_settlement", number(answer.at(row, "pre_settle"))},
                              {"volume", number(answer.at(row, "vol"))},
                              // Published in units of ten thousand.
                              {"amount", number(answer.at(row, "amount"), 4)},
                              {"open_interest", number(answer.at(row, "oi"))},
                              {"open_interest_change", number(answer.at(row, "oi_chg"))},
                              {"delivery_settlement", number(answer.at(row, "delv_settle"))}},
                             columns));
    } catch (const std::invalid_argument& error) {
      throw TableError(partition + ": " + (code.empty() ? "a row without a code" : code) + " " +
                       error.what());
    }
  }
  return rows;
}
} // namespace asterion::tushare
