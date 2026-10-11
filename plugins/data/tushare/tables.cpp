#include "tables.hpp"
#include "wire.hpp"
#include <asterion/domain/history_tables.hpp>
#include <asterion/kernel/logger.hpp>
#include <algorithm>
#include <cctype>
#include <ranges>
namespace asterion::tushare {
namespace {
constexpr const char* every_exchange[] = {"SHFE", "DCE", "CZCE", "CFFEX", "INE", "GFEX"};
// CFFEX has no physical delivery by warehouse receipt.
constexpr const char* receipt_exchanges[] = {"SHFE", "DCE", "CZCE", "INE", "GFEX"};
constexpr TableDefinition definitions[] = {
    {"futures.contracts", "tushare.fut_basic.v1", 1, every_exchange},
    {"futures.calendar", "tushare.trade_cal.v1", 1, every_exchange},
    {"futures.daily", "tushare.fut_daily.v3", 1, every_exchange},
    {"futures.settlement", "tushare.fut_settle.v2", 1, every_exchange},
    {"futures.receipts", "tushare.fut_wsr.v4", 1, receipt_exchanges},
    {"futures.rankings", "tushare.fut_holding.v2", 1, every_exchange},
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
std::string lower(std::string value) {
  std::ranges::transform(value, value.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}
// The product a vendor's letters name. Reports of CZCE call TA by its name.
std::string product_of(const std::string& venue, const std::string& letters) {
  auto product = lower(letters);
  return venue == "CZCE" && product == "pta" ? "ta" : product;
}
// What CZCE itself calls a contract: the letters and three digits of a dated
// code, without the decade the vendor adds.
std::string czce_code(std::string_view code) {
  code.remove_suffix(std::string_view(".ZCE").size());
  return std::string(code.substr(0, code.find_first_of("0123456789"))) +
         std::string(code.substr(code.size() - 3));
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
  std::string code, contract, product, list_date, delist_date;
  Named row;
};
// One vendor table: where each field is, and the rows.
struct Tables::Answer {
  Json root;
  std::map<std::string, std::size_t> fields;
  const Json& rows() const { return root.at("data").at("items"); }
  const Json& at(const Json& row, const char* field) const { return row.at(fields.at(field)); }
};
struct Tables::Shared {
  std::string period;
  Answer answer;
};
Tables::Tables(std::string token, Post post) : token_(std::move(token)), post_(std::move(post)) {
  if (token_.empty() || token_.size() > 256 || token_.find_first_of("\r\n\t ") != std::string::npos)
    throw std::invalid_argument("Tushare token is required without whitespace");
}
Tables::~Tables() = default;
Tables::Answer Tables::request(const char* interface, const Json& parameters, const char* fields,
                               const std::string& partition, std::stop_token stop) {
  const Json request{
      {"api_name", interface}, {"token", token_}, {"params", parameters}, {"fields", fields}};
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
  return answer;
}
Tables::Answer Tables::ask(const char* interface, const Json& parameters, const char* fields,
                           std::size_t limit, const std::string& partition, std::stop_token stop) {
  auto answer = request(interface, parameters, fields, partition, stop);
  // The vendor stops at its limit without saying so.
  if (answer.rows().size() >= limit)
    throw TableError(partition + ": " + interface + " returned its limit of " +
                     std::to_string(limit) + " rows, so the partition may be incomplete");
  return answer;
}
Tables::Answer Tables::pages(const char* interface, Json parameters, const char* fields,
                             std::size_t size, const std::string& partition, std::stop_token stop) {
  // A full page means there may be more; the first that is not full ends them.
  parameters["limit"] = size;
  parameters["offset"] = 0;
  auto answer = request(interface, parameters, fields, partition, stop);
  for (auto last = answer.rows().size(); last == size;) {
    if (answer.rows().size() > max_table_partition_rows)
      throw TableError(partition + ": " + interface + " has more rows than a partition holds");
    parameters["offset"] = answer.rows().size();
    auto page = request(interface, parameters, fields, partition, stop);
    if (page.fields != answer.fields)
      throw TableError(partition + ": the answer of " + interface + " is not the expected table");
    last = page.rows().size();
    auto& rows = answer.root.at("data").at("items");
    for (auto& row : page.root.at("data").at("items"))
      rows.push_back(std::move(row));
  }
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
      const auto product = lower(text(answer.at(row, "fut_code")));
      if (month.size() != 6)
        throw std::invalid_argument("has no delivery month");
      const HistoryIdentity identity{venue, product, month.substr(0, 4) + "-" + month.substr(4)};
      mapping(identity, code);
      Listing listing{code,
                      identity.key(),
                      product,
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
const Tables::Listing& Tables::listed(const std::string& venue, const std::string& code,
                                      const std::string& period, std::stop_token stop) {
  const auto& listings = catalogue(venue, stop);
  const auto live = [&](const Listing& listing) {
    return listing.list_date <= period && period <= listing.delist_date;
  };
  const auto exact = std::ranges::find(listings, code, &Listing::code);
  if (venue == "CZCE" && (exact == listings.end() || !live(*exact))) {
    // CZCE itself writes the year as one digit, so its codes repeat every ten
    // years, and the decade the vendor adds is not always the right one
    // (AP1003 for the AP003 that was listed in 2019). The letters and the last
    // three digits then name the contract listed on that day.
    const auto own = czce_code(code);
    const auto meant = std::ranges::find_if(listings, [&](const Listing& listing) {
      return live(listing) && czce_code(listing.code) == own;
    });
    if (meant != listings.end())
      return *meant;
  }
  if (exact == listings.end())
    throw std::invalid_argument("is not in the contract list");
  return *exact;
}
std::vector<TableRow> Tables::read(std::string_view table, Columns columns,
                                   const std::string& venue, const std::string& period,
                                   std::stop_token stop) {
  if (table == "futures.contracts") {
    std::vector<TableRow> rows;
    for (const auto& listing : catalogue(venue, stop))
      rows.push_back(ordered(listing.row, columns));
    return rows;
  }
  if (table == "futures.calendar")
    return calendar(columns, venue, period, stop);
  if (table == "futures.daily")
    return daily(columns, venue, period, stop);
  if (table == "futures.settlement")
    return settlement(columns, venue, period, stop);
  if (table == "futures.receipts")
    return receipts(columns, venue, period, stop);
  if (table == "futures.rankings")
    return rankings(columns, venue, period, stop);
  throw std::logic_error("Tushare does not provide this table");
}
std::vector<TableRow> Tables::calendar(Columns columns, const std::string& venue,
                                       const std::string& period, std::stop_token stop) {
  const auto partition = "futures.calendar " + venue + " " + period;
  const auto answer =
      ask("trade_cal",
          {{"exchange", venue}, {"start_date", period + "0101"}, {"end_date", period + "1231"}},
          "exchange,cal_date,is_open,pretrade_date", 2000, partition, stop);
  std::vector<TableRow> rows;
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
std::vector<TableRow> Tables::daily(Columns columns, const std::string& venue,
                                    const std::string& period, std::stop_token stop) {
  const auto partition = "futures.daily " + venue + " " + period;
  (void)catalogue(venue, stop);
  const auto answer = ask("fut_daily", {{"trade_date", compact(period)}, {"exchange", venue}},
                          "ts_code,trade_date,pre_close,pre_settle,open,high,low,close,settle,vol,"
                          "amount,oi,oi_chg,delv_settle",
                          2000, partition, stop);
  std::vector<TableRow> rows;
  for (const auto& row : answer.rows()) {
    std::string code;
    try {
      code = text(answer.at(row, "ts_code"));
      if (!code.ends_with(suffix(venue)) || text(answer.at(row, "trade_date")) != compact(period))
        throw std::invalid_argument("belongs to another exchange or day");
      if (!dated(code, suffix(venue)))
        continue;
      const auto& contract = listed(venue, code, period, stop);
      // A contract without a trade in continuous matching has no opening,
      // highest or lowest price that day: nobody traded it, or its volume
      // came from delivery and exchange-for-physical transfers. SHFE and INE
      // rows then carry nulls; CZCE, DCE and GFEX rows carry zeros in their
      // place. No price of these markets is zero: it is the absence of one.
      const auto traded_price = [&](const char* field) {
        auto value = number(answer.at(row, field));
        return value == "0" ? std::optional<std::string>() : value;
      };
      rows.push_back(ordered({{"contract", contract.contract},
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
std::vector<TableRow> Tables::settlement(Columns columns, const std::string& venue,
                                         const std::string& period, std::stop_token stop) {
  const auto partition = "futures.settlement " + venue + " " + period;
  (void)catalogue(venue, stop);
  // The settlement price of the same answer is the one futures.daily holds.
  const auto answer =
      ask("fut_settle", {{"trade_date", compact(period)}, {"exchange", venue}},
          "ts_code,trade_date,trading_fee_rate,trading_fee,delivery_fee,b_hedging_margin_rate,"
          "s_hedging_margin_rate,long_margin_rate,short_margin_rate,offset_today_fee",
          1600, partition, stop);
  std::vector<TableRow> rows;
  std::size_t unlisted = 0;
  for (const auto& row : answer.rows()) {
    std::string code;
    try {
      code = text(answer.at(row, "ts_code"));
      if (!code.ends_with(suffix(venue)) || text(answer.at(row, "trade_date")) != compact(period))
        throw std::invalid_argument("belongs to another exchange or day");
      if (!dated(code, suffix(venue)))
        continue;
      const auto& contract = listed(venue, code, period, stop);
      // The answer for CFFEX goes on repeating a contract's parameters for
      // months after its last day. A day has those of the contracts it lists.
      if (period < contract.list_date || contract.delist_date < period) {
        ++unlisted;
        continue;
      }
      rows.push_back(
          ordered({{"contract", contract.contract},
                   {"trading_fee_rate", number(answer.at(row, "trading_fee_rate"))},
                   {"trading_fee", number(answer.at(row, "trading_fee"))},
                   {"close_today_fee", number(answer.at(row, "offset_today_fee"))},
                   {"delivery_fee", number(answer.at(row, "delivery_fee"))},
                   {"long_margin_rate", number(answer.at(row, "long_margin_rate"))},
                   {"short_margin_rate", number(answer.at(row, "short_margin_rate"))},
                   {"buy_hedging_margin_rate", number(answer.at(row, "b_hedging_margin_rate"))},
                   {"sell_hedging_margin_rate", number(answer.at(row, "s_hedging_margin_rate"))}},
                  columns));
    } catch (const std::invalid_argument& error) {
      throw TableError(partition + ": " + (code.empty() ? "a row without a code" : code) + " " +
                       error.what());
    }
  }
  if (unlisted && reported_.insert("settlement " + venue).second)
    log_process_event("tushare", LogLevel::info, "settlement.unlisted_contracts",
                      {{"venue", venue}, {"day", period}, {"count", unlisted}});
  return rows;
}
bool Tables::lists(const std::string& venue, const std::string& product, std::stop_token stop) {
  return std::ranges::any_of(catalogue(venue, stop),
                             [&](const Listing& listing) { return listing.product == product; });
}
const Tables::Answer& Tables::shanghai(const char* interface, const char* fields, std::size_t size,
                                       const std::string& period, const std::string& partition,
                                       std::stop_token stop) {
  auto& shared = shanghai_[interface];
  if (!shared || shared->period != period)
    shared = std::make_unique<Shared>(
        Shared{period, pages(interface, {{"trade_date", compact(period)}, {"exchange", "SHFE"}},
                             fields, size, partition, stop)});
  return shared->answer;
}
std::vector<TableRow> Tables::receipts(Columns columns, const std::string& venue,
                                       const std::string& period, std::stop_token stop) {
  const auto partition = "futures.receipts " + venue + " " + period;
  constexpr auto fields = "trade_date,symbol,fut_name,warehouse,wh_id,pre_vol,vol,vol_chg,area,"
                          "year,grade,brand,place,pd,is_ct,unit";
  const bool family = venue == "SHFE" || venue == "INE";
  Answer own;
  if (!family)
    own = pages("fut_wsr", {{"trade_date", compact(period)}, {"exchange", venue}}, fields, 1000,
                partition, stop);
  const auto& answer = family ? shanghai("fut_wsr", fields, 1000, period, partition, stop) : own;
  std::vector<TableRow> rows;
  std::map<std::string, unsigned> lines; // of each product so far
  for (const auto& row : answer.rows()) {
    std::string symbol;
    try {
      symbol = text(answer.at(row, "symbol"));
      if (text(answer.at(row, "trade_date")) != compact(period))
        throw std::invalid_argument("belongs to another day");
      const auto name = label(answer.at(row, "fut_name"));
      // International copper is an INE product; the Shanghai report lists it
      // under copper, set apart by its name alone.
      const auto product =
          family && symbol == "CU" && name == "铜(BC)" ? "bc" : product_of(venue, symbol);
      if (!lists(venue, product, stop)) {
        if (family && lists(venue == "SHFE" ? "INE" : "SHFE", product, stop))
          continue;
        throw std::invalid_argument("is not a product of this exchange");
      }
      rows.push_back(ordered({{"product", product},
                              {"line", std::to_string(++lines[product])},
                              {"product_name", name},
                              {"warehouse", label(answer.at(row, "warehouse"))},
                              {"warehouse_id", label(answer.at(row, "wh_id"))},
                              {"area", label(answer.at(row, "area"))},
                              {"year", label(answer.at(row, "year"))},
                              {"grade", label(answer.at(row, "grade"))},
                              {"brand", label(answer.at(row, "brand"))},
                              {"place", label(answer.at(row, "place"))},
                              {"converted", label(answer.at(row, "is_ct"))},
                              {"previous_volume", number(answer.at(row, "pre_vol"))},
                              {"volume", number(answer.at(row, "vol"))},
                              {"change", number(answer.at(row, "vol_chg"))},
                              {"unit", label(answer.at(row, "unit"))},
                              {"premium", number(answer.at(row, "pd"))}},
                             columns));
    } catch (const std::invalid_argument& error) {
      throw TableError(partition + ": " + (symbol.empty() ? "a row without a product" : symbol) +
                       " " + error.what());
    }
  }
  return rows;
}
std::vector<TableRow> Tables::rankings(Columns columns, const std::string& venue,
                                       const std::string& period, std::stop_token stop) {
  const auto partition = "futures.rankings " + venue + " " + period;
  constexpr auto fields =
      "trade_date,symbol,broker,vol,vol_chg,long_hld,long_chg,short_hld,short_chg";
  const bool family = venue == "SHFE" || venue == "INE";
  Answer own;
  if (!family)
    own = pages("fut_holding", {{"trade_date", compact(period)}, {"exchange", venue}}, fields, 2000,
                partition, stop);
  const auto& answer =
      family ? shanghai("fut_holding", fields, 2000, period, partition, stop) : own;
  std::vector<TableRow> rows;
  std::set<std::string> outside, unknown;
  for (const auto& row : answer.rows()) {
    std::string symbol;
    try {
      symbol = text(answer.at(row, "symbol"));
      if (text(answer.at(row, "trade_date")) != compact(period))
        throw std::invalid_argument("belongs to another day");
      // Exchanges rank members in a contract, and some in a whole product
      // too. CZCE used to name a product in Chinese before its letters
      // (尿素UR), SHFE used to rank a product over its active contracts
      // (CUACTV), and the vendor writes the codes of some exchanges in lower
      // case.
      std::string code(std::ranges::find_if(symbol, [](unsigned char c) { return c < 0x80; }),
                       symbol.end());
      std::ranges::transform(code, code.begin(),
                             [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
      if (code.ends_with("ACTV"))
        code.erase(code.size() - 4);
      const auto month = code.find_first_of("0123456789");
      const auto product = product_of(venue, code.substr(0, month));
      const bool whole = month == std::string::npos;
      if (product.empty() || product.size() > 3 ||
          (!whole && !dated(code + suffix(venue), suffix(venue)))) {
        unknown.insert(symbol);
        continue;
      }
      // Rankings of what is not a future here are not kept: options, and for
      // the two Shanghai exchanges the products of the other.
      if (!lists(venue, product, stop)) {
        outside.insert(code.substr(0, month));
        continue;
      }
      rows.push_back(ordered(
          {{"instrument", whole ? venue + "/" + product
                                : listed(venue, code + suffix(venue), period, stop).contract},
           {"member", label(answer.at(row, "broker"))},
           {"volume", number(answer.at(row, "vol"))},
           {"volume_change", number(answer.at(row, "vol_chg"))},
           {"long", number(answer.at(row, "long_hld"))},
           {"long_change", number(answer.at(row, "long_chg"))},
           {"short", number(answer.at(row, "short_hld"))},
           {"short_change", number(answer.at(row, "short_chg"))}},
          columns));
    } catch (const std::invalid_argument& error) {
      throw TableError(partition + ": " + (symbol.empty() ? "a row without a symbol" : symbol) +
                       " " + error.what());
    }
  }
  // The unknown notations together: each is a rule this reader does not have.
  if (!unknown.empty()) {
    constexpr std::size_t shown = 8;
    std::string symbols;
    for (const auto& item : unknown | std::views::take(shown))
      symbols += (symbols.empty() ? "" : " ") + item;
    if (unknown.size() > shown)
      symbols += " and " + std::to_string(unknown.size() - shown) + " more";
    throw TableError(partition + ": " + symbols + " name neither a contract nor a product");
  }
  if (!outside.empty() && reported_.insert("rankings " + venue).second) {
    std::string products;
    for (const auto& item : outside)
      products += (products.empty() ? "" : " ") + item;
    log_process_event("tushare", LogLevel::info, "rankings.outside_products",
                      {{"venue", venue}, {"products", products}});
  }
  return rows;
}
} // namespace asterion::tushare
