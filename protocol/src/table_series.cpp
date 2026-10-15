#include <asterion/domain/history_identity.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/protocol/data.hpp>
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
namespace asterion::protocol {
namespace {
bool digest(const std::string& text) {
  return text.size() == 64 && text.find_first_not_of("0123456789abcdef") == std::string::npos;
}
// What a series was asked of: the same in a request and in the fixed series.
template <class Message> void validate_selection(const Message& message) {
  validate_history_source(message.daily_source());
  validate_history_source(message.calendar_source());
  (void)parse_trading_date(message.begin_day());
  (void)parse_trading_date(message.end_day());
  if (!digest(message.snapshot()) || message.begin_day() > message.end_day())
    throw std::invalid_argument("invalid table series request");
}
constexpr std::pair<data::v1::DailyPeriod, std::string_view> periods[] = {
    {data::v1::DAY, "day"},
    {data::v1::WEEK, "week"},
    {data::v1::MONTH, "month"},
    {data::v1::QUARTER, "quarter"},
    {data::v1::YEAR, "year"}};
// A contract's days, and so its candles: at most twenty years of them.
constexpr std::uint64_t max_table_candles = 20 * 366 + 1;
} // namespace
TableInstrument table_instrument(std::string_view text) {
  try {
    const auto first = text.find('/');
    if (first != std::string_view::npos && first == text.rfind('/')) {
      TableInstrument product{
          std::string(text.substr(0, first)), std::string(text.substr(first + 1)), {}};
      // A product is named as a contract is, without the delivery month.
      HistoryIdentity{product.venue, product.product, "2000-01"}.validate();
      return product;
    }
    const auto contract = HistoryIdentity::parse(text);
    return {contract.venue, contract.product, contract.delivery_month};
  } catch (const std::invalid_argument&) {
    throw std::invalid_argument("invalid table instrument");
  }
}
void validate_daily_table(const data::v1::DailyTable& table) {
  validate_message(table);
  validate_history_source(table.daily_source());
  validate_history_source(table.calendar_source());
  validate_history_source(table.contracts_source());
  if (!digest(table.snapshot()))
    throw std::invalid_argument("invalid daily table reference");
}
data::v1::DailyTable encode_daily_table(const Json& value) {
  require_fields(value, {"snapshot", "daily_source", "calendar_source", "contracts_source"});
  data::v1::DailyTable table;
  table.set_snapshot(value.at("snapshot").get<std::string>());
  table.set_daily_source(value.at("daily_source").get<std::string>());
  table.set_calendar_source(value.at("calendar_source").get<std::string>());
  table.set_contracts_source(value.at("contracts_source").get<std::string>());
  validate_daily_table(table);
  return table;
}
Json decode_daily_table(const data::v1::DailyTable& table) {
  validate_daily_table(table);
  return {{"snapshot", table.snapshot()},
          {"daily_source", table.daily_source()},
          {"calendar_source", table.calendar_source()},
          {"contracts_source", table.contracts_source()}};
}
void validate_table_series_request(const data::v1::TableSeriesRequest& request) {
  validate_message(request);
  validate_selection(request);
  if (request.instruments().empty() || request.instruments_size() > max_table_instruments)
    throw std::invalid_argument("invalid table series request");
  std::set<std::string_view> seen;
  for (const auto& instrument : request.instruments()) {
    (void)table_instrument(instrument);
    if (!seen.insert(instrument).second)
      throw std::invalid_argument("invalid table series request");
  }
}
data::v1::TableSeriesRequest encode_table_series_request(const Json& value) {
  require_fields(value, {"snapshot", "daily_source", "calendar_source", "begin_day", "end_day",
                         "instruments"});
  if (!value.at("instruments").is_array())
    throw std::invalid_argument("invalid table series request");
  data::v1::TableSeriesRequest request;
  request.set_snapshot(value.at("snapshot").get<std::string>());
  request.set_daily_source(value.at("daily_source").get<std::string>());
  request.set_calendar_source(value.at("calendar_source").get<std::string>());
  request.set_begin_day(value.at("begin_day").get<std::string>());
  request.set_end_day(value.at("end_day").get<std::string>());
  for (const auto& instrument : value.at("instruments"))
    request.add_instruments(instrument.get<std::string>());
  validate_table_series_request(request);
  return request;
}
std::vector<v1::Contract> table_series_months(const data::v1::TableSeries& series) {
  std::vector<v1::Contract> months;
  for (const auto& month : series.months()) {
    const auto identity = HistoryIdentity::parse(month.contract());
    auto& contract = months.emplace_back();
    contract.set_venue(identity.venue);
    contract.set_product(identity.product);
    contract.set_delivery_month(identity.delivery_month);
  }
  return months;
}
void validate_table_series(const data::v1::TableSeries& series) {
  validate_message(series);
  validate_selection(series);
  const auto reject = [] { throw std::invalid_argument("invalid table series"); };
  if (series.version() != 2)
    throw std::invalid_argument("unsupported table series version");
  if (series.acquired_at_ns() <= 0 || series.months().empty())
    reject();
  const auto instrument = table_instrument(series.instrument());
  const auto months = table_series_months(series);
  const bool product = instrument.delivery_month.empty();
  if (!product &&
      (series.months_size() != 1 || series.months(0).contract() != series.instrument() ||
       series.has_schedule() || series.months(0).closes().empty()))
    reject();
  if (product) {
    // The months that are dominant at some point, in roll order.
    std::vector<const v1::Contract*> contracts;
    for (const auto& month : months) {
      if (month.venue() != instrument.venue || month.product() != instrument.product)
        reject();
      contracts.push_back(&month);
    }
    validate_dominant_schedule(series.schedule(), contracts);
    if (series.schedule().rolls_size() != series.months_size())
      reject();
    for (int i = 0; i < series.months_size(); ++i)
      if (series.schedule().rolls(i).contract() != static_cast<unsigned>(i))
        reject();
  }
  for (int m = 0; m < series.months_size(); ++m) {
    // A month is read from the day it becomes dominant to the day before the next does.
    const auto first = product ? series.schedule().rolls(m).trading_day() : series.begin_day();
    const auto* previous = &first;
    bool any = false;
    for (const auto& close : series.months(m).closes()) {
      (void)parse_trading_date(close.trading_day());
      if ((close.has_volume_contracts() && close.volume_contracts() < 0) ||
          (close.has_open_interest_contracts() && close.open_interest_contracts() < 0) ||
          close.close().units() <= 0 || close.trading_day() > series.end_day() ||
          (any ? close.trading_day() <= *previous : close.trading_day() < first) ||
          (product && m + 1 < series.months_size() &&
           close.trading_day() >= series.schedule().rolls(m + 1).trading_day()))
        reject();
      previous = &close.trading_day();
      any = true;
    }
  }
}
std::string table_series_revision(const data::v1::TableSeries& series) {
  validate_table_series(series);
  Json months = Json::array();
  for (const auto& month : series.months()) {
    Json closes = Json::array();
    for (const auto& close : month.closes())
      closes.push_back(
          {close.trading_day(), Decimal::from_raw(close.close().units()).str(),
           close.has_volume_contracts() ? Json(close.volume_contracts()) : Json(),
           close.has_open_interest_contracts() ? Json(close.open_interest_contracts()) : Json()});
    months.push_back({{"contract", month.contract()}, {"closes", std::move(closes)}});
  }
  // Content only: where and when it was read from is evidence, not identity.
  return sha256_bytes(
      Json{{"version", 2},
           {"kind", "futures.table-series"},
           {"instrument", series.instrument()},
           {"months", std::move(months)},
           {"rolls", series.has_schedule() ? decode_dominant_schedule(series.schedule()) : Json()},
           {"terms", series.has_schedule() ? decode_term_points(series.schedule()) : Json()}}
          .dump());
}
Json decode_table_series(const data::v1::TableSeries& series) {
  validate_table_series(series);
  Json months = Json::array();
  std::size_t count = 0;
  for (const auto& month : series.months()) {
    const auto size = month.closes_size();
    count += static_cast<std::size_t>(size);
    months.push_back({{"contract", month.contract()},
                      {"count", size},
                      {"first_day", size ? Json(month.closes(0).trading_day()) : Json()},
                      {"last_day", size ? Json(month.closes(size - 1).trading_day()) : Json()}});
  }
  return {{"instrument", series.instrument()},
          {"snapshot", series.snapshot()},
          {"daily_source", series.daily_source()},
          {"calendar_source", series.calendar_source()},
          {"begin_day", series.begin_day()},
          {"end_day", series.end_day()},
          {"acquired_at_ns", std::to_string(series.acquired_at_ns())},
          {"months", std::move(months)},
          {"rolls",
           series.has_schedule() ? decode_dominant_schedule(series.schedule()) : Json::array()},
          {"terms", series.has_schedule() ? series.schedule().terms_size() : 0},
          {"count", count}};
}
data::v1::TableProductsQuery encode_table_products_query(const Json& value) {
  require_fields(value, {"snapshot", "contracts_source"});
  data::v1::TableProductsQuery query;
  query.set_snapshot(value.at("snapshot").get<std::string>());
  query.set_contracts_source(value.at("contracts_source").get<std::string>());
  validate_history_source(query.contracts_source());
  if (!digest(query.snapshot()))
    throw std::invalid_argument("invalid table products query");
  return query;
}
Json decode_table_products(const data::v1::TableProducts& products) {
  validate_message(products);
  Json rows = Json::array();
  for (const auto& product : products.products()) {
    (void)table_instrument(product.exchange() + "/" + product.product());
    (void)parse_trading_date(product.first_listed());
    (void)parse_trading_date(product.last_delisted());
    if (!product.contracts())
      throw std::invalid_argument("invalid table products");
    rows.push_back({{"exchange", product.exchange()},
                    {"product", product.product()},
                    {"contracts", product.contracts()},
                    {"first_listed", product.first_listed()},
                    {"last_delisted", product.last_delisted()}});
  }
  return rows;
}
void validate_table_listed_contracts_query(const data::v1::TableListedContractsQuery& query) {
  validate_message(query);
  validate_history_source(query.contracts_source());
  const auto product = table_instrument(query.exchange() + "/" + query.product());
  if (!digest(query.snapshot()) || !product.delivery_month.empty())
    throw std::invalid_argument("invalid listed contract query");
}
data::v1::TableListedContractsQuery encode_table_listed_contracts_query(const Json& value) {
  require_fields(value, {"snapshot", "contracts_source", "exchange", "product"});
  data::v1::TableListedContractsQuery query;
  query.set_snapshot(value.at("snapshot").get<std::string>());
  query.set_contracts_source(value.at("contracts_source").get<std::string>());
  query.set_exchange(value.at("exchange").get<std::string>());
  query.set_product(value.at("product").get<std::string>());
  validate_table_listed_contracts_query(query);
  return query;
}
Json decode_table_listed_contracts(const data::v1::TableListedContracts& catalog) {
  validate_message(catalog);
  const auto& query = catalog.query();
  validate_table_listed_contracts_query(query);
  Json items = Json::array();
  for (const auto& item : catalog.items()) {
    const auto identity = HistoryIdentity::parse(item.contract_id());
    (void)parse_trading_date(item.list_date());
    (void)parse_trading_date(item.delist_date());
    if (identity.venue != query.exchange() || identity.product != query.product() ||
        item.list_date() > item.delist_date())
      throw std::invalid_argument("invalid listed contract catalog");
    items.push_back({{"contract_id", item.contract_id()},
                     {"name", item.name()},
                     {"list_date", item.list_date()},
                     {"delist_date", item.delist_date()}});
  }
  return {{"query",
           {{"snapshot", query.snapshot()},
            {"contracts_source", query.contracts_source()},
            {"exchange", query.exchange()},
            {"product", query.product()}}},
          {"items", std::move(items)}};
}
void validate_table_daily_query(const data::v1::TableDailyQuery& query) {
  validate_message(query);
  validate_daily_table(query.daily());
  if (table_instrument(query.contract()).delivery_month.empty() ||
      !data::v1::DailyPeriod_IsValid(query.period()) || query.limit() < 1 || query.limit() > 200 ||
      query.offset() >= max_table_candles)
    throw std::invalid_argument("invalid daily table chart query");
}
data::v1::TableDailyQuery encode_table_daily_query(const Json& value) {
  require_fields(value, {"daily", "contract", "period", "offset", "limit"});
  const auto& offset = value.at("offset");
  const auto& limit = value.at("limit");
  const auto period =
      std::ranges::find(periods, value.at("period").get<std::string>(),
                        &std::pair<data::v1::DailyPeriod, std::string_view>::second);
  if (!offset.is_number_integer() || !limit.is_number_integer() || std::end(periods) == period ||
      offset < 0 || offset.get<std::int64_t>() >= static_cast<std::int64_t>(max_table_candles) ||
      limit < 1 || limit > 200)
    throw std::invalid_argument("invalid daily table chart query");
  data::v1::TableDailyQuery query;
  *query.mutable_daily() = encode_daily_table(value.at("daily"));
  query.set_contract(value.at("contract").get<std::string>());
  query.set_period(period->first);
  query.set_offset(offset.get<std::uint64_t>());
  query.set_limit(limit.get<std::uint32_t>());
  validate_table_daily_query(query);
  return query;
}
Json decode_table_daily(const data::v1::TableDailyPage& page) {
  validate_message(page);
  const auto reject = [] { throw std::invalid_argument("invalid daily table chart"); };
  const auto period = std::ranges::find(periods, page.period(),
                                        &std::pair<data::v1::DailyPeriod, std::string_view>::first);
  if (table_instrument(page.contract()).delivery_month.empty() || std::end(periods) == period ||
      page.limit() < 1 || page.limit() > 200 || page.total() > max_table_candles ||
      page.offset() > page.total() || (page.total() && page.offset() == page.total()) ||
      static_cast<std::uint64_t>(page.candles_size()) !=
          std::min<std::uint64_t>(page.limit(), page.total() - page.offset()))
    reject();
  Json candles = Json::array();
  std::string previous;
  for (const auto& row : page.candles()) {
    (void)parse_trading_date(row.trading_day());
    const auto open = Decimal::from_raw(row.open().units()),
               high = Decimal::from_raw(row.high().units()),
               low = Decimal::from_raw(row.low().units()),
               close = Decimal::from_raw(row.close().units()),
               volume = Decimal::from_raw(row.volume().units());
    if (row.trading_day() <= previous || low > high || open < low || open > high || close < low ||
        close > high || volume < Decimal{} || volume.raw() % 100000000)
      reject();
    previous = row.trading_day();
    candles.push_back({{"trading_day", row.trading_day()},
                       {"open", open.str()},
                       {"high", high.str()},
                       {"low", low.str()},
                       {"close", close.str()},
                       {"volume", volume.str()}});
    if (row.has_macd()) {
      const auto& macd = row.macd();
      if (!std::isfinite(macd.diff()) || !std::isfinite(macd.signal()) ||
          !std::isfinite(macd.histogram()))
        reject();
      candles.back()["macd"] = {
          {"diff", macd.diff()}, {"signal", macd.signal()}, {"histogram", macd.histogram()}};
    }
  }
  return {{"daily", decode_daily_table(page.daily())},
          {"contract", page.contract()},
          {"period", period->second},
          {"total", page.total()},
          {"offset", page.offset()},
          {"limit", page.limit()},
          {"candles", std::move(candles)}};
}
} // namespace asterion::protocol
