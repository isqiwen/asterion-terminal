#include <asterion/domain/history_identity.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/protocol/data.hpp>
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
void validate_table_series_request(const data::v1::TableSeriesRequest& request) {
  validate_message(request);
  validate_selection(request);
  if (request.instruments().empty() || request.instruments_size() > 20)
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
  if (series.version() != 1 || series.acquired_at_ns() <= 0 || series.months().empty())
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
      if (close.close().units() <= 0 || close.trading_day() > series.end_day() ||
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
      closes.push_back({close.trading_day(), Decimal::from_raw(close.close().units()).str()});
    months.push_back({{"contract", month.contract()}, {"closes", std::move(closes)}});
  }
  // Content only: where and when it was read from is evidence, not identity.
  return sha256_bytes(
      Json{{"version", 1},
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
} // namespace asterion::protocol
