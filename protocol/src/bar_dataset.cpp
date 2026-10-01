#include <asterion/domain/futures.hpp>
#include <asterion/foundation/id.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/protocol/data.hpp>
#include <algorithm>
#include <charconv>
#include <stdexcept>
namespace asterion::protocol {
namespace {
Decimal value(const v1::Decimal& decimal) {
  return Decimal::from_raw(decimal.units());
}
void set(v1::Decimal* target, Decimal decimal) {
  target->set_units(decimal.raw());
}
Decimal decimal(const Json& json, const char* key) {
  return Decimal::parse(json.at(key).get<std::string>());
}
std::int64_t nanoseconds(const Json& json) {
  const auto text = json.get<std::string>();
  std::int64_t result = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
  if (error != std::errc{} || end != text.data() + text.size() || std::to_string(result) != text)
    throw std::invalid_argument("invalid bar timestamp");
  return result;
}
bool digest(const std::string& text) {
  return text.size() == 64 && text.find_first_not_of("0123456789abcdef") == std::string::npos;
}
Json bar_json(const MarketBar& bar) {
  return {{"trading_day", bar.trading_day}, {"timestamp_ns", std::to_string(bar.timestamp_ns)},
          {"open", bar.open.str()},         {"high", bar.high.str()},
          {"low", bar.low.str()},           {"close", bar.close.str()},
          {"volume", bar.volume.str()}};
}
// Canonical content: the fields that define what the bars are, not where they came from.
Json contents(const data::v1::BarDataset& dataset) {
  auto bars = Json::array();
  for (const auto& bar : dataset.bars())
    bars.push_back(bar_json(market_bar(bar)));
  auto days = Json::array();
  for (const auto& day : dataset.days())
    days.push_back({{"trading_day", day.trading_day()},
                    {"settlement_price", value(day.settlement_price()).str()}});
  // Evaluated before the braced initializer (GCC < 13 PR66139 leak).
  auto contract = decode_contract(dataset.contract());
  return {{"version", 1},
          {"contract", std::move(contract)},
          {"interval_minutes", dataset.interval_minutes()},
          {"bars", std::move(bars)},
          {"days", std::move(days)}};
}
} // namespace
Instrument instrument(const v1::Contract& c) {
  if (!c.has_price_increment() || !c.has_quantity_increment() || !c.has_multiplier())
    throw std::invalid_argument("incomplete contract specification");
  Instrument result{
      {c.venue(), c.symbol()},    AssetClass::futures,           c.currency(),
      value(c.price_increment()), value(c.quantity_increment()), value(c.multiplier())};
  FuturesContract{result, c.product(), c.delivery_month()}.validate();
  return result;
}
v1::Bar encode_bar(const MarketBar& bar) {
  v1::Bar result;
  result.set_trading_day(bar.trading_day);
  result.set_timestamp_ns(bar.timestamp_ns);
  set(result.mutable_open(), bar.open);
  set(result.mutable_high(), bar.high);
  set(result.mutable_low(), bar.low);
  set(result.mutable_close(), bar.close);
  set(result.mutable_volume(), bar.volume);
  return result;
}
MarketBar market_bar(const v1::Bar& bar) {
  if (!bar.has_open() || !bar.has_high() || !bar.has_low() || !bar.has_close() || !bar.has_volume())
    throw std::invalid_argument("incomplete bar");
  return {bar.trading_day(), bar.timestamp_ns(), value(bar.open()),  value(bar.high()),
          value(bar.low()),  value(bar.close()), value(bar.volume())};
}
std::string bar_dataset_revision(const data::v1::BarDataset& dataset) {
  return sha256_bytes(contents(dataset).dump());
}
void validate_bar_dataset(const data::v1::BarDataset& dataset) {
  validate_message(dataset);
  if (dataset.version() != 1 || !dataset.has_contract() || dataset.bars_size() < 1 ||
      static_cast<std::size_t>(dataset.bars_size()) > max_dataset_bars || dataset.days_size() < 1 ||
      dataset.days_size() > dataset.bars_size())
    throw std::invalid_argument("bar dataset requires version 1, 1..200000 bars and trading days");
  const auto interval = dataset.interval_minutes();
  if (interval > 1440)
    throw std::invalid_argument("unsupported bar interval");
  if (dataset.uncovered_days_size() > 10000)
    throw std::invalid_argument("invalid uncovered trading days");
  for (int i = 0; i < dataset.uncovered_days_size(); ++i) {
    (void)parse_trading_date(dataset.uncovered_days(i));
    if (i && dataset.uncovered_days(i) <= dataset.uncovered_days(i - 1))
      throw std::invalid_argument("invalid uncovered trading days");
  }
  if (dataset.source().empty() || dataset.source().size() > 64 ||
      !digest(dataset.manifest_sha256()) || !digest(dataset.settlement_manifest_sha256()))
    throw std::invalid_argument("invalid bar dataset provenance");
  validate_id(dataset.source_task_id());
  validate_id(dataset.settlement_task_id());
  const auto spec = instrument(dataset.contract());
  const MarketBar* previous = nullptr;
  std::vector<std::string> bar_days;
  std::vector<MarketBar> bars;
  bars.reserve(static_cast<std::size_t>(dataset.bars_size()));
  for (const auto& row : dataset.bars()) {
    bars.push_back(market_bar(row));
    const auto& bar = bars.back();
    bar.validate(spec);
    if (previous &&
        (bar.timestamp_ns <= previous->timestamp_ns || bar.trading_day < previous->trading_day))
      throw std::invalid_argument("dataset bars must be strictly ascending");
    if (bar_days.empty() || bar_days.back() != bar.trading_day)
      bar_days.push_back(bar.trading_day);
    previous = &bar;
  }
  std::vector<std::string> days;
  for (const auto& day : dataset.days()) {
    (void)parse_trading_date(day.trading_day());
    const auto price = value(day.settlement_price());
    if (!day.has_settlement_price() || price <= Decimal{} ||
        !price.multiple_of(spec.price_increment) ||
        (!days.empty() && day.trading_day() <= days.back()))
      throw std::invalid_argument("invalid dataset settlement day");
    days.push_back(day.trading_day());
  }
  if (days != bar_days)
    throw std::invalid_argument("dataset days must match the trading days of its bars");
  if (dataset.revision() != bar_dataset_revision(dataset))
    throw std::invalid_argument("bar dataset revision mismatch");
}
std::vector<MarketBar> dataset_bars(const data::v1::BarDataset& dataset) {
  validate_bar_dataset(dataset);
  std::vector<MarketBar> result;
  result.reserve(static_cast<std::size_t>(dataset.bars_size()));
  for (const auto& bar : dataset.bars())
    result.push_back(market_bar(bar));
  return result;
}
std::vector<DaySettlement> dataset_days(const data::v1::BarDataset& dataset) {
  validate_bar_dataset(dataset);
  std::vector<DaySettlement> result;
  for (const auto& day : dataset.days())
    result.push_back({day.trading_day(), value(day.settlement_price())});
  return result;
}
Json decode_bar_dataset(const data::v1::BarDataset& dataset) {
  validate_bar_dataset(dataset);
  auto result = contents(dataset);
  result.update({{"revision", dataset.revision()},
                 {"source", dataset.source()},
                 {"source_task_id", dataset.source_task_id()},
                 {"settlement_task_id", dataset.settlement_task_id()},
                 {"manifest_sha256", dataset.manifest_sha256()},
                 {"settlement_manifest_sha256", dataset.settlement_manifest_sha256()}});
  return result;
}
data::v1::BarDataset encode_bar_dataset(const Json& json) {
  require_fields(json, {"version", "revision", "contract", "interval_minutes", "bars", "days",
                        "source", "source_task_id", "settlement_task_id", "manifest_sha256",
                        "settlement_manifest_sha256"});
  if (!json.at("version").is_number_integer() || json.at("version") != 1 ||
      !json.at("interval_minutes").is_number_unsigned() || !json.at("bars").is_array() ||
      !json.at("days").is_array() || json.at("bars").size() > max_dataset_bars)
    throw std::invalid_argument("unsupported bar dataset format");
  data::v1::BarDataset result;
  result.set_version(1);
  result.set_revision(json.at("revision").get<std::string>());
  *result.mutable_contract() = encode_contract(json.at("contract"));
  result.set_interval_minutes(json.at("interval_minutes").get<std::uint32_t>());
  for (const auto& row : json.at("bars")) {
    require_fields(row, {"trading_day", "timestamp_ns", "open", "high", "low", "close", "volume"});
    *result.add_bars() =
        encode_bar({row.at("trading_day").get<std::string>(), nanoseconds(row.at("timestamp_ns")),
                    decimal(row, "open"), decimal(row, "high"), decimal(row, "low"),
                    decimal(row, "close"), decimal(row, "volume")});
  }
  for (const auto& row : json.at("days")) {
    require_fields(row, {"trading_day", "settlement_price"});
    auto* day = result.add_days();
    day->set_trading_day(row.at("trading_day").get<std::string>());
    set(day->mutable_settlement_price(), decimal(row, "settlement_price"));
  }
  result.set_source(json.at("source").get<std::string>());
  result.set_source_task_id(json.at("source_task_id").get<std::string>());
  result.set_settlement_task_id(json.at("settlement_task_id").get<std::string>());
  result.set_manifest_sha256(json.at("manifest_sha256").get<std::string>());
  result.set_settlement_manifest_sha256(json.at("settlement_manifest_sha256").get<std::string>());
  validate_bar_dataset(result);
  return result;
}
data::v1::BarDatasetRequest encode_bar_dataset_request(const Json& json) {
  require_fields(json,
                 {"source_task_id", "settlement_task_id", "begin_day", "end_day", "contract"});
  data::v1::BarDatasetRequest result;
  result.set_source_task_id(json.at("source_task_id").get<std::string>());
  result.set_settlement_task_id(json.at("settlement_task_id").get<std::string>());
  result.set_begin_day(json.at("begin_day").get<std::string>());
  result.set_end_day(json.at("end_day").get<std::string>());
  *result.mutable_contract() = encode_contract(json.at("contract"));
  (void)decode_bar_dataset_request(result);
  return result;
}
Json decode_bar_dataset_request(const data::v1::BarDatasetRequest& request) {
  validate_message(request);
  validate_id(request.source_task_id());
  validate_id(request.settlement_task_id());
  for (const auto* day : {&request.begin_day(), &request.end_day()})
    if (!day->empty())
      (void)parse_trading_date(*day);
  if (!request.begin_day().empty() && !request.end_day().empty() &&
      request.begin_day() > request.end_day())
    throw std::invalid_argument("dataset begin day follows end day");
  (void)instrument(request.contract());
  // Evaluated before the braced initializer (GCC < 13 PR66139 leak).
  auto contract = decode_contract(request.contract());
  return {{"source_task_id", request.source_task_id()},
          {"settlement_task_id", request.settlement_task_id()},
          {"begin_day", request.begin_day()},
          {"end_day", request.end_day()},
          {"contract", std::move(contract)}};
}
} // namespace asterion::protocol
