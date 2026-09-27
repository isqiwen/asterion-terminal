#include "calendar.hpp"
#include "csv_settlement_calendar.hpp"
#include "file_journal.hpp"
#include <asterion/kernel/process/artifact.hpp>
#include <fstream>
#include <stdexcept>
namespace asterion::data_pipeline {
namespace fs = std::filesystem;
namespace {
void cancelled(std::stop_token stop) {
  if (stop.stop_requested())
    throw std::runtime_error("calendar import cancelled");
}
void directory_check(const fs::path& directory) {
  if (!directory.is_absolute() || !fs::is_directory(directory) || fs::is_symlink(directory) ||
      fs::exists(directory / "pending.tmp") || fs::is_symlink(directory / "pending.tmp"))
    throw std::invalid_argument(
        "calendar publication requires an intact existing absolute directory");
}
data::v1::CalendarPublication record(const Json& value) {
  require_fields(value, {"version", "type", "publication"});
  if (!value.at("version").is_number_integer() || value.at("version") != 1 ||
      value.at("type") != "calendar.published")
    throw std::invalid_argument("unsupported calendar publication record");
  return protocol::encode_calendar_publication(value.at("publication"));
}
} // namespace
data::v1::CalendarCsvSnapshot capture_calendar_csv(const data::v1::CsvImport& input,
                                                   std::stop_token stop) {
  cancelled(stop);
  protocol::validate_message(input);
  if (input.version() != 1 || !input.has_contract() ||
      input.source_path().find('\0') != std::string::npos)
    throw std::invalid_argument("invalid calendar import specification");
  const auto& name = input.source_path();
  const fs::path path(std::u8string(name.begin(), name.end()));
  constexpr std::size_t limit = 1024 * 1024;
  if (!path.is_absolute() || fs::is_symlink(path) || !fs::is_regular_file(path) ||
      fs::file_size(path) > limit)
    throw std::invalid_argument("calendar CSV requires an absolute regular file of at most 1 MiB");
  std::ifstream file(path, std::ios::binary);
  if (!file)
    throw std::runtime_error("cannot open calendar CSV");
  std::string bytes(limit + 1, '\0');
  file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  bytes.resize(static_cast<std::size_t>(file.gcount()));
  if (file.bad() || !file.eof() || bytes.size() > limit ||
      sha256_bytes(bytes) != input.source_sha256())
    throw std::invalid_argument("calendar CSV changed or digest/size mismatch");
  data::v1::CalendarCsvSnapshot result;
  result.set_version(1);
  *result.mutable_contract() = input.contract();
  const auto filename = path.filename().u8string();
  result.set_source_name(std::string(filename.begin(), filename.end()));
  result.set_source_sha256(input.source_sha256());
  result.set_contents(std::move(bytes));
  static_cast<void>(protocol::decode_calendar_snapshot(result));
  cancelled(stop);
  return result;
}
data::v1::CalendarPublication import_calendar_snapshot(const data::v1::CalendarCsvSnapshot& input,
                                                       std::stop_token stop) {
  cancelled(stop);
  static_cast<void>(protocol::decode_calendar_snapshot(input));
  const auto& c = input.contract();
  CsvSettlementCalendar provider({{c.venue(), c.symbol()},
                                  AssetClass::futures,
                                  c.currency(),
                                  Decimal::from_raw(c.price_increment().units()),
                                  Decimal::from_raw(c.quantity_increment().units()),
                                  Decimal::from_raw(c.multiplier().units())},
                                 input.contents());
  provider.start();
  const auto parsed = provider.read(stop);
  provider.stop();
  google::protobuf::RepeatedPtrField<data::v1::SettlementDay> days;
  for (const auto& row : parsed) {
    auto* day = days.Add();
    day->set_trading_day(row.schedule.trading_day());
    day->set_schedule_source(row.schedule_source);
    day->set_settlement_source(row.settlement_source);
    day->mutable_settlement_price()->set_units(row.settlement_price.raw());
    for (const auto& interval : row.schedule.sessions()) {
      auto* session = day->add_sessions();
      session->set_begin_ns(interval.begin_ns);
      session->set_end_ns(interval.end_ns);
    }
  }
  data::v1::CalendarPublication result;
  result.set_version(1);
  *result.mutable_calendar() = protocol::make_settlement_calendar(c, days);
  result.set_source_name(input.source_name());
  result.set_source_sha256(input.source_sha256());
  result.set_source_bytes(input.contents().size());
  result.set_importer("asterion.csv.settlement.v1");
  result.set_id(protocol::calendar_publication_id(result));
  cancelled(stop);
  return result;
}
void verify_calendar_result(const data::v1::CalendarCsvSnapshot& input,
                            const data::v1::CalendarPublication& result) {
  if (protocol::decode_calendar_publication(result) !=
      protocol::decode_calendar_publication(import_calendar_snapshot(input)))
    throw std::invalid_argument("calendar publication differs from immutable source");
}
bool publish_calendar(const data::v1::CalendarPublication& value, const fs::path& directory) {
  const auto payload = protocol::decode_calendar_publication(value);
  directory_check(directory);
  FileJournal journal(directory);
  journal.start();
  const auto records = journal.read();
  if (!records.empty()) {
    if (records.size() != 1 ||
        protocol::decode_calendar_publication(record(records.front())) != payload)
      throw std::invalid_argument("calendar publication already contains different data");
    return false;
  }
  journal.append({{"version", 1}, {"type", "calendar.published"}, {"publication", payload}});
  return true;
}
data::v1::CalendarPublication read_calendar(const fs::path& directory) {
  directory_check(directory);
  FileJournal journal(directory);
  journal.start();
  const auto records = journal.read();
  if (records.size() != 1)
    throw std::invalid_argument("calendar publication must contain one committed record");
  return record(records.front());
}
Json calendar_summary(const data::v1::CalendarPublication& value) {
  auto result = protocol::decode_calendar_publication(value);
  result["day_count"] = value.calendar().days_size();
  return result;
}
} // namespace asterion::data_pipeline
