#include "data_store.hpp"
#include "margins.hpp"
#include "history_providers.hpp"
#include "history_minutes.hpp"
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/protocol/data.hpp>
#include <fstream>
#include <chrono>

namespace asterion::data {
namespace fs = std::filesystem;

namespace {

fs::path checked_root(fs::path root)
{
    if (!root.is_absolute() || !fs::is_directory(root) || fs::is_symlink(root))
        throw std::invalid_argument("data service requires an existing absolute directory");
    return root;
}

std::string read_bytes(const fs::path& path)
{
    if (fs::is_symlink(path) || !fs::is_regular_file(path) || fs::file_size(path) > 262144)
        throw std::invalid_argument("data service record is unavailable");
    std::ifstream stream(path, std::ios::binary);
    std::string bytes{std::istreambuf_iterator<char>(stream), {}};
    if (stream.bad())
        throw std::runtime_error("data service record read failed");
    return bytes;
}

template <class Message>
Message read_message(const fs::path& path)
{
    Message message;
    if (!message.ParseFromString(read_bytes(path)))
        throw std::invalid_argument("invalid data service record");
    protocol::validate_message(message);
    return message;
}

std::string dataset_id(const proto::HistoryRecord& record)
{
    return record.minute_result().manifest_sha256();
}

void store_once(const fs::path& path, const std::string& bytes)
{
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

Store::Store(fs::path root, std::string instance)
    : root_(checked_root(std::move(root))),
      instance_(std::move(instance)),
      ownership_(root_, "data.lock"),
      archive_([&] {
          validate_id(instance_);
          const Json identity{{"data_instance", instance_}};
          const auto path = root_ / "instance.json";
          if (fs::exists(path)) {
              if (parse_json(read_bytes(path)) != identity)
                  throw std::invalid_argument("data service instance identity mismatch");
          } else {
              for (const auto& file : fs::directory_iterator(root_))
                  if (file.path().filename() != "data.lock")
                      throw std::invalid_argument(
                          "data directory has no supported instance identity");
              replace_file_durably(path, identity.dump());
          }
          create_directories_durably(root_ / "downloads");
          create_directories_durably(root_ / "authorizations");
          return root_ / "history";
      }()),
      tables_(root_ / "tables")
{}

proto::DownloadBudgetPolicy Store::verify_budget(
    const proto::DownloadBudgetConfiguration& request) const
{
    protocol::validate_message(request);
    for (const auto& source : history_providers::sources()) {
        if (source.id != request.source())
            continue;
        if (!request.requests_per_minute()
            || request.requests_per_minute() > source.max_requests_per_minute
            || (source.credential_required && request.credential().empty()))
            throw std::invalid_argument("invalid download request budget");
        proto::DownloadBudgetPolicy result;
        result.set_provider_id(source.plugin_id);
        result.set_requests_per_minute(request.requests_per_minute());
        return result;
    }
    if (request.source() == shfe::source && request.credential().empty()
        && request.requests_per_minute() > 0 && request.requests_per_minute() <= 60) {
        (void)history_providers::table_artifact(shfe::provider);
        proto::DownloadBudgetPolicy result;
        result.set_provider_id(shfe::provider);
        result.set_requests_per_minute(request.requests_per_minute());
        return result;
    }
    throw std::invalid_argument("historical data source is unavailable");
}

proto::StoredDownloadAuthorization Store::download_authorization(
    const proto::DownloadIdentity& identity) const
{
    const auto allocated =
        read_message<proto::AllocatedDownload>(operation(identity) / "allocation.pb");
    if (allocated.request().identity().SerializeAsString() != identity.SerializeAsString())
        throw std::invalid_argument("download allocation identity mismatch");
    const auto stored = read_authorization(allocated.request().authorization_id());
    if (stored.authorization().task_id() != identity.task_id())
        throw Error(ErrorCode::conflict, "download allocation does not match authorization");
    return stored;
}

fs::path Store::authorization_path(const std::string& id) const
{
    if (id.size() != 64 || id.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::invalid_argument("invalid download authorization reference");
    return root_ / "authorizations" / (id + ".pb");
}

Store::VerifiedAuthorization Store::verify_authorization(
    const proto::DownloadAuthorizationRequest& request,
    std::stop_token stop) const
{
    protocol::validate_message(request);
    validate_id(request.task_id());
    const auto& owner = instance_;
    if (request.task_instance() != owner)
        throw Error(ErrorCode::conflict, "download authorization does not match task binding");
    const auto id = sha256_bytes(instance_ + "/" + owner + "/" + request.task_id());
    if (fs::exists(authorization_path(id))) {
        auto existing = read_authorization(id);
        const auto& fixed = existing.authorization();
        const bool same_definition = request.has_minutes()
            ? fixed.has_minutes()
                && request.minutes().SerializeAsString() == fixed.minutes().SerializeAsString()
            : request.has_table_update() && fixed.has_table_update()
                && request.table_update().SerializeAsString()
                    == fixed.table_update().SerializeAsString();
        if (!same_definition || request.credential() != existing.credential())
            throw Error(ErrorCode::conflict, "data operation identity has different content");
        return VerifiedAuthorization(std::move(existing));
    }
    proto::StoredDownloadAuthorization stored;
    auto* authorization = stored.mutable_authorization();
    authorization->set_data_instance(instance_);
    authorization->set_task_instance(owner);
    authorization->set_task_id(request.task_id());
    authorization->set_id(id);
    if (request.has_minutes()) {
        const auto& input = request.minutes();
        const auto range = history_files::minute_range(input);
        history_providers::validate_request(
            input.source(),
            range.instrument,
            input.interval_minutes(),
            input.requests_per_minute());
        auto provider = history_providers::minutes(input.source(), request.credential());
        authorization->set_provider_artifact(history_providers::artifact(input.source()));
        *authorization->mutable_minutes() = input;
    } else if (request.has_table_update()) {
        const auto& input = request.table_update();
        protocol::validate_table_update(input);
        const auto offered = history_providers::table_sources();
        for (const auto& table : input.tables())
            if (std::ranges::none_of(offered, [&](const auto& item) {
                    return item.plugin_id == input.provider() && item.table == table;
                }))
                throw std::invalid_argument("history table is unavailable from this source");
        if (input.has_refresh()) {
            proto::TableRefreshQuery query;
            query.set_provider(input.provider());
            query.set_source(input.refresh().source());
            *query.mutable_partition() = input.refresh().partition();
            query.set_previous_snapshot(input.refresh().previous_snapshot());
            query.set_requests_per_minute(input.requests_per_minute());
            const auto expected = table_refresh_plan(
                tables_,
                table_offers(),
                query,
                history_providers::table_artifact(input.provider()),
                format_shanghai_time(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::system_clock::now().time_since_epoch())
                        .count())
                    .substr(0, 10));
            if (expected.update().SerializeAsString() != input.SerializeAsString())
                throw Error(ErrorCode::conflict, "table refresh plan changed; preview again");
        }
        // Starting the provider checks the credential's shape, as for the other downloads.
        auto provider = history_providers::tables(input.provider(), request.credential());
        authorization->set_provider_artifact(history_providers::table_artifact(input.provider()));
        authorization->set_provider_id(input.provider());
        *authorization->mutable_table_update() = input;
    } else
        throw std::invalid_argument("download definition is missing");
    const auto source = request.has_minutes() ? request.minutes().source() : std::string();
    for (const auto& item : history_providers::sources())
        if (item.id == source) {
            authorization->set_provider_id(item.plugin_id);
            break;
        }
    history_providers::preserve_artifact(
        authorization->provider_artifact(),
        root_ / "programs",
        stop);
    stored.set_credential(request.credential());
    return VerifiedAuthorization(std::move(stored));
}

proto::DownloadAuthorization Store::authorize(const VerifiedAuthorization& verified)
{
    const auto& stored = verified.value_;
    const auto& authorization = stored.authorization();
    if (authorization.data_instance() != instance_ || authorization.task_instance() != instance_)
        throw Error(ErrorCode::conflict, "download authorization does not match task binding");
    const auto path = authorization_path(authorization.id());
    // One private atomic record is the commit point. A repeated capture must use
    // the same definition, plugin and secret, never the current default token.
    store_once(path, stored.SerializeAsString());
    return authorization;
}

proto::StoredDownloadAuthorization Store::read_authorization(const std::string& id) const
{
    const auto stored = read_message<proto::StoredDownloadAuthorization>(authorization_path(id));
    const auto& value = stored.authorization();
    if (value.id() != id || value.data_instance() != instance_ || value.task_instance() != instance_
        || sha256_bytes(instance_ + "/" + value.task_instance() + "/" + value.task_id()) != id
        || value.provider_artifact().size() != 64
        || value.provider_artifact().find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::invalid_argument("invalid stored download authorization");
    validate_id(value.task_id());
    validate_id(value.provider_id());
    // Metadata reads do not load the provider or depend on its current availability.
    if (value.has_minutes())
        (void)history_files::minute_range(value.minutes());
    else if (value.has_table_update())
        protocol::validate_table_update(value.table_update());
    else
        throw std::invalid_argument("invalid stored download authorization");
    return stored;
}

proto::DownloadAuthorization Store::authorization(const std::string& id) const
{
    const auto stored = read_authorization(id);
    // Complete any capture whose reply was lost after rename but before directory sync.
    sync_directory(root_ / "authorizations");
    return stored.authorization();
}

fs::path Store::operation(const proto::DownloadIdentity& identity) const
{
    protocol::validate_message(identity);
    validate_id(identity.task_id());
    if (identity.data_instance() != instance_ || identity.task_instance() != instance_
        || !identity.attempt())
        throw Error(ErrorCode::conflict, "download identity does not match data binding");
    return root_ / "downloads" / sha256_bytes(identity.SerializeAsString());
}

BarDatasetSources Store::sources(const proto::BarDatasetRequest& request) const
{
    protocol::decode_bar_dataset_request(request);
    BarDatasetSources result;
    result.request = request;
    for (const auto& id : request.source_dataset_ids())
        result.sources.push_back(archive_.get(id));
    return result;
}

Store::VerifiedDataset Store::verify_dataset(const proto::NamedDataset& value) const
{
    protocol::validate_named_dataset(value);
    std::size_t bars = 0;
    std::vector<std::string> days;
    for (int i = 0; i < value.selections_size(); ++i) {
        const auto resolved = resolve_bar_dataset(tables_, sources(value.selections(i)));
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

void Store::save(const VerifiedDataset& verified)
{
    for (const auto& selection : verified.value_.selections()) {
        if (!tables_.sequence(selection.daily().snapshot()))
            throw Error(
                ErrorCode::conflict,
                "named dataset table snapshot retired during preparation");
        for (const auto& id : selection.source_dataset_ids())
            (void)archive_.get(id);
    }
    archive_.save_named_dataset(verified.value_);
}

Store::VerifiedAllocation Store::verify_allocation(const proto::DownloadAllocation& request) const
{
    protocol::validate_message(request);
    const auto path = operation(request.identity());
    const auto authorized = authorization(request.authorization_id());
    if (authorized.task_instance() != request.identity().task_instance()
        || authorized.task_id() != request.identity().task_id()
        || (authorized.has_minutes() ? !request.has_minutes()
                    || authorized.minutes().SerializeAsString()
                        != request.minutes().SerializeAsString()
                                     : !request.has_table_update()
                    || authorized.table_update().SerializeAsString()
                        != request.table_update().SerializeAsString()))
        throw Error(ErrorCode::conflict, "download allocation does not match authorization");
    std::optional<std::string> directory;
    if (fs::exists(path / "allocation.pb")) {
        const auto saved = read_message<proto::AllocatedDownload>(path / "allocation.pb");
        if (saved.request().SerializeAsString() != request.SerializeAsString())
            throw Error(ErrorCode::conflict, "data operation identity has different content");
        if (request.has_table_update() ? !saved.directory().empty()
                                       : !fs::path(saved.directory()).is_absolute())
            throw std::invalid_argument("invalid allocated download directory");
        directory = saved.directory();
    }
    if (request.has_table_update())
        return VerifiedAllocation(request, std::nullopt, {}, 0, std::move(directory));
    if (request.has_minutes()) {
        const auto range = history_files::minute_range(request.minutes());
        return VerifiedAllocation(
            request,
            range.instrument,
            range.source,
            range.interval_minutes,
            std::move(directory));
    }
    throw std::invalid_argument("download definition is missing");
}

proto::DownloadDirectory Store::allocate(const VerifiedAllocation& verified)
{
    const auto& request = verified.value_;
    const auto path = operation(request.identity());
    proto::DownloadDirectory result;
    if (!verified.contract_) {
        // The receipt alone binds the attempt to its authorization.
        create_directories_durably(path);
        proto::AllocatedDownload allocation;
        *allocation.mutable_request() = request;
        store_once(path / "allocation.pb", allocation.SerializeAsString());
        return result;
    }
    const auto directory = archive_.directory(
        *verified.contract_,
        verified.source_,
        verified.interval_,
        path.filename().string());
    if (verified.directory_) {
        // The receipt owns the confirmed path. OS aliases do not allocate a new version.
        if (!fs::equivalent(
                archive_.resolve_directory(request.minutes(), *verified.directory_),
                directory))
            throw Error(
                ErrorCode::conflict,
                "allocated download directory does not belong to this operation");
        sync_directory(path);
        result.set_directory(*verified.directory_);
        return result;
    }
    create_directories_durably(path);
    proto::AllocatedDownload allocation;
    *allocation.mutable_request() = request;
    allocation.set_directory(directory.string());
    store_once(path / "allocation.pb", allocation.SerializeAsString());
    result.set_directory(directory.string());
    return result;
}

proto::DownloadCredentials Store::credentials(const proto::DownloadIdentity& identity) const
{
    const auto stored = download_authorization(identity);
    const auto& authorized = stored.authorization();
    proto::DownloadCredentials result;
    *result.mutable_identity() = identity;
    result.set_authorization_id(authorized.id());
    result.set_credential(stored.credential());
    return result;
}

Store::VerifiedDownload Store::verify_download(const proto::DownloadPreparation& request) const
{
    protocol::validate_message(request);
    const auto path = operation(request.identity());
    const auto allocated = read_message<proto::AllocatedDownload>(path / "allocation.pb");
    const auto& allocation = allocated.request();
    if (allocation.identity().SerializeAsString() != request.identity().SerializeAsString())
        throw std::invalid_argument("download allocation identity mismatch");
    const auto& owned = allocated.directory();
    const auto& record = request.record();
    if (allocation.has_minutes() && record.has_minutes() && record.has_minute_result()) {
        if (record.minutes().SerializeAsString() != allocation.minutes().SerializeAsString()
            || record.minute_result().directory() != owned)
            throw std::invalid_argument("download candidate does not match its allocation");
        history_files::verify_minute_result(record.minutes(), record.minute_result());
    } else
        throw std::invalid_argument("download candidate does not match its allocation");
    return VerifiedDownload(request);
}

proto::PreparedDownload Store::prepare(const VerifiedDownload& verified)
{
    const auto& request = verified.value_;
    const auto path = operation(request.identity());
    const auto& record = request.record();
    // Files are already under the warehouse. Persisting this record accepts
    // custody without exposing the version in the public archive index.
    store_once(path / "prepared.pb", request.SerializeAsString());
    proto::PreparedDownload result;
    *result.mutable_identity() = request.identity();
    result.set_candidate_digest(sha256_bytes(request.SerializeAsString()));
    result.set_dataset_id(dataset_id(record));
    return result;
}

Store::VerifiedPublication Store::verify_publication(
    const proto::DownloadPublication& decision) const
{
    protocol::validate_message(decision);
    validate_id(decision.publication_id());
    const auto path = operation(decision.identity());
    const auto prepared = read_message<proto::DownloadPreparation>(path / "prepared.pb");
    if (prepared.identity().SerializeAsString() != decision.identity().SerializeAsString()
        || sha256_bytes(prepared.SerializeAsString()) != decision.candidate_digest())
        throw Error(ErrorCode::conflict, "publication does not match prepared download");
    if (fs::exists(path / "publication.pb")
        && read_bytes(path / "publication.pb") != decision.SerializeAsString())
        throw Error(ErrorCode::conflict, "data operation identity has different content");
    return VerifiedPublication(decision, prepared.record());
}

proto::PublishedDownload Store::publish(const VerifiedPublication& verified)
{
    const auto& decision = verified.decision_;
    const auto path = operation(decision.identity());
    // The task service already committed its decision. Retain the exact decision
    // before making this version visible; a repeated confirmation uses these bytes.
    store_once(path / "publication.pb", decision.SerializeAsString());
    archive_.publish_verified(verified.record_);
    return published(decision.identity());
}

proto::PublishedDownload Store::published(const proto::DownloadIdentity& identity) const
{
    const auto path = operation(identity);
    const auto decision = read_message<proto::DownloadPublication>(path / "publication.pb");
    const auto prepared = read_message<proto::DownloadPreparation>(path / "prepared.pb");
    if (decision.identity().SerializeAsString() != identity.SerializeAsString()
        || prepared.identity().SerializeAsString() != identity.SerializeAsString()
        || decision.candidate_digest() != sha256_bytes(prepared.SerializeAsString()))
        throw std::invalid_argument("published download evidence mismatch");
    proto::PublishedDownload result;
    *result.mutable_decision() = decision;
    *result.mutable_record() = archive_.get(dataset_id(prepared.record()));
    return result;
}

void Store::begin_table_retirement(const history_files::TableStore::Retirement& decision)
{
    tables_.begin_retirement(decision);
}

void Store::clean_table_retirement(const std::string& id, std::stop_token stop)
{
    tables_.clean_retirement(id, stop);
}

void Store::finish_table_retirement(const std::string& id)
{
    tables_.finish_retirement(id);
}

proto::HistoryUsage Store::snapshot_usage(const std::string& snapshot) const
{
    proto::HistoryUsage result;
    result.set_dataset_id(snapshot);
    const auto named = archive_.named_datasets();
    for (const auto& saved : named.items()) {
        if (std::ranges::none_of(saved.selections(), [&](const auto& selection) {
                return selection.daily().snapshot() == snapshot;
            }))
            continue;
        auto* item = result.add_references();
        item->set_kind(proto::HISTORY_SAVED_DATASET);
        item->set_id(saved.id());
        item->set_name(saved.name());
        item->add_roles(proto::HISTORY_MARKET);
    }
    (void)protocol::decode_history_usage(result);
    return result;
}

proto::HistoryUsage Store::history_usage(const std::string& dataset) const
{
    proto::HistoryUsage usage;
    usage.set_dataset_id(dataset);
    const auto saved_datasets = archive_.named_datasets();
    for (const auto& saved : saved_datasets.items()) {
        bool market = false;
        for (const auto& selection : saved.selections())
            market |= std::ranges::find(selection.source_dataset_ids(), usage.dataset_id())
                != selection.source_dataset_ids().end();
        if (!market)
            continue;
        auto* row = usage.add_references();
        row->set_kind(asterion::data::proto::HISTORY_SAVED_DATASET);
        row->set_id(saved.id());
        row->set_name(saved.name());
        row->add_roles(asterion::data::proto::HISTORY_MARKET);
    }
    (void)asterion::protocol::decode_history_usage(usage);
    return usage;
}

std::vector<TableOffer> Store::table_offers() const
{
    std::vector<TableOffer> result;
    for (auto& item : history_providers::table_sources())
        result.push_back(
            {{std::move(item.table), item.schema, std::move(item.source)},
                std::move(item.plugin_id),
                std::move(item.venues)});
    return result;
}

std::pair<proto::TableUpdate, std::vector<TableOffer>> Store::table_attempt(
    const proto::DownloadIdentity& identity) const
{
    const auto stored = download_authorization(identity);
    const auto& authorization = stored.authorization();
    if (!authorization.has_table_update())
        throw Error(ErrorCode::conflict, "download attempt is not a table update");
    const auto& provider = authorization.table_update().provider();
    // One library means one set of sources and mapping rules. An attempt pinned
    // to a library this service no longer runs would record under another's name.
    if (history_providers::table_artifact(provider) != authorization.provider_artifact())
        throw Error(
            ErrorCode::conflict,
            "the data source changed after this update was submitted; submit it again");
    auto offers = table_offers();
    std::erase_if(offers, [&](const TableOffer& item) {
        return item.plugin_id != provider;
    });
    return {authorization.table_update(), std::move(offers)};
}

proto::TableUpdatePlan
Store::table_plan(const proto::TablePlanQuery& query, const std::string& today) const
{
    protocol::validate_message(query);
    const auto [update, offers] = table_attempt(query.identity());
    return table_update_plan(tables_, offers, update, query.step(), today);
}

std::variant<proto::TablePartitionReceipt, Store::VerifiedPartition> Store::verify_partition(
    const proto::TablePartitionRows& request) const
{
    protocol::validate_message(request);
    const auto [update, offers] = table_attempt(request.identity());
    const auto& key = request.partition();
    if (!table_update_covers(offers, update, key))
        throw std::invalid_argument("partition is not part of this table update");
    if (request.rows().empty())
        throw std::invalid_argument("a table partition needs rows");
    const auto& table = history_table(key.table());
    std::vector<TableSourceRow> rows;
    rows.reserve(static_cast<std::size_t>(request.rows_size()));
    for (const auto& source : request.rows()) {
        auto& row = rows.emplace_back();
        row.reserve(static_cast<std::size_t>(source.cells_size()));
        for (const auto& cell : source.cells())
            row.push_back(cell.has_text() ? std::optional(cell.text()) : std::nullopt);
    }
    proto::TablePartitionReceipt receipt;
    VerifiedPartition verified;
    if (request.has_response()) {
        const auto& captured = request.response();
        verified.response_ =
            SourceResponse{captured.format(), captured.body(), captured.received_at_ns()};
        history_providers::validate_response(
            key.table(),
            key.exchange(),
            key.period(),
            *verified.response_);
    }
    try {
        verified.partition_ = table_partition(table, key.exchange(), key.period(), rows);
    } catch (const std::invalid_argument& error) {
        // An answer about the rows, not a failed request.
        receipt.set_rejection(error.what());
        return receipt;
    }
    verified.series_ =
        std::ranges::find(offers, key.table(), [](const TableOffer& item) -> const std::string& {
            return item.series.table;
        })->series;
    verified.provider_artifact_ = history_providers::table_artifact(update.provider());
    if (update.has_refresh() && verified.response_ && key.table() == "futures.daily"
        && key.exchange() == "SHFE") {
        const auto& identity = request.identity();
        verified.observation_context_ = history_files::TableObservationContext{
            identity.data_instance(),
            identity.task_instance(),
            identity.task_id(),
            identity.attempt(),
            update.provider(),
            verified.provider_artifact_,
            update.refresh().previous_snapshot(),
            update.refresh().previous_content()
        };
        verified.previous_observation_ = tables_.refresh_observation(
            *verified.observation_context_,
            verified.series_,
            key.exchange(),
            key.period());
        if (verified.previous_observation_)
            return verified; // replay is checked again on the serialized writer
    }
    if (const auto held = tables_.current(verified.series_, key.exchange(), key.period());
        held && held->content == verified.partition_.content) {
        if (verified.observation_context_) {
            verified.expected_snapshot_ = update.refresh().previous_snapshot();
            return verified;
        }
        *receipt.mutable_revision() = encode_table_revision(*held);
        receipt.set_unchanged(true);
        return receipt;
    }
    verified.missing_only_day_ = !update.has_refresh() && table.period == TablePeriod::day;
    if (verified.missing_only_day_
        && tables_.current(verified.series_, key.exchange(), key.period())) {
        receipt.set_rejection("day partition is already stored; use explicit refresh");
        return receipt;
    }
    if (update.has_refresh()) {
        const auto held = tables_.current(verified.series_, key.exchange(), key.period());
        if (!held || held->snapshot != update.refresh().previous_snapshot()) {
            receipt.set_rejection("selected table partition changed; read it and preview again");
            return receipt;
        }
        verified.expected_snapshot_ = update.refresh().previous_snapshot();
    }
    verified.object_ = tables_.write(verified.partition_);
    verified.provider_artifact_ = history_providers::table_artifact(update.provider());
    return verified;
}

proto::TablePartitionReceipt
Store::record_partition(const VerifiedPartition& verified, std::int64_t now_ns)
{
    const auto observed = [&](const history_files::TableRevision& revision, bool unchanged) {
        proto::TablePartitionReceipt receipt;
        *receipt.mutable_revision() = encode_table_revision(revision);
        receipt.set_unchanged(unchanged);
        if (verified.observation_context_) {
            const auto observation = tables_.record_refresh_observation(
                *verified.observation_context_,
                revision,
                *verified.response_,
                now_ns,
                unchanged);
            *receipt.mutable_observation() = encode_refresh_observation(observation);
            receipt.set_unchanged(observation.unchanged);
        }
        return receipt;
    };
    if (verified.previous_observation_) {
        if (verified.previous_observation_->revision.content != verified.partition_.content)
            throw std::invalid_argument(
                "table refresh observation identity conflicts with recorded response");
        return observed(
            verified.previous_observation_->revision,
            verified.previous_observation_->unchanged);
    }
    if (!verified.expected_snapshot_.empty() || verified.missing_only_day_) {
        const auto held = tables_.current(
            verified.series_,
            verified.partition_.exchange,
            verified.partition_.period);
        if (held && held->content == verified.partition_.content) {
            return observed(*held, true);
        }
        if (verified.missing_only_day_ && held) {
            proto::TablePartitionReceipt receipt;
            receipt.set_rejection("day partition is already stored; use explicit refresh");
            return receipt;
        }
        if (!verified.missing_only_day_
            && (!held || held->snapshot != verified.expected_snapshot_)) {
            proto::TablePartitionReceipt receipt;
            receipt.set_rejection("selected table partition changed; read it and preview again");
            return receipt;
        }
    }
    const auto recorded = tables_.record(
        verified.partition_,
        verified.series_,
        verified.object_,
        verified.provider_artifact_,
        now_ns,
        verified.response_);
    return observed(recorded.revision, recorded.unchanged);
}

} // namespace asterion::data

namespace asterion::data {

void Store::begin_minute_retirement(const proto::HistoryRecord& record, const std::string& manifest)
{
    archive_.begin_retirement(record, manifest);
}

void Store::clean_minute_retirement(const std::string& id, std::stop_token stop)
{
    archive_.clean_retirement(id, stop);
}

void Store::finish_minute_retirement(const std::string& id)
{
    archive_.finish_retirement(id);
}

} // namespace asterion::data
