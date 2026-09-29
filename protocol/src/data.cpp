#include <asterion/domain/daily_bars.hpp>
#include <asterion/domain/futures.hpp>
#include <asterion/domain/historical_bars.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/protocol/data.hpp>
#include <stdexcept>
#include <cmath>
namespace asterion::protocol {
namespace {
Json contents(const data::v1::TradeDataset& dataset) {
  validate_message(dataset);
  if (dataset.version() != 1 || !dataset.has_contract() || dataset.ticks_size() < 1 ||
      dataset.ticks_size() > 10000)
    throw std::invalid_argument("trade dataset requires 1..10000 events and version 1");
  const auto& c = dataset.contract();
  if (!c.has_price_increment() || !c.has_quantity_increment() || !c.has_multiplier())
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
  for (const auto& value : dataset.ticks()) {
    if (!value.has_price() || !value.has_quantity())
      throw std::invalid_argument("incomplete dataset event");
    TradeTick tick{spec.id, value.timestamp_ns(), Decimal::from_raw(value.price().units()),
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
Json provenance(const data::v1::DatasetPublication& p) {
  validate_message(p);
  if (p.version() != 1 || !p.has_dataset() || p.importer() != "asterion.csv.trades.v1" ||
      p.source_name().empty() || p.source_name().size() > 255 ||
      p.source_name().find_first_of("/\\") != std::string::npos ||
      p.source_name().find('\0') != std::string::npos || p.source_sha256().size() != 64 ||
      p.source_sha256().find_first_not_of("0123456789abcdef") != std::string::npos ||
      !p.source_bytes() || p.source_bytes() > 32 * 1024 * 1024)
    throw std::invalid_argument("invalid dataset provenance");
  // Evaluated before the braced initializer: GCC < 13 leaks already-built
  // initializer_list elements when a later element throws (PR66139).
  auto dataset = decode_dataset(p.dataset());
  return {{"version", 1},
          {"dataset", std::move(dataset)},
          {"source_name", p.source_name()},
          {"source_sha256", p.source_sha256()},
          {"source_bytes", p.source_bytes()},
          {"importer", p.importer()}};
}
} // namespace
data::v1::TradeDataset
make_trade_dataset(const v1::Contract& contract,
                   const google::protobuf::RepeatedPtrField<v1::Tick>& ticks) {
  data::v1::TradeDataset result;
  result.set_version(1);
  *result.mutable_contract() = contract;
  *result.mutable_ticks() = ticks;
  result.set_revision(sha256_bytes(contents(result).dump()));
  return result;
}
Json decode_dataset(const data::v1::TradeDataset& dataset) {
  auto result = contents(dataset);
  if (dataset.revision() != sha256_bytes(result.dump()))
    throw std::invalid_argument("dataset revision mismatch");
  result["revision"] = dataset.revision();
  return result;
}
data::v1::TradeDataset encode_dataset(const Json& dataset) {
  require_fields(dataset, {"version", "type", "contract", "ticks", "revision"});
  if (!dataset.at("version").is_number_integer() || dataset.at("version") != 1 ||
      dataset.at("type") != "futures.trade-events" || !dataset.at("ticks").is_array() ||
      dataset.at("ticks").size() > 10000)
    throw std::invalid_argument("unsupported dataset format");
  data::v1::TradeDataset result;
  result.set_version(1);
  result.set_revision(dataset.at("revision").get<std::string>());
  *result.mutable_contract() = encode_contract(dataset.at("contract"));
  for (const auto& row : dataset.at("ticks"))
    *result.add_ticks() = encode_tick(row);
  static_cast<void>(decode_dataset(result));
  return result;
}
std::string publication_id(const data::v1::DatasetPublication& p) {
  return sha256_bytes(provenance(p).dump());
}
Json decode_publication(const data::v1::DatasetPublication& p) {
  auto result = provenance(p);
  if (p.id() != sha256_bytes(result.dump()))
    throw std::invalid_argument("publication identity mismatch");
  result["id"] = p.id();
  return result;
}
data::v1::DatasetPublication encode_publication(const Json& p) {
  require_fields(
      p, {"version", "id", "dataset", "source_name", "source_sha256", "source_bytes", "importer"});
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
Json decode_csv_snapshot(const data::v1::CsvSnapshot& input) {
  validate_message(input);
  if (input.version() != 1 || !input.has_contract() || input.contents().empty() ||
      input.contents().size() > 32 * 1024 * 1024 || input.source_name().empty() ||
      input.source_name().size() > 255 ||
      input.source_name().find_first_of("/\\") != std::string::npos ||
      input.source_name().find('\0') != std::string::npos)
    throw std::invalid_argument("invalid CSV snapshot metadata or size");
  for (unsigned char byte : input.contents())
    if (byte != '\n' && byte != '\r' && (byte < 32 || byte > 126))
      throw std::invalid_argument("numeric CSV snapshot contains unsupported bytes");
  if (input.source_sha256() != sha256_bytes(input.contents()))
    throw std::invalid_argument("CSV snapshot digest mismatch");
  const auto& c = input.contract();
  if (!c.has_price_increment() || !c.has_quantity_increment() || !c.has_multiplier())
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
data::v1::CsvSnapshot encode_csv_snapshot(const Json& input) {
  require_fields(input, {"version", "source_name", "source_sha256", "contract", "contents"});
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

Json decode_minute_page(const data::v1::MinutePage& page) {
  validate_message(page);
  if (page.version() != 1 || page.task_id().empty() || page.limit() < 1 || page.limit() > 200 ||
      page.bars_size() > static_cast<int>(page.limit()) ||
      page.matched_rows() > page.total_rows() || page.total_rows() > 60000000 ||
      page.offset() > page.matched_rows() ||
      page.offset() + page.bars_size() > page.matched_rows() ||
      static_cast<std::uint64_t>(page.bars_size()) !=
          std::min<std::uint64_t>(page.limit(), page.matched_rows() - page.offset()) ||
      page.begin_ns() > page.end_ns() || page.manifest_sha256().size() != 64 ||
      page.source().empty() ||
      (page.total_rows() && (page.first_ns() <= 0 || page.first_ns() > page.last_ns())))
    throw std::invalid_argument("invalid minute dataset page response");
  Json bars = Json::array();
  std::int64_t previous = -1;
  for (const auto& row : page.bars()) {
    if (!row.has_open() || !row.has_high() || !row.has_low() || !row.has_close() ||
        !row.has_volume() || !row.has_amount() || !row.has_open_interest())
      throw std::invalid_argument("invalid minute dataset page response");
    HistoricalBar bar{row.timestamp_ns(),
                      Decimal::from_raw(row.open().units()),
                      Decimal::from_raw(row.high().units()),
                      Decimal::from_raw(row.low().units()),
                      Decimal::from_raw(row.close().units()),
                      Decimal::from_raw(row.volume().units()),
                      Decimal::from_raw(row.amount().units()),
                      Decimal::from_raw(row.open_interest().units())};
    bar.validate();
    if (bar.timestamp_ns <= previous || bar.timestamp_ns < page.begin_ns() ||
        bar.timestamp_ns > page.end_ns())
      throw std::invalid_argument("invalid minute dataset page response");
    previous = bar.timestamp_ns;
    bars.push_back({{"timestamp_ns", std::to_string(bar.timestamp_ns)},
                    {"open", bar.open.str()},
                    {"high", bar.high.str()},
                    {"low", bar.low.str()},
                    {"close", bar.close.str()},
                    {"volume", bar.volume.str()},
                    {"amount", bar.amount.str()},
                    {"open_interest", bar.open_interest.str()}});
    if (row.has_macd()) {
      const auto& macd = row.macd();
      if (!std::isfinite(macd.diff()) || !std::isfinite(macd.signal()) ||
          !std::isfinite(macd.histogram()))
        throw std::invalid_argument("invalid minute dataset page response");
      bars.back()["macd"] = {
          {"diff", macd.diff()}, {"signal", macd.signal()}, {"histogram", macd.histogram()}};
    }
  }
  return {{"id", page.task_id()},
          {"source", page.source()},
          {"ts_code", page.ts_code()},
          {"interval_minutes", page.interval_minutes()},
          {"manifest_sha256", page.manifest_sha256()},
          {"total_rows", page.total_rows()},
          {"matched_rows", page.matched_rows()},
          {"offset", page.offset()},
          {"limit", page.limit()},
          {"first_ns", std::to_string(page.first_ns())},
          {"last_ns", std::to_string(page.last_ns())},
          {"begin_ns", std::to_string(page.begin_ns())},
          {"end_ns", std::to_string(page.end_ns())},
          {"bars", bars}};
}
data::v1::DailyBar encode_daily_bar(const HistoricalDailyBar& bar) {
  bar.validate();
  data::v1::DailyBar row;
  row.set_trading_day(format_trading_date(bar.trading_day));
  row.mutable_open()->set_units(bar.open.raw());
  row.mutable_high()->set_units(bar.high.raw());
  row.mutable_low()->set_units(bar.low.raw());
  row.mutable_close()->set_units(bar.close.raw());
  row.mutable_volume()->set_units(bar.volume.raw());
  row.mutable_amount()->set_units(bar.amount.raw());
  row.mutable_open_interest()->set_units(bar.open_interest.raw());
  if (bar.previous_close)
    row.mutable_previous_close()->set_units(bar.previous_close->raw());
  if (bar.previous_settlement)
    row.mutable_previous_settlement()->set_units(bar.previous_settlement->raw());
  if (bar.settlement)
    row.mutable_settlement()->set_units(bar.settlement->raw());
  return row;
}
HistoricalDailyBar daily_bar(const data::v1::DailyBar& row) {
  validate_message(row);
  if (!row.has_open() || !row.has_high() || !row.has_low() || !row.has_close() ||
      !row.has_volume() || !row.has_amount() || !row.has_open_interest())
    throw std::invalid_argument("invalid daily dataset page response");
  HistoricalDailyBar bar{parse_trading_date(row.trading_day()),
                         Decimal::from_raw(row.open().units()),
                         Decimal::from_raw(row.high().units()),
                         Decimal::from_raw(row.low().units()),
                         Decimal::from_raw(row.close().units()),
                         Decimal::from_raw(row.volume().units()),
                         Decimal::from_raw(row.amount().units()),
                         Decimal::from_raw(row.open_interest().units()),
                         {},
                         {},
                         {}};
  if (row.has_previous_close())
    bar.previous_close = Decimal::from_raw(row.previous_close().units());
  if (row.has_previous_settlement())
    bar.previous_settlement = Decimal::from_raw(row.previous_settlement().units());
  if (row.has_settlement())
    bar.settlement = Decimal::from_raw(row.settlement().units());
  bar.validate();
  return bar;
}
Json decode_daily_page(const data::v1::DailyPage& page) {
  validate_message(page);
  validate_id(page.task_id());
  const auto begin = parse_trading_date(page.begin_day());
  const auto end = parse_trading_date(page.end_day());
  if (page.version() != 1 || page.limit() < 1 || page.limit() > 200 || begin > end ||
      !data::v1::DailyPeriod_IsValid(page.period()) || page.source() != "tushare.fut_daily" ||
      page.ts_code().empty() || page.total_rows() > 20 * 366 + 1 ||
      page.matched_rows() > page.total_rows() || page.offset() > page.matched_rows() ||
      (page.matched_rows() && page.offset() == page.matched_rows()) ||
      static_cast<std::uint64_t>(page.bars_size()) !=
          std::min<std::uint64_t>(page.limit(), page.matched_rows() - page.offset()) ||
      page.manifest_sha256().size() != 64 ||
      page.manifest_sha256().find_first_not_of("0123456789abcdef") != std::string::npos)
    throw std::invalid_argument("invalid daily dataset page response");
  std::optional<std::chrono::year_month_day> first, last;
  if (page.total_rows()) {
    first = parse_trading_date(page.first_day());
    last = parse_trading_date(page.last_day());
    if (*first > *last ||
        page.total_rows() >
            static_cast<std::uint64_t>(
                (std::chrono::sys_days(*last) - std::chrono::sys_days(*first)).count() + 1))
      throw std::invalid_argument("invalid daily dataset page response");
  } else if (!page.first_day().empty() || !page.last_day().empty())
    throw std::invalid_argument("invalid daily dataset page response");
  Json bars = Json::array();
  std::optional<std::chrono::year_month_day> previous;
  for (const auto& row : page.bars()) {
    const auto bar = daily_bar(row);
    if ((previous && bar.trading_day <= *previous) || bar.trading_day < begin ||
        bar.trading_day > end || !first || bar.trading_day < *first || bar.trading_day > *last ||
        bar.volume.raw() % 100000000 || bar.open_interest.raw() % 100000000)
      throw std::invalid_argument("invalid daily dataset page response");
    previous = bar.trading_day;
    const auto optional = [](const std::optional<Decimal>& value) -> Json {
      return value ? Json(value->str()) : Json(nullptr);
    };
    bars.push_back({{"trading_day", row.trading_day()},
                    {"open", bar.open.str()},
                    {"high", bar.high.str()},
                    {"low", bar.low.str()},
                    {"close", bar.close.str()},
                    {"volume", bar.volume.str()},
                    {"amount", bar.amount.str()},
                    {"open_interest", bar.open_interest.str()},
                    {"previous_close", optional(bar.previous_close)},
                    {"previous_settlement", optional(bar.previous_settlement)},
                    {"settlement", optional(bar.settlement)}});
    if (row.has_macd()) {
      const auto& macd = row.macd();
      if (!std::isfinite(macd.diff()) || !std::isfinite(macd.signal()) ||
          !std::isfinite(macd.histogram()))
        throw std::invalid_argument("invalid daily dataset page response");
      bars.back()["macd"] = {
          {"diff", macd.diff()}, {"signal", macd.signal()}, {"histogram", macd.histogram()}};
    }
  }
  return {{"id", page.task_id()},
          {"period", page.period() == data::v1::DAY       ? "day"
                     : page.period() == data::v1::WEEK    ? "week"
                     : page.period() == data::v1::MONTH   ? "month"
                     : page.period() == data::v1::QUARTER ? "quarter"
                                                          : "year"},
          {"source", page.source()},
          {"ts_code", page.ts_code()},
          {"manifest_sha256", page.manifest_sha256()},
          {"total_rows", page.total_rows()},
          {"matched_rows", page.matched_rows()},
          {"offset", page.offset()},
          {"limit", page.limit()},
          {"first_day", page.first_day()},
          {"last_day", page.last_day()},
          {"begin_day", page.begin_day()},
          {"end_day", page.end_day()},
          {"bars", bars}};
}
} // namespace asterion::protocol
