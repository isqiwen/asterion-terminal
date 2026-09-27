#include <asterion/domain/futures.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/protocol/data.hpp>
namespace asterion::protocol {
namespace {
Json contents(const data::v1::TradeDataset &dataset) {
  validate_message(dataset);
  if (dataset.version() != 1 || !dataset.has_contract() ||
      dataset.ticks_size() < 1 || dataset.ticks_size() > 10000)
    throw std::invalid_argument(
        "trade dataset requires 1..10000 events and version 1");
  const auto &c = dataset.contract();
  if (!c.has_price_increment() || !c.has_quantity_increment() ||
      !c.has_multiplier())
    throw std::invalid_argument("incomplete dataset contract");
  Instrument spec{{c.venue(), c.symbol()},
                  AssetClass::futures,
                  c.currency(),
                  Decimal::from_raw(c.price_increment().units()),
                  Decimal::from_raw(c.quantity_increment().units()),
                  Decimal::from_raw(c.multiplier().units())};
  FuturesContract{spec, c.product(), c.delivery_month()}.validate();
  Json rows = Json::array();
  std::int64_t previous = -1;
  for (const auto &value : dataset.ticks()) {
    if (!value.has_price() || !value.has_quantity())
      throw std::invalid_argument("incomplete dataset event");
    TradeTick tick{spec.id, value.timestamp_ns(),
                   Decimal::from_raw(value.price().units()),
                   Decimal::from_raw(value.quantity().units())};
    tick.validate(spec);
    if (tick.timestamp_ns < previous)
      throw std::invalid_argument("dataset events are out of order");
    previous = tick.timestamp_ns;
    rows.push_back(decode_tick(value));
  }
  return {{"version", 1},
          {"type", "futures.trade-events"},
          {"contract", decode_contract(c)},
          {"ticks", rows}};
}
Json provenance(const data::v1::DatasetPublication &p) {
  validate_message(p);
  if (p.version() != 1 || !p.has_dataset() ||
      p.importer() != "asterion.csv.trades.v1" || p.source_name().empty() ||
      p.source_name().size() > 255 ||
      p.source_name().find_first_of("/\\") != std::string::npos ||
      p.source_name().find('\0') != std::string::npos ||
      p.source_sha256().size() != 64 ||
      p.source_sha256().find_first_not_of("0123456789abcdef") !=
          std::string::npos ||
      !p.source_bytes() || p.source_bytes() > 32 * 1024 * 1024)
    throw std::invalid_argument("invalid dataset provenance");
  return {{"version", 1},
          {"dataset", decode_dataset(p.dataset())},
          {"source_name", p.source_name()},
          {"source_sha256", p.source_sha256()},
          {"source_bytes", p.source_bytes()},
          {"importer", p.importer()}};
}
} // namespace
data::v1::TradeDataset
make_trade_dataset(const v1::Contract &contract,
                   const google::protobuf::RepeatedPtrField<v1::Tick> &ticks) {
  data::v1::TradeDataset result;
  result.set_version(1);
  *result.mutable_contract() = contract;
  *result.mutable_ticks() = ticks;
  result.set_revision(sha256_bytes(contents(result).dump()));
  return result;
}
Json decode_dataset(const data::v1::TradeDataset &dataset) {
  auto result = contents(dataset);
  if (dataset.revision() != sha256_bytes(result.dump()))
    throw std::invalid_argument("dataset revision mismatch");
  result["revision"] = dataset.revision();
  return result;
}
data::v1::TradeDataset encode_dataset(const Json &dataset) {
  require_fields(dataset, {"version", "type", "contract", "ticks", "revision"});
  if (!dataset.at("version").is_number_integer() ||
      dataset.at("version") != 1 ||
      dataset.at("type") != "futures.trade-events" ||
      !dataset.at("ticks").is_array() || dataset.at("ticks").size() > 10000)
    throw std::invalid_argument("unsupported dataset format");
  data::v1::TradeDataset result;
  result.set_version(1);
  result.set_revision(dataset.at("revision").get<std::string>());
  *result.mutable_contract() = encode_contract(dataset.at("contract"));
  for (const auto &row : dataset.at("ticks"))
    *result.add_ticks() = encode_tick(row);
  static_cast<void>(decode_dataset(result));
  return result;
}
std::string publication_id(const data::v1::DatasetPublication &p) {
  return sha256_bytes(provenance(p).dump());
}
Json decode_publication(const data::v1::DatasetPublication &p) {
  auto result = provenance(p);
  if (p.id() != sha256_bytes(result.dump()))
    throw std::invalid_argument("publication identity mismatch");
  result["id"] = p.id();
  return result;
}
data::v1::DatasetPublication encode_publication(const Json &p) {
  require_fields(p, {"version", "id", "dataset", "source_name", "source_sha256",
                     "source_bytes", "importer"});
  if (!p.at("version").is_number_integer() || p.at("version") != 1 ||
      !p.at("source_bytes").is_number_integer() || p.at("source_bytes") < 1 ||
      p.at("source_bytes") > 32 * 1024 * 1024)
    throw std::invalid_argument("invalid publication format");
  data::v1::DatasetPublication result;
  result.set_version(1);
  result.set_id(p.at("id").get<std::string>());
  *result.mutable_dataset() = encode_dataset(p.at("dataset"));
  result.set_source_name(p.at("source_name").get<std::string>());
  result.set_source_sha256(p.at("source_sha256").get<std::string>());
  result.set_source_bytes(p.at("source_bytes").get<std::uint64_t>());
  result.set_importer(p.at("importer").get<std::string>());
  static_cast<void>(decode_publication(result));
  return result;
}
Json decode_csv_snapshot(const data::v1::CsvSnapshot &input) {
  validate_message(input);
  if (input.version() != 1 || !input.has_contract() ||
      input.contents().empty() || input.contents().size() > 32 * 1024 * 1024 ||
      input.source_name().empty() || input.source_name().size() > 255 ||
      input.source_name().find_first_of("/\\") != std::string::npos ||
      input.source_name().find('\0') != std::string::npos)
    throw std::invalid_argument("invalid CSV snapshot metadata or size");
  for (unsigned char byte : input.contents())
    if (byte != '\n' && byte != '\r' && (byte < 32 || byte > 126))
      throw std::invalid_argument(
          "numeric CSV snapshot contains unsupported bytes");
  if (input.source_sha256() != sha256_bytes(input.contents()))
    throw std::invalid_argument("CSV snapshot digest mismatch");
  const auto &c = input.contract();
  if (!c.has_price_increment() || !c.has_quantity_increment() ||
      !c.has_multiplier())
    throw std::invalid_argument("incomplete import contract");
  FuturesContract{{{c.venue(), c.symbol()},
                   AssetClass::futures,
                   c.currency(),
                   Decimal::from_raw(c.price_increment().units()),
                   Decimal::from_raw(c.quantity_increment().units()),
                   Decimal::from_raw(c.multiplier().units())},
                  c.product(),
                  c.delivery_month()}
      .validate();
  return {{"version", 1},
          {"source_name", input.source_name()},
          {"source_sha256", input.source_sha256()},
          {"contract", decode_contract(c)},
          {"contents", input.contents()}};
}
data::v1::CsvSnapshot encode_csv_snapshot(const Json &input) {
  require_fields(input, {"version", "source_name", "source_sha256", "contract",
                         "contents"});
  if (!input.at("version").is_number_integer() || input.at("version") != 1)
    throw std::invalid_argument("unsupported CSV snapshot version");
  data::v1::CsvSnapshot result;
  result.set_version(1);
  result.set_source_name(input.at("source_name").get<std::string>());
  result.set_source_sha256(input.at("source_sha256").get<std::string>());
  result.set_contents(input.at("contents").get<std::string>());
  *result.mutable_contract() = encode_contract(input.at("contract"));
  static_cast<void>(decode_csv_snapshot(result));
  return result;
}
} // namespace asterion::protocol
