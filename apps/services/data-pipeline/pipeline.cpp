#include "pipeline.hpp"
#include "csv_market_data.hpp"
#include "file_journal.hpp"
#include <asterion/domain/futures.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <fstream>
#include <stdexcept>
namespace asterion::data_pipeline {
namespace fs = std::filesystem;
namespace {
void cancelled(std::stop_token stop) {
  if (stop.stop_requested())
    throw std::runtime_error("data import cancelled");
}
void directory_check(const fs::path& directory) {
  if (!directory.is_absolute() || !fs::is_directory(directory) || fs::is_symlink(directory))
    throw std::invalid_argument(
        "publication requires an existing absolute directory without symlinks");
  if (fs::exists(directory / "pending.tmp") || fs::is_symlink(directory / "pending.tmp"))
    throw std::invalid_argument(
        "incomplete publication write; preserve the directory for inspection");
}
data::v1::DatasetPublication record(const Json& value) {
  require_fields(value, {"version", "type", "publication"});
  if (!value.at("version").is_number_integer() || value.at("version") != 1 ||
      value.at("type") != "dataset.published")
    throw std::invalid_argument("unsupported publication record");
  return protocol::encode_publication(value.at("publication"));
}
} // namespace
data::v1::CsvSnapshot capture_csv(const data::v1::CsvImport& input, std::stop_token stop) {
  cancelled(stop);
  protocol::validate_message(input);
  if (input.version() != 1 || !input.has_contract() || input.source_sha256().size() != 64 ||
      input.source_sha256().find_first_not_of("0123456789abcdef") != std::string::npos)
    throw std::invalid_argument(
        "CSV import requires version, contract and expected source SHA-256");
  const auto& c = input.contract();
  if (!c.has_price_increment() || !c.has_quantity_increment() || !c.has_multiplier())
    throw std::invalid_argument("incomplete import contract");
  Instrument instrument{{c.venue(), c.symbol()},
                        AssetClass::futures,
                        c.currency(),
                        Decimal::from_raw(c.price_increment().units()),
                        Decimal::from_raw(c.quantity_increment().units()),
                        Decimal::from_raw(c.multiplier().units())};
  FuturesContract{instrument, c.product(), c.delivery_month()}.validate();
  const auto& name = input.source_path();
  if (name.find('\0') != std::string::npos)
    throw std::invalid_argument("invalid CSV path");
  const fs::path path(std::u8string(name.begin(), name.end()));
  constexpr std::size_t limit = 32 * 1024 * 1024;
  if (!path.is_absolute() || fs::is_symlink(path) || !fs::is_regular_file(path) ||
      fs::file_size(path) > limit)
    throw std::invalid_argument("CSV source must be an absolute regular file of at most 32 MiB");
  std::ifstream file(path, std::ios::binary);
  if (!file)
    throw std::runtime_error("cannot read CSV source");
  std::string bytes(limit + 1, '\0');
  file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  const auto size = static_cast<std::size_t>(file.gcount());
  if (file.bad() || !file.eof() || size > limit)
    throw std::runtime_error("CSV source exceeds limit or cannot be read completely");
  bytes.resize(size);
  const auto hash = sha256_bytes(bytes);
  if (hash != input.source_sha256())
    throw std::invalid_argument("CSV source changed or digest does not match");
  cancelled(stop);
  data::v1::CsvSnapshot snapshot;
  snapshot.set_version(1);
  *snapshot.mutable_contract() = c;
  const auto filename = path.filename().u8string();
  snapshot.set_source_name(std::string(filename.begin(), filename.end()));
  snapshot.set_source_sha256(hash);
  snapshot.set_contents(std::move(bytes));
  static_cast<void>(protocol::decode_csv_snapshot(snapshot));
  return snapshot;
}
data::v1::DatasetPublication import_csv(const data::v1::CsvImport& input, std::stop_token stop) {
  return import_snapshot(capture_csv(input, stop), stop);
}
data::v1::DatasetPublication
import_snapshot(const data::v1::CsvSnapshot& input, std::stop_token stop,
                const std::function<void(std::size_t, std::size_t)>& progress) {
  cancelled(stop);
  static_cast<void>(protocol::decode_csv_snapshot(input));
  const auto total = input.contents().size();
  if (progress)
    progress(0, total);
  const auto& c = input.contract();
  Instrument instrument{{c.venue(), c.symbol()},
                        AssetClass::futures,
                        c.currency(),
                        Decimal::from_raw(c.price_increment().units()),
                        Decimal::from_raw(c.quantity_increment().units()),
                        Decimal::from_raw(c.multiplier().units())};
  CsvMarketData provider(instrument, input.contents());
  provider.start();
  google::protobuf::RepeatedPtrField<protocol::v1::Tick> ticks;
  while (auto tick = provider.next()) {
    cancelled(stop);
    if (ticks.size() >= 10000)
      throw std::invalid_argument("publication currently supports at most "
                                  "10000 events; no rows were dropped");
    auto* target = ticks.Add();
    target->set_timestamp_ns(tick->timestamp_ns);
    target->mutable_price()->set_units(tick->price.raw());
    target->mutable_quantity()->set_units(tick->quantity.raw());
    if (progress)
      progress(0, total);
  }
  provider.stop();
  cancelled(stop);
  data::v1::DatasetPublication result;
  result.set_version(1);
  *result.mutable_dataset() = protocol::make_trade_dataset(c, ticks);
  result.set_source_name(input.source_name());
  result.set_source_sha256(input.source_sha256());
  result.set_source_bytes(total);
  result.set_importer("asterion.csv.trades.v1");
  result.set_id(protocol::publication_id(result));
  if (progress)
    progress(total, total);
  cancelled(stop);
  return result;
}
void verify_result(const data::v1::CsvSnapshot& input, const data::v1::DatasetPublication& result) {
  if (protocol::decode_publication(result) != protocol::decode_publication(import_snapshot(input)))
    throw std::invalid_argument("publication does not match immutable CSV input");
}
bool publish(const data::v1::DatasetPublication& publication, const fs::path& directory) {
  const auto payload = protocol::decode_publication(publication);
  directory_check(directory);
  FileJournal journal(directory);
  journal.start();
  const auto records = journal.read();
  if (!records.empty()) {
    if (records.size() != 1 || protocol::decode_publication(record(records.front())) != payload)
      throw std::invalid_argument("publication directory already contains different data");
    return false;
  }
  journal.append({{"version", 1}, {"type", "dataset.published"}, {"publication", payload}});
  return true;
}
data::v1::DatasetPublication read(const fs::path& directory) {
  directory_check(directory);
  FileJournal journal(directory);
  journal.start();
  const auto records = journal.read();
  if (records.size() != 1)
    throw std::invalid_argument("directory has no single committed publication");
  return record(records.front());
}
Json summary(const data::v1::DatasetPublication& p) {
  static_cast<void>(protocol::decode_publication(p));
  return {{"id", p.id()},
          {"revision", p.dataset().revision()},
          {"count", p.dataset().ticks_size()},
          {"source_name", p.source_name()},
          {"source_sha256", p.source_sha256()},
          {"source_bytes", p.source_bytes()},
          {"first_timestamp_ns", std::to_string(p.dataset().ticks(0).timestamp_ns())},
          {"last_timestamp_ns", std::to_string(p.dataset().ticks().rbegin()->timestamp_ns())}};
}
} // namespace asterion::data_pipeline
