#include <set>
#include <asterion/domain/futures.hpp>
#include <asterion/foundation/id.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/protocol/data.hpp>
#include <algorithm>
#include <charconv>
#include <optional>
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
void validate_versions(const google::protobuf::RepeatedPtrField<std::string>& ids) {
  if (ids.empty() || ids.size() > max_dataset_sources)
    throw std::invalid_argument("dataset requires 1..32 versions per source role");
  for (int i = 0; i < ids.size(); ++i)
    if (!digest(ids.Get(i)) || (i && ids.Get(i) <= ids.Get(i - 1)))
      throw std::invalid_argument("dataset versions must be sorted unique SHA-256 identities");
}
void encode_versions(const Json& json, google::protobuf::RepeatedPtrField<std::string>* out) {
  if (!json.is_array() || json.size() > static_cast<std::size_t>(max_dataset_sources))
    throw std::invalid_argument("dataset requires 1..32 versions per source role");
  auto ids = json.get<std::vector<std::string>>();
  std::ranges::sort(ids);
  for (const auto& id : ids)
    *out->Add() = id;
  validate_versions(*out);
}
Json versions(const google::protobuf::RepeatedPtrField<std::string>& ids) {
  return std::vector<std::string>(ids.begin(), ids.end());
}
Json bar_json(const MarketBar& bar) {
  return {{"trading_day", bar.trading_day}, {"timestamp_ns", std::to_string(bar.timestamp_ns)},
          {"open", bar.open.str()},         {"high", bar.high.str()},
          {"low", bar.low.str()},           {"close", bar.close.str()},
          {"volume", bar.volume.str()}};
}
// Canonical content: the fields that define what the bars are, not where they came from.
Json contents(const data::v1::BarDataset& dataset, DatasetView view) {
  auto contract = decode_contract(dataset.contract());
  Json result{{"version", 1},
              {"contract", std::move(contract)},
              {"interval_minutes", dataset.interval_minutes()}};
  if (view == DatasetView::metadata)
    return result;
  auto bars = Json::array();
  for (const auto& bar : dataset.bars())
    bars.push_back(bar_json(market_bar(bar)));
  auto days = Json::array();
  for (const auto& day : dataset.days())
    days.push_back({{"trading_day", day.trading_day()},
                    {"settlement_price", value(day.settlement_price()).str()}});
  result["bars"] = std::move(bars);
  result["days"] = std::move(days);
  return result;
}
} // namespace
void validate_history_evidence(const data::v1::HistoryVersionEvidence& evidence) {
  validate_message(evidence);
  if (!digest(evidence.dataset_id()) || evidence.acquired_at_ns() <= 0 ||
      evidence.source_availability() != data::v1::SOURCE_AVAILABILITY_UNKNOWN)
    throw std::invalid_argument("invalid historical availability evidence");
}
Json decode_history_evidence(const data::v1::HistoryVersionEvidence& evidence) {
  validate_history_evidence(evidence);
  return {{"dataset_id", evidence.dataset_id()},
          {"acquired_at_ns", std::to_string(evidence.acquired_at_ns())},
          {"source_availability", "unknown"}};
}
data::v1::HistoryVersionEvidence encode_history_evidence(const Json& json) {
  require_fields(json, {"dataset_id", "acquired_at_ns", "source_availability"});
  if (json.at("source_availability") != "unknown")
    throw std::invalid_argument("invalid historical availability evidence");
  data::v1::HistoryVersionEvidence result;
  result.set_dataset_id(json.at("dataset_id").get<std::string>());
  result.set_acquired_at_ns(nanoseconds(json.at("acquired_at_ns")));
  result.set_source_availability(data::v1::SOURCE_AVAILABILITY_UNKNOWN);
  validate_history_evidence(result);
  return result;
}
Instrument instrument(const v1::Contract& c) {
  if (!c.has_price_increment() || !c.has_quantity_increment() || !c.has_multiplier())
    throw std::invalid_argument("incomplete contract specification");
  Instrument result{{c.venue(), c.symbol()},
                    c.currency(),
                    value(c.price_increment()),
                    value(c.quantity_increment()),
                    value(c.multiplier())};
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
  // Preserve the exact sorted-key JSON digest without constructing a JSON
  // object for every bar. Only the canonical byte buffer grows with the input.
  std::string canonical = "{\"bars\":[";
  std::string previous_day, encoded_day;
  bool first = true;
  for (const auto& row : dataset.bars()) {
    const auto bar = market_bar(row);
    if (!first)
      canonical += ',';
    if (first || previous_day != bar.trading_day) {
      previous_day = bar.trading_day;
      encoded_day = Json(previous_day).dump();
    }
    first = false;
    canonical += "{\"close\":\"";
    canonical += bar.close.str();
    canonical += "\",\"high\":\"";
    canonical += bar.high.str();
    canonical += "\",\"low\":\"";
    canonical += bar.low.str();
    canonical += "\",\"open\":\"";
    canonical += bar.open.str();
    canonical += "\",\"timestamp_ns\":\"";
    canonical += std::to_string(bar.timestamp_ns);
    canonical += "\",\"trading_day\":";
    canonical += encoded_day;
    canonical += ",\"volume\":\"";
    canonical += bar.volume.str();
    canonical += "\"}";
  }
  canonical += "],\"contract\":";
  canonical += decode_contract(dataset.contract()).dump();
  canonical += ",\"days\":[";
  first = true;
  for (const auto& day : dataset.days()) {
    if (!first)
      canonical += ',';
    first = false;
    canonical += "{\"settlement_price\":\"";
    canonical += value(day.settlement_price()).str();
    canonical += "\",\"trading_day\":";
    canonical += Json(day.trading_day()).dump();
    canonical += '}';
  }
  canonical += "],\"interval_minutes\":";
  canonical += std::to_string(dataset.interval_minutes());
  canonical += ",\"version\":1}";
  return sha256_bytes(canonical);
}
void validate_bar_dataset(const data::v1::BarDataset& dataset) {
  validate_message(dataset);
  if (dataset.version() != 2 || !dataset.has_contract() || dataset.bars_size() < 1 ||
      static_cast<std::size_t>(dataset.bars_size()) > max_dataset_bars || dataset.days_size() < 1 ||
      dataset.days_size() > dataset.bars_size())
    throw std::invalid_argument("bar dataset requires version 2, 1..200000 bars and trading days");
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
  if (dataset.source().empty() || dataset.source().size() > 64)
    throw std::invalid_argument("invalid bar dataset provenance");
  validate_versions(dataset.source_dataset_ids());
  validate_versions(dataset.settlement_dataset_ids());
  std::set<std::string> versions(dataset.source_dataset_ids().begin(),
                                 dataset.source_dataset_ids().end());
  versions.insert(dataset.settlement_dataset_ids().begin(), dataset.settlement_dataset_ids().end());
  if (dataset.history_evidence_size() != static_cast<int>(versions.size()))
    throw std::invalid_argument("historical evidence does not match dataset versions");
  auto expected = versions.begin();
  for (const auto& evidence : dataset.history_evidence()) {
    validate_history_evidence(evidence);
    if (evidence.dataset_id() != *expected++)
      throw std::invalid_argument("historical evidence does not match dataset versions");
  }
  const auto spec = instrument(dataset.contract());
  std::optional<MarketBar> previous;
  std::vector<std::string> bar_days;
  for (const auto& row : dataset.bars()) {
    auto bar = market_bar(row);
    bar.validate(spec);
    if (previous &&
        (bar.timestamp_ns <= previous->timestamp_ns || bar.trading_day < previous->trading_day))
      throw std::invalid_argument("dataset bars must be strictly ascending");
    if (bar_days.empty() || bar_days.back() != bar.trading_day)
      bar_days.push_back(bar.trading_day);
    previous = std::move(bar);
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
Json decode_bar_dataset_range(const data::v1::BarDataset& dataset) {
  if (dataset.bars().empty())
    throw std::invalid_argument("bar dataset requires version 2, 1..200000 bars and trading days");
  const auto& first = dataset.bars(0);
  const auto& last = dataset.bars(dataset.bars_size() - 1);
  return {{"count", dataset.bars_size()},
          {"first_timestamp_ns", std::to_string(first.timestamp_ns())},
          {"last_timestamp_ns", std::to_string(last.timestamp_ns())},
          {"first_day", first.trading_day()},
          {"last_day", last.trading_day()},
          {"interval_minutes", dataset.interval_minutes()},
          {"source", dataset.source()},
          {"source_dataset_ids", std::vector<std::string>(dataset.source_dataset_ids().begin(),
                                                          dataset.source_dataset_ids().end())}};
}
Json decode_bar_dataset(const data::v1::BarDataset& dataset, DatasetView view) {
  validate_bar_dataset(dataset);
  auto result = contents(dataset, view);
  result["version"] = dataset.version();
  result["history_evidence"] = Json::array();
  for (const auto& evidence : dataset.history_evidence())
    result["history_evidence"].push_back(decode_history_evidence(evidence));
  result.update({{"revision", dataset.revision()},
                 {"source", dataset.source()},
                 {"source_dataset_ids", versions(dataset.source_dataset_ids())},
                 {"settlement_dataset_ids", versions(dataset.settlement_dataset_ids())}});
  return result;
}
data::v1::BarDataset encode_bar_dataset(const Json& json) {
  require_fields(json,
                 {"version", "revision", "contract", "interval_minutes", "bars", "days", "source",
                  "source_dataset_ids", "settlement_dataset_ids", "history_evidence"});
  if (!json.at("version").is_number_integer() || json.at("version") != 2 ||
      !json.at("interval_minutes").is_number_unsigned() || !json.at("bars").is_array() ||
      !json.at("days").is_array() || json.at("bars").size() > max_dataset_bars)
    throw std::invalid_argument("unsupported bar dataset format");
  data::v1::BarDataset result;
  result.set_version(2);
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
  encode_versions(json.at("source_dataset_ids"), result.mutable_source_dataset_ids());
  encode_versions(json.at("settlement_dataset_ids"), result.mutable_settlement_dataset_ids());
  if (!json.at("history_evidence").is_array() ||
      json.at("history_evidence").size() > 2 * max_dataset_sources)
    throw std::invalid_argument("historical evidence does not match dataset versions");
  for (const auto& evidence : json.at("history_evidence"))
    *result.add_history_evidence() = encode_history_evidence(evidence);
  validate_bar_dataset(result);
  return result;
}
data::v1::BarDatasetRequest encode_bar_dataset_request(const Json& json) {
  require_fields(
      json, {"source_dataset_ids", "settlement_dataset_ids", "begin_day", "end_day", "contract"});
  data::v1::BarDatasetRequest result;
  encode_versions(json.at("source_dataset_ids"), result.mutable_source_dataset_ids());
  encode_versions(json.at("settlement_dataset_ids"), result.mutable_settlement_dataset_ids());
  result.set_begin_day(json.at("begin_day").get<std::string>());
  result.set_end_day(json.at("end_day").get<std::string>());
  *result.mutable_contract() = encode_contract(json.at("contract"));
  (void)decode_bar_dataset_request(result);
  return result;
}
Json decode_bar_dataset_request(const data::v1::BarDatasetRequest& request) {
  validate_message(request);
  validate_versions(request.source_dataset_ids());
  validate_versions(request.settlement_dataset_ids());
  for (const auto* day : {&request.begin_day(), &request.end_day()})
    if (!day->empty())
      (void)parse_trading_date(*day);
  if (!request.begin_day().empty() && !request.end_day().empty() &&
      request.begin_day() > request.end_day())
    throw std::invalid_argument("dataset begin day follows end day");
  (void)instrument(request.contract());
  // Evaluated before the braced initializer (GCC < 13 PR66139 leak).
  auto contract = decode_contract(request.contract());
  return {{"source_dataset_ids", versions(request.source_dataset_ids())},
          {"settlement_dataset_ids", versions(request.settlement_dataset_ids())},
          {"begin_day", request.begin_day()},
          {"end_day", request.end_day()},
          {"contract", std::move(contract)}};
}
void validate_dominant_schedule(const data::v1::DominantSchedule& schedule,
                                std::span<const v1::Contract* const> contracts) {
  if (schedule.rolls().empty())
    throw std::invalid_argument("invalid dominant series schedule");
  const data::v1::DominantRoll* previous = nullptr;
  for (const auto& roll : schedule.rolls()) {
    (void)parse_trading_date(roll.trading_day());
    if (roll.contract() >= contracts.size() || !roll.has_factor() || roll.factor().units() <= 0)
      throw std::invalid_argument("invalid dominant series schedule");
    if (previous) {
      const auto& contract = *contracts[roll.contract()];
      const auto& before = *contracts[previous->contract()];
      // One product, later months only, each from a later day.
      if (roll.trading_day() <= previous->trading_day() || contract.venue() != before.venue() ||
          contract.product() != before.product() ||
          contract.delivery_month() <= before.delivery_month() ||
          contract.price_increment().units() != before.price_increment().units() ||
          contract.multiplier().units() != before.multiplier().units())
        throw std::invalid_argument("invalid dominant series schedule");
    }
    previous = &roll;
  }
  if (previous->factor().units() != Decimal::parse("1").raw())
    throw std::invalid_argument("invalid dominant series schedule");
  const data::v1::TermPoint* before = nullptr;
  for (const auto& term : schedule.terms()) {
    (void)parse_trading_date(term.trading_day());
    const auto& month =
        contracts[dominant_roll(schedule, term.trading_day()).contract()]->delivery_month();
    if (term.trading_day() < schedule.rolls(0).trading_day() ||
        (before && term.trading_day() <= before->trading_day()) || term.near().units() <= 0 ||
        term.far().units() <= 0 || term.far_month().size() != month.size() ||
        term.far_month() <= month)
      throw std::invalid_argument("invalid dominant series schedule");
    (void)term_carry(term, month);
    before = &term;
  }
}
const data::v1::DominantRoll& dominant_roll(const data::v1::DominantSchedule& schedule,
                                            const std::string& trading_day) {
  const auto* current = &schedule.rolls(0);
  for (const auto& roll : schedule.rolls())
    if (roll.trading_day() <= trading_day)
      current = &roll;
  return *current;
}
Decimal dominant_price(Decimal raw, Decimal factor, Decimal increment) {
  return quantize(multiply(raw, factor, Rounding::half_up), increment, Rounding::half_up);
}
Decimal term_carry(const data::v1::TermPoint& point, const std::string& near_month) {
  // Delivery months are YYYY-MM.
  const auto months = [](const std::string& text) {
    int year = 0, month = 0;
    const auto* end = text.data() + text.size();
    if (text.size() != 7 || text[4] != '-' ||
        std::from_chars(text.data(), text.data() + 4, year).ptr != text.data() + 4 ||
        std::from_chars(text.data() + 5, end, month).ptr != end || month < 1 || month > 12)
      throw std::invalid_argument("invalid dominant series schedule");
    return year * 12 + month;
  };
  const auto apart = months(point.far_month()) - months(near_month);
  if (apart <= 0)
    throw std::invalid_argument("invalid dominant series schedule");
  const auto spread =
      divide(value(point.near()), value(point.far()), Rounding::half_up) - Decimal::parse("1");
  return divide(multiply(spread, Decimal::parse("12"), Rounding::half_up),
                Decimal::parse(std::to_string(apart)), Rounding::half_up);
}
Json decode_term_points(const data::v1::DominantSchedule& schedule) {
  Json terms = Json::array();
  for (const auto& term : schedule.terms())
    terms.push_back({{"trading_day", term.trading_day()},
                     {"near", value(term.near()).str()},
                     {"far", value(term.far()).str()},
                     {"far_month", term.far_month()}});
  return terms;
}
Json decode_dominant_schedule(const data::v1::DominantSchedule& schedule) {
  Json rolls = Json::array();
  for (const auto& roll : schedule.rolls())
    rolls.push_back({{"trading_day", roll.trading_day()},
                     {"contract", roll.contract()},
                     {"factor", value(roll.factor()).str()}});
  return rolls;
}
std::string named_dataset_revision(const data::v1::NamedDataset& value) {
  auto canonical = value;
  canonical.clear_id();
  return sha256_bytes(canonical.SerializeAsString());
}
void validate_named_dataset(const data::v1::NamedDataset& value) {
  validate_message(value);
  if (value.version() != 1 || value.name().empty() || value.name().size() > 120 ||
      value.name().front() == ' ' || value.name().back() == ' ' ||
      std::any_of(value.name().begin(), value.name().end(),
                  [](unsigned char c) { return c < 32 || c == 127; }) ||
      value.selections_size() < 1 || value.selections_size() > 20 ||
      value.content_revisions_size() != value.selections_size())
    throw std::invalid_argument("invalid saved dataset");
  std::set<std::string> contracts;
  for (int i = 0; i < value.selections_size(); ++i) {
    const auto& request = value.selections(i);
    (void)decode_bar_dataset_request(request);
    if (!contracts.insert(request.contract().venue() + "/" + request.contract().symbol()).second ||
        !digest(value.content_revisions(i)))
      throw std::invalid_argument("invalid saved dataset");
  }
  if (!digest(value.id()) || value.id() != named_dataset_revision(value))
    throw std::invalid_argument("saved dataset revision mismatch");
}
Json decode_named_dataset(const data::v1::NamedDataset& value) {
  validate_named_dataset(value);
  Json inputs = Json::array();
  for (int i = 0; i < value.selections_size(); ++i) {
    auto input = decode_bar_dataset_request(value.selections(i));
    input["revision"] = value.content_revisions(i);
    inputs.push_back(std::move(input));
  }
  return {{"id", value.id()}, {"name", value.name()}, {"selections", std::move(inputs)}};
}
} // namespace asterion::protocol
