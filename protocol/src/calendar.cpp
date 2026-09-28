#include <asterion/domain/futures.hpp>
#include <asterion/domain/trading_schedule.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/protocol/data.hpp>
#include <charconv>
#include <stdexcept>
namespace asterion::protocol {
void encode_settlement_days(const Json& days,
                            google::protobuf::RepeatedPtrField<data::v1::SettlementDay>& result) {
  if (!result.empty())
    throw std::invalid_argument("day output must be empty");
  const auto timestamp = [](const Json& json) {
    const auto value = json.get<std::string>();
    std::int64_t result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size() ||
        value != std::to_string(result) || result < 0)
      throw std::invalid_argument("invalid session timestamp");
    return result;
  };
  if (!days.is_array() || days.empty() || days.size() > 64)
    throw std::invalid_argument("backtest requires 1..64 explicit trading days");
  for (const auto& row : days) {
    require_fields(row, {"trading_day", "sessions", "schedule_source", "settlement_price",
                         "settlement_source"});
    auto* day = result.Add();
    day->set_trading_day(row.at("trading_day").get<std::string>());
    day->set_schedule_source(row.at("schedule_source").get<std::string>());
    day->set_settlement_source(row.at("settlement_source").get<std::string>());
    const auto price_text = row.at("settlement_price").get<std::string>();
    const auto price = Decimal::parse(price_text);
    if (price.str() != price_text)
      throw std::invalid_argument("settlement price must be canonical decimal");
    day->mutable_settlement_price()->set_units(price.raw());
    const auto& sessions = row.at("sessions");
    if (!sessions.is_array() || sessions.empty() || sessions.size() > 16)
      throw std::invalid_argument("backtest day requires 1..16 explicit sessions");
    for (const auto& interval : sessions) {
      require_fields(interval, {"begin_ns", "end_ns"});
      auto* session = day->add_sessions();
      session->set_begin_ns(timestamp(interval.at("begin_ns")));
      session->set_end_ns(timestamp(interval.at("end_ns")));
    }
  }
}
Json decode_settlement_days(
    const google::protobuf::RepeatedPtrField<data::v1::SettlementDay>& input) {
  if (input.empty() || input.size() > 64)
    throw std::invalid_argument("calendar requires 1..64 days");
  Json days = Json::array();
  std::string previous_day;
  std::int64_t previous_end = 0;
  for (const auto& day : input) {
    validate_message(day);
    for (const auto& source : {day.schedule_source(), day.settlement_source()})
      if (source.empty() || source.size() > 256 ||
          source.find_first_not_of(" \t\r\n") == std::string::npos)
        throw std::invalid_argument("explicit schedule and settlement sources required");
    if (day.sessions().empty() || day.sessions_size() > 16 || !day.has_settlement_price() ||
        day.trading_day() <= previous_day)
      throw std::invalid_argument("invalid ordered backtest days or settlement price");
    std::vector<TradingSession> intervals;
    Json sessions = Json::array();
    for (const auto& session : day.sessions()) {
      intervals.push_back({session.begin_ns(), session.end_ns()});
      sessions.push_back({{"begin_ns", std::to_string(session.begin_ns())},
                          {"end_ns", std::to_string(session.end_ns())}});
    }
    const TradingDaySchedule schedule(day.trading_day(), std::move(intervals));
    if (schedule.sessions().front().begin_ns < previous_end)
      throw std::invalid_argument("trading day intervals overlap or reverse");
    previous_day = day.trading_day();
    previous_end = schedule.sessions().back().end_ns;
    days.push_back({{"trading_day", day.trading_day()},
                    {"sessions", sessions},
                    {"schedule_source", day.schedule_source()},
                    {"settlement_price", Decimal::from_raw(day.settlement_price().units()).str()},
                    {"settlement_source", day.settlement_source()}});
  }
  return days;
}

