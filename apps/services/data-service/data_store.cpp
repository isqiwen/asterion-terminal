#include "data_store.hpp"
#include "history_daily.hpp"
#include "history_providers.hpp"
#include "history_minutes.hpp"
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/protocol/data.hpp>
#include <fstream>

namespace asterion::data {
namespace fs = std::filesystem;
namespace {
fs::path checked_root(fs::path root) {
  if (!root.is_absolute() || !fs::is_directory(root) || fs::is_symlink(root))
    throw std::invalid_argument("data service requires an existing absolute directory");
  return root;
}
std::string read_bytes(const fs::path& path) {
  if (fs::is_symlink(path) || !fs::is_regular_file(path) || fs::file_size(path) > 262144)
    throw std::invalid_argument("data service record is unavailable");
  std::ifstream stream(path, std::ios::binary);
  std::string bytes{std::istreambuf_iterator<char>(stream), {}};
  if (stream.bad())
    throw std::runtime_error("data service record read failed");
  return bytes;
}
template <class Message> Message read_message(const fs::path& path) {
  Message message;
  if (!message.ParseFromString(read_bytes(path)))
    throw std::invalid_argument("invalid data service record");
  protocol::validate_message(message);
  return message;
}
std::string dataset_id(const v1::HistoryRecord& record) {
  return record.has_minutes() ? record.minute_result().manifest_sha256()
                              : record.daily_result().manifest_sha256();
}
void store_once(const fs::path& path, const std::string& bytes) {
  if (fs::exists(path)) {
    if (read_bytes(path) != bytes)
      throw Error(ErrorCode::conflict, "data operation identity has different content");
    // A prior process may have stopped after rename and before directory sync.
    sync_directory(path.parent_path());
    return;
  }
  replace_file_durably(path, bytes);
}
} // namespace
Store::Store(fs::path root, std::string instance, std::string task_instance)
    : root_(checked_root(std::move(root))), instance_(std::move(instance)),
      task_instance_(std::move(task_instance)), ownership_(root_, "data.lock"), archive_([&] {
        validate_id(instance_);
        validate_id(task_instance_);
        const Json identity{
            {"version", 1}, {"data_instance", instance_}, {"task_instance", task_instance_}};
        const auto path = root_ / "instance.json";
        if (fs::exists(path)) {
          if (parse_json(read_bytes(path)) != identity)
            throw std::invalid_argument("data service instance or task binding mismatch");
        } else {
          for (const auto& file : fs::directory_iterator(root_))
            if (file.path().filename() != "data.lock")
              throw std::invalid_argument("data directory has no supported instance identity");
          replace_file_durably(path, identity.dump());
        }
        create_directories_durably(root_ / "downloads");
        create_directories_durably(root_ / "authorizations");
        return root_ / "history";
      }()) {}
v1::DownloadBudgetPolicy
Store::verify_budget(const v1::DownloadBudgetConfiguration& request) const {
  protocol::validate_message(request);
  for (const auto& source : history_providers::sources()) {
    if (source.id != request.source())
      continue;
    if (!request.requests_per_minute() ||
        request.requests_per_minute() > source.max_requests_per_minute ||
        (source.credential_required && request.credential().empty()))
      throw std::invalid_argument("invalid download request budget");
    v1::DownloadBudgetPolicy result;
    result.set_version(1);
    result.set_provider_id(source.plugin_id);
    result.set_requests_per_minute(request.requests_per_minute());
    return result;
  }
  throw std::invalid_argument("historical data source is unavailable");
}
v1::StoredDownloadAuthorization
Store::download_authorization(const v1::DownloadIdentity& identity) const {
  const auto allocated = read_message<v1::AllocatedDownload>(operation(identity) / "allocation.pb");
  if (allocated.request().identity().SerializeAsString() != identity.SerializeAsString())
    throw std::invalid_argument("download allocation identity mismatch");
  const auto stored = read_authorization(allocated.request().authorization_id());
  if (stored.authorization().task_id() != identity.task_id())
    throw Error(ErrorCode::conflict, "download allocation does not match authorization");
  return stored;
}
fs::path Store::authorization_path(const std::string& id) const {
  if (id.size() != 64 || id.find_first_not_of("0123456789abcdef") != std::string::npos)
    throw std::invalid_argument("invalid download authorization reference");
  return root_ / "authorizations" / (id + ".pb");
}
Store::VerifiedAuthorization
Store::verify_authorization(const v1::DownloadAuthorizationRequest& request) const {
  protocol::validate_message(request);
  validate_id(request.task_id());
  if (request.task_instance() != task_instance_)
    throw Error(ErrorCode::conflict, "download authorization does not match task binding");
  const auto id = sha256_bytes(instance_ + "/" + task_instance_ + "/" + request.task_id());
  if (fs::exists(authorization_path(id))) {
    auto existing = read_authorization(id);
    const auto& fixed = existing.authorization();
    const bool same_definition =
        request.has_minutes()
            ? fixed.has_minutes() &&
                  request.minutes().SerializeAsString() == fixed.minutes().SerializeAsString()
            : request.has_daily() && fixed.has_daily() &&
                  request.daily().SerializeAsString() == fixed.daily().SerializeAsString();
    if (!same_definition || request.credential() != existing.credential())
      throw Error(ErrorCode::conflict, "data operation identity has different content");
    return VerifiedAuthorization(std::move(existing));
  }
  v1::StoredDownloadAuthorization stored;
  auto* authorization = stored.mutable_authorization();
  authorization->set_version(2);
  authorization->set_data_instance(instance_);
  authorization->set_task_instance(task_instance_);
  authorization->set_task_id(request.task_id());
  authorization->set_id(id);
  if (request.has_minutes()) {
    const auto& input = request.minutes();
    const auto range = history_files::minute_range(input);
    history_providers::validate_request(input.source(), range.instrument, input.interval_minutes(),
                                        input.requests_per_minute());
    auto provider = history_providers::minutes(input.source(), request.credential());
    authorization->set_provider_artifact(history_providers::artifact(input.source()));
    *authorization->mutable_minutes() = input;
  } else if (request.has_daily()) {
    const auto& input = request.daily();
    const auto range = history_files::daily_range(input);
    history_providers::validate_request(input.source(), range.instrument, 0,
                                        input.requests_per_minute());
    auto provider = history_providers::daily(input.source(), request.credential());
    authorization->set_provider_artifact(history_providers::artifact(input.source()));
    *authorization->mutable_daily() = input;
  } else
    throw std::invalid_argument("download definition is missing");
  const auto source = request.has_minutes() ? request.minutes().source() : request.daily().source();
  for (const auto& item : history_providers::sources())
    if (item.id == source) {
      authorization->set_provider_id(item.plugin_id);
      break;
    }
  stored.set_credential(request.credential());
  return VerifiedAuthorization(std::move(stored));
}
v1::DownloadAuthorization Store::authorize(const VerifiedAuthorization& verified) {
  const auto& stored = verified.value_;
  const auto& authorization = stored.authorization();
  if (authorization.data_instance() != instance_ || authorization.task_instance() != task_instance_)
    throw Error(ErrorCode::conflict, "download authorization does not match task binding");
  const auto path = authorization_path(authorization.id());
  // One private atomic record is the commit point. A repeated capture must use
  // the same definition, plugin and secret, never the current default token.
  store_once(path, stored.SerializeAsString());
  return authorization;
}
v1::StoredDownloadAuthorization Store::read_authorization(const std::string& id) const {
  const auto stored = read_message<v1::StoredDownloadAuthorization>(authorization_path(id));
  const auto& value = stored.authorization();
  if (value.version() != 2 || value.id() != id || value.data_instance() != instance_ ||
      value.task_instance() != task_instance_ ||
      sha256_bytes(instance_ + "/" + task_instance_ + "/" + value.task_id()) != id ||
      value.provider_artifact().size() != 64 ||
      value.provider_artifact().find_first_not_of("0123456789abcdef") != std::string::npos)
    throw std::invalid_argument("invalid stored download authorization");
  validate_id(value.task_id());
  validate_id(value.provider_id());
  // Metadata reads do not load the provider or depend on its current availability.
  if (value.has_minutes())
    (void)history_files::minute_range(value.minutes());
  else if (value.has_daily())
    (void)history_files::daily_range(value.daily());
  else
    throw std::invalid_argument("invalid stored download authorization");
  return stored;
}
v1::DownloadAuthorization Store::authorization(const std::string& id) const {
  const auto stored = read_authorization(id);
  // Complete any capture whose reply was lost after rename but before directory sync.
  sync_directory(root_ / "authorizations");
  return stored.authorization();
}
fs::path Store::operation(const v1::DownloadIdentity& identity) const {
  protocol::validate_message(identity);
  validate_id(identity.task_id());
  if (identity.data_instance() != instance_ || identity.task_instance() != task_instance_ ||
      !identity.attempt())
    throw Error(ErrorCode::conflict, "download identity does not match data binding");
  return root_ / "downloads" / sha256_bytes(identity.SerializeAsString());
}
BarDatasetSources Store::sources(const v1::BarDatasetRequest& request) const {
  protocol::decode_bar_dataset_request(request);
  BarDatasetSources result;
  result.request = request;
  for (const auto& id : request.source_dataset_ids())
    result.sources.push_back(archive_.get(id));
  for (const auto& id : request.settlement_dataset_ids())
    result.settlements.push_back(archive_.get(id));
  return result;
}
Store::VerifiedDataset Store::verify_dataset(const v1::NamedDataset& value) const {
  protocol::validate_named_dataset(value);
  std::size_t bars = 0;
  std::vector<std::string> days;
  for (int i = 0; i < value.selections_size(); ++i) {
    const auto resolved = resolve_bar_dataset(sources(value.selections(i)));
    if (resolved.revision() != value.content_revisions(i))
      throw std::invalid_argument("saved dataset revision mismatch");
    bars += static_cast<std::size_t>(resolved.bars_size());
    std::vector<std::string> current;
    for (const auto& day : resolved.days())
      current.push_back(day.trading_day());
    if (i && days != current)
      throw std::invalid_argument("saved dataset trading days differ");
    days = std::move(current);
  }
  if (bars > protocol::max_dataset_bars)
    throw std::invalid_argument("dataset exceeds 200000 bars; narrow the date range");
  return VerifiedDataset(value, !archive_.has_named_dataset(value.id()));
}
void Store::save(const VerifiedDataset& verified) {
  archive_.save_named_dataset(verified.value_);
}
Store::VerifiedAllocation Store::verify_allocation(const v1::DownloadAllocation& request) const {
  protocol::validate_message(request);
  const auto path = operation(request.identity());
  const auto authorized = authorization(request.authorization_id());
  if (authorized.task_id() != request.identity().task_id() ||
      (authorized.has_minutes()
           ? !request.has_minutes() ||
                 authorized.minutes().SerializeAsString() != request.minutes().SerializeAsString()
           : !request.has_daily() ||
                 authorized.daily().SerializeAsString() != request.daily().SerializeAsString()))
    throw Error(ErrorCode::conflict, "download allocation does not match authorization");
  std::optional<std::string> directory;
  if (fs::exists(path / "allocation.pb")) {
    const auto saved = read_message<v1::AllocatedDownload>(path / "allocation.pb");
    if (saved.request().SerializeAsString() != request.SerializeAsString())
      throw Error(ErrorCode::conflict, "data operation identity has different content");
    if (!fs::path(saved.directory()).is_absolute())
      throw std::invalid_argument("invalid allocated download directory");
    directory = saved.directory();
  }
  if (request.has_minutes()) {
    const auto range = history_files::minute_range(request.minutes());
    return VerifiedAllocation(request, range.instrument, range.source, range.interval_minutes,
                              std::move(directory));
  }
  if (request.has_daily()) {
    const auto range = history_files::daily_range(request.daily());
    return VerifiedAllocation(request, range.instrument, range.source, 0, std::move(directory));
  }
  throw std::invalid_argument("download definition is missing");
}
v1::DownloadDirectory Store::allocate(const VerifiedAllocation& verified) {
  const auto& request = verified.value_;
  const auto path = operation(request.identity());
  const auto directory = archive_.directory(verified.contract_, verified.source_,
                                            verified.interval_, path.filename().string());
  v1::DownloadDirectory result;
  if (verified.directory_) {
    // The receipt owns the confirmed path. OS aliases do not allocate a new version.
    if (!fs::equivalent(*verified.directory_, directory))
      throw Error(ErrorCode::conflict,
                  "allocated download directory does not belong to this operation");
    sync_directory(path);
    result.set_directory(*verified.directory_);
    return result;
  }
  create_directories_durably(path);
  v1::AllocatedDownload allocation;
  *allocation.mutable_request() = request;
  allocation.set_directory(directory.string());
  store_once(path / "allocation.pb", allocation.SerializeAsString());
  result.set_directory(directory.string());
  return result;
}
v1::DownloadCredentials Store::credentials(const v1::DownloadIdentity& identity) const {
  const auto stored = download_authorization(identity);
  const auto& authorized = stored.authorization();
  v1::DownloadCredentials result;
  *result.mutable_identity() = identity;
  result.set_authorization_id(authorized.id());
  result.set_credential(stored.credential());
  return result;
}
Store::VerifiedDownload Store::verify_download(const v1::DownloadPreparation& request) const {
  protocol::validate_message(request);
  const auto path = operation(request.identity());
  const auto allocated = read_message<v1::AllocatedDownload>(path / "allocation.pb");
  const auto& allocation = allocated.request();
  if (allocation.identity().SerializeAsString() != request.identity().SerializeAsString())
    throw std::invalid_argument("download allocation identity mismatch");
  const auto& owned = allocated.directory();
  const auto& record = request.record();
  if (record.version() != 1)
    throw std::invalid_argument("unsupported historical archive record");
  if (allocation.has_minutes() && record.has_minutes() && record.has_minute_result()) {
    if (record.minutes().SerializeAsString() != allocation.minutes().SerializeAsString() ||
        record.minute_result().directory() != owned)
      throw std::invalid_argument("download candidate does not match its allocation");
    history_files::verify_minute_result(record.minutes(), record.minute_result());
  } else if (allocation.has_daily() && record.has_daily() && record.has_daily_result()) {
    if (record.daily().SerializeAsString() != allocation.daily().SerializeAsString() ||
        record.daily_result().directory() != owned)
      throw std::invalid_argument("download candidate does not match its allocation");
    history_files::verify_daily_result(record.daily(), record.daily_result());
  } else
    throw std::invalid_argument("download candidate does not match its allocation");
  return VerifiedDownload(request);
}
v1::PreparedDownload Store::prepare(const VerifiedDownload& verified) {
  const auto& request = verified.value_;
  const auto path = operation(request.identity());
  const auto& record = request.record();
  // Files are already under the warehouse. Persisting this record accepts
  // custody without exposing the version in the public archive index.
  store_once(path / "prepared.pb", request.SerializeAsString());
  v1::PreparedDownload result;
  *result.mutable_identity() = request.identity();
  result.set_candidate_digest(sha256_bytes(request.SerializeAsString()));
  result.set_dataset_id(dataset_id(record));
  return result;
}
Store::VerifiedPublication
Store::verify_publication(const v1::DownloadPublication& decision) const {
  protocol::validate_message(decision);
  validate_id(decision.publication_id());
  const auto path = operation(decision.identity());
  const auto prepared = read_message<v1::DownloadPreparation>(path / "prepared.pb");
  if (prepared.identity().SerializeAsString() != decision.identity().SerializeAsString() ||
      sha256_bytes(prepared.SerializeAsString()) != decision.candidate_digest())
    throw Error(ErrorCode::conflict, "publication does not match prepared download");
  if (fs::exists(path / "publication.pb") &&
      read_bytes(path / "publication.pb") != decision.SerializeAsString())
    throw Error(ErrorCode::conflict, "data operation identity has different content");
  return VerifiedPublication(decision, prepared.record());
}
v1::PublishedDownload Store::publish(const VerifiedPublication& verified) {
  const auto& decision = verified.decision_;
  const auto path = operation(decision.identity());
  // The task service already committed its decision. Retain the exact decision
  // before making this version visible; a repeated confirmation uses these bytes.
  store_once(path / "publication.pb", decision.SerializeAsString());
  archive_.publish_verified(verified.record_);
  return published(decision.identity());
}
v1::PublishedDownload Store::published(const v1::DownloadIdentity& identity) const {
  const auto path = operation(identity);
  const auto decision = read_message<v1::DownloadPublication>(path / "publication.pb");
  const auto prepared = read_message<v1::DownloadPreparation>(path / "prepared.pb");
  if (decision.identity().SerializeAsString() != identity.SerializeAsString() ||
      prepared.identity().SerializeAsString() != identity.SerializeAsString() ||
      decision.candidate_digest() != sha256_bytes(prepared.SerializeAsString()))
    throw std::invalid_argument("published download evidence mismatch");
  v1::PublishedDownload result;
  *result.mutable_decision() = decision;
  *result.mutable_record() = archive_.get(dataset_id(prepared.record()));
  return result;
}
v1::HistoryUsage Store::history_usage(const std::string& dataset) const {
  v1::HistoryUsage usage;
  usage.set_dataset_id(dataset);
  const auto saved_datasets = archive_.named_datasets();
  for (const auto& saved : saved_datasets.items()) {
    bool market = false, settlement = false;
    for (const auto& selection : saved.selections()) {
      market |= std::ranges::find(selection.source_dataset_ids(), usage.dataset_id()) !=
                selection.source_dataset_ids().end();
      settlement |= std::ranges::find(selection.settlement_dataset_ids(), usage.dataset_id()) !=
                    selection.settlement_dataset_ids().end();
    }
    if (!market && !settlement)
      continue;
    auto* row = usage.add_references();
    row->set_kind(asterion::data::v1::HISTORY_SAVED_DATASET);
    row->set_id(saved.id());
    row->set_name(saved.name());
    if (market)
      row->add_roles(asterion::data::v1::HISTORY_MARKET);
    if (settlement)
      row->add_roles(asterion::data::v1::HISTORY_SETTLEMENT);
  }
  (void)asterion::protocol::decode_history_usage(usage);
  return usage;
}
} // namespace asterion::data
