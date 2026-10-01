#include "history_archive.hpp"
#include "history_minutes.hpp"
#include "history_daily.hpp"
#include <asterion/foundation/id.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/file_lock.hpp>
#include <asterion/protocol/data.hpp>
#include <fstream>
#include <algorithm>
namespace asterion::history_files {
namespace {
void safe(const std::filesystem::path& p) {
  if (std::filesystem::is_symlink(p))
    throw std::invalid_argument("historical archive rejects symbolic links");
}
void digest(const std::string& id) {
  if (id.size() != 64 || id.find_first_not_of("0123456789abcdef") != std::string::npos)
    throw std::invalid_argument("invalid historical dataset revision");
}
HistoryDataset summary(const data::v1::HistoryRecord& record) {
  protocol::validate_message(record);
  if (record.version() != 1)
    throw std::invalid_argument("unsupported historical archive record");
  if (record.has_minutes() && record.has_minute_result()) {
    const auto range = minute_range(record.minutes());
    const auto& result = record.minute_result();
    return {result.manifest_sha256(),
            range.instrument,
            range.source,
            result.manifest_sha256(),
            format_shanghai_time(range.begin_ns),
            format_shanghai_time(range.end_ns),
            range.interval_minutes,
            result.rows()};
  }
  if (record.has_daily() && record.has_daily_result()) {
    const auto range = daily_range(record.daily());
    const auto& result = record.daily_result();
    return {result.manifest_sha256(),
            range.instrument,
            range.source,
            result.manifest_sha256(),
            format_trading_date(range.begin),
            format_trading_date(range.end),
            0,
            result.rows()};
  }
  throw std::invalid_argument("invalid historical archive record");
}
} // namespace
Archive::Archive(std::filesystem::path root) : root_(std::move(root)) {
  if (!root_.is_absolute())
    throw std::invalid_argument("historical archive requires an absolute directory");
  safe(root_);
  std::filesystem::create_directories(root_);
  safe(root_ / "index");
  std::filesystem::create_directory(root_ / "index");
}
PluginDescriptor Archive::descriptor() const {
  return {"asterion.storage.history.filesystem", PluginKind::storage, plugin_contract_version, {}};
}
std::filesystem::path Archive::directory(const HistoryIdentity& contract, const std::string& source,
                                         unsigned interval, const std::string& acquisition) const {
  contract.validate();
  validate_history_source(source);
  validate_id(acquisition);
  if (acquisition == "." || acquisition == "..")
    throw std::invalid_argument("invalid task id");
  auto p = root_;
  for (const auto& part :
       std::vector<std::string>{contract.venue, contract.product, contract.delivery_month, source,
                                interval ? std::to_string(interval) + "m" : "daily", acquisition}) {
    p /= part;
    safe(p);
    std::filesystem::create_directory(p);
  }
  return p;
}
void Archive::publish(const data::v1::HistoryRecord& record) {
  const auto item = summary(record);
  digest(item.id);
  const auto path = std::filesystem::path(record.has_minutes() ? record.minute_result().directory()
                                                               : record.daily_result().directory());
  const auto relative = path.lexically_relative(root_);
  if (relative.empty() || relative.native().starts_with(".."))
    throw std::invalid_argument("historical dataset outside archive");
  auto checked = root_;
  for (const auto& part : relative) {
    if (part == ".." || part == ".")
      throw std::invalid_argument("historical dataset outside archive");
    checked /= part;
    safe(checked);
  }
  if (record.has_minutes())
    verify_minute_result(record.minutes(), record.minute_result());
  else
    verify_daily_result(record.daily(), record.daily_result());
  FileLock lock(root_, "archive.lock");
  const auto index = root_ / "index" / (item.id + ".pb");
  safe(index);
  if (std::filesystem::exists(index)) {
    (void)get(item.id);
    return;
  }
  replace_file_durably(index, record.SerializeAsString());
}
data::v1::HistoryRecord Archive::get(const std::string& id) const {
  digest(id);
  const auto index = root_ / "index" / (id + ".pb");
  safe(index);
  if (!std::filesystem::is_regular_file(index) || std::filesystem::file_size(index) > 65536)
    throw std::invalid_argument("historical dataset is unavailable");
  std::ifstream file(index, std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(file)), {});
  data::v1::HistoryRecord record;
  if (!record.ParseFromString(bytes) || summary(record).id != id)
    throw std::invalid_argument("historical archive revision mismatch");
  const auto path = std::filesystem::path(record.has_minutes() ? record.minute_result().directory()
                                                               : record.daily_result().directory());
  auto relative = path.lexically_relative(root_);
  if (relative.empty() || relative.native().starts_with(".."))
    throw std::invalid_argument("historical dataset outside archive");
  auto checked = root_;
  for (const auto& part : relative) {
    if (part == ".." || part == ".")
      throw std::invalid_argument("historical dataset outside archive");
    checked /= part;
    safe(checked);
  }
  return record;
}
std::vector<HistoryDataset> Archive::datasets(const HistoryFilter& filter) const {
  if (!filter.contract_id.empty())
    (void)HistoryIdentity::parse(filter.contract_id);
  if (!filter.source.empty())
    validate_history_source(filter.source);
  FileLock lock(root_, "archive.lock", FileLock::Access::shared);
  std::vector<HistoryDataset> result;
  for (const auto& file : std::filesystem::directory_iterator(root_ / "index")) {
    safe(file.path());
    if (file.path().extension() != ".pb")
      throw std::invalid_argument("unknown historical archive entry");
    const auto item = summary(get(file.path().stem().string()));
    if ((filter.venue.empty() || filter.venue == item.contract.venue) &&
        (filter.product.empty() || filter.product == item.contract.product) &&
        (filter.contract_id.empty() || filter.contract_id == item.contract.key()) &&
        (filter.source.empty() || filter.source == item.source))
      result.push_back(item);
    if (result.size() > 10000)
      throw std::invalid_argument("historical archive query exceeds limit; narrow filters");
  }
  std::ranges::sort(result, {}, &HistoryDataset::id);
  return result;
}
} // namespace asterion::history_files
