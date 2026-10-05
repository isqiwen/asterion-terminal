#include "history_archive.hpp"
#include "history_minutes.hpp"
#include "history_daily.hpp"
#include <asterion/foundation/id.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/file_lock.hpp>
#include <asterion/protocol/data.hpp>
#include <fstream>
#include <algorithm>
#include <optional>
namespace asterion::history_files {
namespace {
void safe(const std::filesystem::path& p) {
  if (std::filesystem::is_symlink(p))
    throw std::invalid_argument("historical archive rejects symbolic links");
}
void require_owned_directory(const std::filesystem::path& root, std::filesystem::path path) {
  if (path.is_absolute()) {
    // Parent aliases such as macOS /var and /private/var name the same archive.
    // Walk the supplied descendants so internal links are still rejected.
    while (path.has_relative_path()) {
      if (path.filename() == "." || path.filename() == "..")
        break;
      safe(path);
      path = path.parent_path();
      if (std::filesystem::equivalent(path, root))
        return;
    }
  }
  throw std::invalid_argument("historical dataset outside archive");
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
Archive::Archive(std::filesystem::path root, Access access)
    : root_(std::move(root)), access_(access) {
  if (!root_.is_absolute())
    throw std::invalid_argument("historical archive requires an absolute directory");
  safe(root_);
  if (std::filesystem::exists(root_ / "index") &&
      !std::filesystem::is_directory(root_ / "named-datasets"))
    throw std::invalid_argument(
        "unsupported historical archive layout; existing data was preserved");
  if (access_ == Access::read_only) {
    for (const auto& directory : {root_, root_ / "named-datasets", root_ / "index"}) {
      safe(directory);
      if (!std::filesystem::is_directory(directory))
        throw std::invalid_argument("historical archive requires existing directories");
    }
    return;
  }
  create_directories_durably(root_);
  safe(root_ / "named-datasets");
  create_directories_durably(root_ / "named-datasets");
  safe(root_ / "index");
  create_directories_durably(root_ / "index");
}
void Archive::writable() const {
  if (access_ == Access::read_only)
    throw std::logic_error("historical archive is read only");
}
PluginDescriptor Archive::descriptor() const {
  return {"asterion.storage.history.filesystem", PluginKind::storage, plugin_contract_version, {}};
}
std::filesystem::path Archive::directory(const HistoryIdentity& contract, const std::string& source,
                                         unsigned interval, const std::string& acquisition) const {
  writable();
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
    create_directories_durably(p);
  }
  return p;
}
void Archive::publish_verified(const data::v1::HistoryRecord& record) {
  writable();
  const auto item = summary(record);
  digest(item.id);
  const auto path = std::filesystem::path(record.has_minutes() ? record.minute_result().directory()
                                                               : record.daily_result().directory());
  require_owned_directory(root_, path);
  FileLock lock(root_, "archive.lock");
  const auto index = root_ / "index" / (item.id + ".pb");
  safe(index);
  // Prepared evidence owns file verification. Reconfirming a publication
  // checks its immutable index record without loading the data again.
  if (std::filesystem::exists(index)) {
    (void)get(item.id);
    sync_directory(index.parent_path());
    return;
  }
  replace_file_durably(index, record.SerializeAsString());
}
data::v1::HistoryRecord Archive::get(const std::string& id) const {
  digest(id);
  const auto index = root_ / "index" / (id + ".pb");
  safe(index);
  if (!std::filesystem::is_regular_file(index) || std::filesystem::file_size(index) > 262144)
    throw std::invalid_argument("historical dataset is unavailable");
  std::ifstream file(index, std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(file)), {});
  data::v1::HistoryRecord record;
  if (!record.ParseFromString(bytes) || summary(record).id != id)
    throw std::invalid_argument("historical archive revision mismatch");
  const auto path = std::filesystem::path(record.has_minutes() ? record.minute_result().directory()
                                                               : record.daily_result().directory());
  require_owned_directory(root_, path);
  return record;
}
std::vector<HistoryDataset> Archive::datasets(const HistoryFilter& filter) const {
  if (!filter.contract_id.empty())
    (void)HistoryIdentity::parse(filter.contract_id);
  if (!filter.source.empty())
    validate_history_source(filter.source);
  safe(root_ / "archive.lock");
  std::optional<FileLock> lock;
  if (access_ == Access::writer || std::filesystem::exists(root_ / "archive.lock"))
    lock.emplace(root_, "archive.lock",
                 access_ == Access::writer ? FileLock::Access::shared
                                           : FileLock::Access::shared_existing);
  else if (!std::filesystem::is_empty(root_ / "index"))
    throw std::invalid_argument("historical archive coordination file is missing");
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
void Archive::save_named_dataset(const data::v1::NamedDataset& value) {
  writable();
  protocol::validate_named_dataset(value);
  for (const auto& input : value.selections()) {
    for (const auto& id : input.source_dataset_ids())
      (void)get(id);
    for (const auto& id : input.settlement_dataset_ids())
      (void)get(id);
  }
  FileLock lock(root_, "datasets.lock");
  const auto path = root_ / "named-datasets" / (value.id() + ".pb");
  safe(path);
  if (std::filesystem::exists(path)) {
    (void)named_dataset(value.id());
    sync_directory(path.parent_path());
    return;
  }
  replace_file_durably(path, value.SerializeAsString());
}
bool Archive::has_named_dataset(const std::string& id) const {
  digest(id);
  const auto path = root_ / "named-datasets" / (id + ".pb");
  safe(path);
  if (!std::filesystem::exists(path))
    return false;
  (void)named_dataset(id);
  return true;
}
data::v1::NamedDataset Archive::named_dataset(const std::string& id) const {
  digest(id);
  const auto path = root_ / "named-datasets" / (id + ".pb");
  safe(path);
  if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) > 262144)
    throw std::invalid_argument("saved dataset is unavailable");
  std::ifstream file(path, std::ios::binary);
  std::string bytes((std::istreambuf_iterator<char>(file)), {});
  data::v1::NamedDataset value;
  if (!value.ParseFromString(bytes) || value.id() != id)
    throw std::invalid_argument("saved dataset revision mismatch");
  protocol::validate_named_dataset(value);
  return value;
}
data::v1::NamedDatasets Archive::named_datasets() const {
  safe(root_ / "datasets.lock");
  std::optional<FileLock> lock;
  if (access_ == Access::writer || std::filesystem::exists(root_ / "datasets.lock"))
    lock.emplace(root_, "datasets.lock",
                 access_ == Access::writer ? FileLock::Access::shared
                                           : FileLock::Access::shared_existing);
  else if (!std::filesystem::is_empty(root_ / "named-datasets"))
    throw std::invalid_argument("historical archive coordination file is missing");
  std::vector<data::v1::NamedDataset> rows;
  for (const auto& entry : std::filesystem::directory_iterator(root_ / "named-datasets")) {
    if (entry.path().extension() != ".pb" || rows.size() >= 1000)
      throw std::invalid_argument("invalid saved dataset");
    rows.push_back(named_dataset(entry.path().stem().string()));
  }
  std::ranges::sort(rows, [](const auto& a, const auto& b) {
    return std::make_pair(a.name(), a.id()) < std::make_pair(b.name(), b.id());
  });
  data::v1::NamedDatasets result;
  for (auto& row : rows)
    *result.add_items() = std::move(row);
  return result;
}
} // namespace asterion::history_files