namespace {
void metadata(const std::string& name, const std::string& hash, std::uint64_t size) {
  if (name.empty() || name.size() > 255 || name.find_first_of("/\\") != std::string::npos ||
      name.find('\0') != std::string::npos || hash.size() != 64 ||
      hash.find_first_not_of("0123456789abcdef") != std::string::npos || !size ||
      size > 1024 * 1024)
    throw std::invalid_argument("invalid settlement calendar source metadata");
}
Json contents(const data::v1::SettlementCalendar& calendar) {
  validate_message(calendar);
  if (calendar.version() != 1 || !calendar.has_contract())
    throw std::invalid_argument("invalid calendar version or contract");
  const auto& c = calendar.contract();
  const auto contract = decode_contract(c);
  FuturesContract{{{c.venue(), c.symbol()},
                   AssetClass::futures,
                   c.currency(),
                   Decimal::from_raw(c.price_increment().units()),
                   Decimal::from_raw(c.quantity_increment().units()),
                   Decimal::from_raw(c.multiplier().units())},
                  c.product(),
                  c.delivery_month()}
      .validate();
  const auto days = decode_settlement_days(calendar.days());
  return {{"version", 1},
          {"type", "futures.settlement-calendar"},
          {"contract", contract},
          {"days", days}};
}
Json provenance(const data::v1::CalendarPublication& p) {
  validate_message(p);
  if (p.version() != 1 || !p.has_calendar() || p.importer() != "asterion.csv.settlement.v1")
    throw std::invalid_argument("invalid calendar publication");
  metadata(p.source_name(), p.source_sha256(), p.source_bytes());
  // Evaluated before the braced initializer: GCC < 13 leaks already-built
  // initializer_list elements when a later element throws (PR66139).
  auto calendar = decode_calendar(p.calendar());
  return {{"version", 1},
          {"calendar", std::move(calendar)},
          {"source_name", p.source_name()},
          {"source_sha256", p.source_sha256()},
          {"source_bytes", p.source_bytes()},
          {"importer", p.importer()}};
}
} // namespace
data::v1::SettlementCalendar
make_settlement_calendar(const v1::Contract& c,
                         const google::protobuf::RepeatedPtrField<data::v1::SettlementDay>& days) {
  data::v1::SettlementCalendar result;
  result.set_version(1);
  *result.mutable_contract() = c;
  *result.mutable_days() = days;
  result.set_revision(sha256_bytes(contents(result).dump()));
  return result;
}
Json decode_calendar(const data::v1::SettlementCalendar& calendar) {
  auto result = contents(calendar);
  if (calendar.revision() != sha256_bytes(result.dump()))
    throw std::invalid_argument("calendar content revision mismatch");
  result["revision"] = calendar.revision();
  return result;
}
data::v1::SettlementCalendar encode_calendar(const Json& input) {
  require_fields(input, {"version", "type", "contract", "days", "revision"});
  if (!input.at("version").is_number_integer() || input.at("version") != 1 ||
      input.at("type") != "futures.settlement-calendar")
    throw std::invalid_argument("unsupported calendar format");
  data::v1::SettlementCalendar result;
  result.set_version(1);
  result.set_revision(input.at("revision").get<std::string>());
  *result.mutable_contract() = encode_contract(input.at("contract"));
  encode_settlement_days(input.at("days"), *result.mutable_days());
  static_cast<void>(decode_calendar(result));
  return result;
}
Json decode_calendar_snapshot(const data::v1::CalendarCsvSnapshot& input) {
  validate_message(input);
  if (input.version() != 1 || !input.has_contract())
    throw std::invalid_argument("invalid calendar CSV snapshot");
  metadata(input.source_name(), input.source_sha256(), input.contents().size());
  if (input.source_sha256() != sha256_bytes(input.contents()))
    throw std::invalid_argument("calendar CSV snapshot digest mismatch");
  static_cast<void>(Json(input.contents()).dump());
  return {{"version", 1},
          {"source_name", input.source_name()},
          {"source_sha256", input.source_sha256()},
          {"contract", decode_contract(input.contract())},
          {"contents", input.contents()}};
}
data::v1::CalendarCsvSnapshot encode_calendar_snapshot(const Json& input) {
  require_fields(input, {"version", "source_name", "source_sha256", "contract", "contents"});
  if (!input.at("version").is_number_integer() || input.at("version") != 1)
    throw std::invalid_argument("unsupported calendar snapshot version");
  data::v1::CalendarCsvSnapshot result;
  result.set_version(1);
  result.set_source_name(input.at("source_name").get<std::string>());
  result.set_source_sha256(input.at("source_sha256").get<std::string>());
  result.set_contents(input.at("contents").get<std::string>());
  *result.mutable_contract() = encode_contract(input.at("contract"));
  static_cast<void>(decode_calendar_snapshot(result));
  return result;
}
std::string calendar_publication_id(const data::v1::CalendarPublication& p) {
  return sha256_bytes(provenance(p).dump());
}
Json decode_calendar_publication(const data::v1::CalendarPublication& p) {
  auto result = provenance(p);
  if (p.id() != sha256_bytes(result.dump()))
    throw std::invalid_argument("calendar publication identity mismatch");
  result["id"] = p.id();
  return result;
}
data::v1::CalendarPublication encode_calendar_publication(const Json& input) {
  require_fields(input, {"version", "id", "calendar", "source_name", "source_sha256",
                         "source_bytes", "importer"});
  if (!input.at("version").is_number_integer() || input.at("version") != 1 ||
      !input.at("source_bytes").is_number_integer() || input.at("source_bytes") < 1 ||
      input.at("source_bytes") > 1024 * 1024)
    throw std::invalid_argument("invalid calendar publication metadata");
  data::v1::CalendarPublication result;
  result.set_version(1);
  result.set_id(input.at("id").get<std::string>());
  *result.mutable_calendar() = encode_calendar(input.at("calendar"));
  result.set_source_name(input.at("source_name").get<std::string>());
  result.set_source_sha256(input.at("source_sha256").get<std::string>());
  result.set_source_bytes(input.at("source_bytes").get<std::uint64_t>());
  result.set_importer(input.at("importer").get<std::string>());
  static_cast<void>(decode_calendar_publication(result));
  return result;
}
} // namespace asterion::protocol
