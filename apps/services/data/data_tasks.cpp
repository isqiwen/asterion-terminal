#include "data_tasks.hpp"
#include "history_minutes.hpp"
#include "task_payload.hpp"
#include <asterion/foundation/error.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/protocol/data.hpp>
#include <asterion/protocol/material.hpp>
#include <array>

namespace asterion::data {
namespace wire = task::proto;

namespace {

// Kind, resource, manifest, definition, result, finish; calculation, publishes,
// execution carries input, references history.
constexpr std::array<tasks::KindSpec, 3> data_kinds{{
    {wire::MINUTE_DOWNLOAD,
        "minutes",
        "minutes.task",
        "minutes",
        "minutes",
        "minutes",
        false,
        true,
        false,
        true},
    {wire::TABLE_UPDATE,
        "table_update",
        "tables.task",
        "table_update",
        "table_update",
        "table_update",
        false,
        false,
        false,
        false},
    {wire::MATERIAL_ACQUISITION,
        "material_acquisition_task",
        "material.task",
        "material",
        "material",
        "material",
        false,
        false,
        false,
        false},
}};

bool hex_digest(const std::string& value)
{
    return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string::npos;
}

proto::DownloadIdentity download_identity(const std::string& instance, const wire::Task& task)
{
    proto::DownloadIdentity result;
    result.set_data_instance(instance);
    result.set_task_instance(instance);
    result.set_task_id(task.id());
    result.set_attempt(task.attempt());
    return result;
}

class DataTasks final : public tasks::TaskDomain
{
public:
    std::string_view name() const override
    {
        return "data";
    }

    std::span<const tasks::KindSpec> kinds() const override
    {
        return data_kinds;
    }

    void describe(wire::Task& task) const override
    {
        switch (task.kind()) {
            case wire::MINUTE_DOWNLOAD: {
                const auto range = history_files::minute_range(task.minutes());
                task.set_source_name(task.minutes().source() + " " + task.minutes().contract_id());
                task.set_instrument(range.instrument.key());
                task.set_total(
                    static_cast<unsigned>((range.end_ns - range.begin_ns) / 86400000000000LL + 1));
                task.set_data_source(task.minutes().source());
                task.set_minute_interval_minutes(task.minutes().interval_minutes());
                break;
            }
            case wire::TABLE_UPDATE: {
                const auto& update = task.table_update();
                protocol::validate_table_update(update);
                std::string tables;
                for (const auto& table : update.tables())
                    tables += (tables.empty() ? "" : " + ") + table;
                if (update.has_refresh()) {
                    *task.mutable_table_refresh() = update.refresh();
                    tables += " " + update.refresh().partition().exchange() + " "
                        + update.refresh().partition().period();
                }
                task.set_source_name(update.provider());
                task.set_instrument(tables);
                task.set_trading_day(
                    update.begin_day().empty() && update.end_day().empty()
                        ? std::string()
                        : update.begin_day() + " / " + update.end_day());
                // What an update reads is decided when it runs: progress is a percentage.
                task.set_total(100);
                task.set_data_source(update.provider());
                break;
            }
            case wire::MATERIAL_ACQUISITION:
                protocol::validate_material_input(task.material());
                if (task.material().request().id() != task.id())
                    throw std::invalid_argument("material acquisition task identity mismatch");
                task.set_provider_artifact(task.material().request().provider_artifact());
                task.set_source_name(task.material().request().source_id());
                task.set_instrument(task.material().request().resource_id());
                task.set_total(1);
                task.set_data_source(task.material().request().source_id());
                break;
            default:
                throw std::logic_error("data task kind has no description");
        }
    }

    void write_manifest(const wire::Task& task, Json& manifest) const override
    {
        manifest["provider_artifact"] = task.provider_artifact();
        manifest["download_authorization"] = task.download_authorization();
    }

    void read_manifest(const Json& manifest, wire::Task& task, const std::string&) const override
    {
        require_fields(manifest, {"provider_artifact", "download_authorization"});
        task.set_provider_artifact(manifest.at("provider_artifact").get<std::string>());
        task.set_download_authorization(manifest.at("download_authorization").get<std::string>());
        if (task.kind() == wire::MATERIAL_ACQUISITION) {
            tasks::payload::validate_digest(task.provider_artifact());
            if (!task.download_authorization().empty())
                throw std::invalid_argument(
                    "material acquisition has unexpected download authorization");
        } else {
            if (!hex_digest(task.download_authorization()))
                throw std::invalid_argument("invalid task download authorization");
            if (!hex_digest(task.provider_artifact()))
                throw std::invalid_argument("invalid task provider artifact");
        }
    }

    void validate_result(
        const wire::Task& task,
        const wire::TaskFinish& result,
        const std::string& instance) const override
    {
        switch (task.kind()) {
            case wire::MINUTE_DOWNLOAD:
                protocol::validate_message(result.minutes());
                break;
            case wire::TABLE_UPDATE:
                protocol::validate_table_update_result(
                    task.table_update(),
                    download_identity(instance, task),
                    result.table_update());
                break;
            case wire::MATERIAL_ACQUISITION:
                protocol::validate_material_result(task.material(), result.material());
                break;
            default:
                throw std::logic_error("data task kind has no result");
        }
    }

    // A download references the Data version it published.
    void history_references(
        const wire::Task& task,
        const std::string& dataset_id,
        proto::HistoryUsage& usage) const override
    {
        if (task.history_dataset_id() != dataset_id || !task.has_minutes())
            return;
        auto* row = usage.add_references();
        row->set_id(task.id());
        row->set_kind(proto::HISTORY_DOWNLOAD);
        row->add_roles(proto::HISTORY_OUTPUT);
    }
};

bool occupies_slot(wire::TaskState state)
{
    return state == wire::QUEUED || state == wire::RUNNING || state == wire::CANCEL_REQUESTED
        || state == wire::PUBLISHING;
}

} // namespace

const tasks::TaskDomain& task_domain()
{
    static const DataTasks domain;
    return domain;
}

std::vector<std::string>
table_snapshot_holds(const tasks::Store& store, const std::string& snapshot)
{
    tasks::payload::validate_digest(snapshot);
    if (store.has_submission() || store.list(1).capacity().uncommitted())
        throw Error(
            ErrorCode::unavailable,
            "uncommitted acquisition input requires recovery before snapshot maintenance");
    std::vector<std::string> result;
    unsigned count = 0;
    store.visit({}, [&](const wire::StoredTaskRecord& record) {
        if (++count > 100000)
            throw Error(
                ErrorCode::resource_exhausted,
                "snapshot acquisition scan exceeds capacity");
        const auto& task = record.task();
        if (task.kind() != wire::TABLE_UPDATE || task.has_retirement())
            return true;
        const auto update = store.get(task.id()).table_update();
        bool held = update.has_refresh() && update.refresh().previous_snapshot() == snapshot;
        // Before an unrestricted update's final receipt exists, its published
        // prefix set is incomplete. Never report that interval as unheld.
        held |= occupies_slot(task.state()) && !update.has_refresh();
        if (task.state() == wire::SUCCEEDED) {
            auto read = store.prepare_result(task.id());
            read.verify();
            const auto output = store.confirm_result(std::move(read)).table_update();
            held |=
                output.snapshot() == snapshot || output.refreshed_revision().snapshot() == snapshot;
        }
        if (held)
            result.push_back(task.id());
        if (result.size() > 10000)
            throw Error(
                ErrorCode::resource_exhausted,
                "snapshot acquisition holds exceed capacity");
        return true;
    });
    return result;
}

proto::DownloadPreparation
download_preparation(const tasks::Store::Completion& completion, const std::string& instance)
{
    const auto& task = completion.task();
    if (!completion.result().has_minutes() || !task.has_minutes())
        throw std::invalid_argument("download result has not been verified");
    proto::DownloadPreparation preparation;
    *preparation.mutable_identity() = download_identity(instance, task);
    auto* record = preparation.mutable_record();
    *record->mutable_minutes() = task.minutes();
    *record->mutable_minute_result() = completion.result().minutes();
    return preparation;
}

tasks::Store::Change prepare_publication(
    tasks::Store& store,
    tasks::Store::Completion completion,
    const proto::PreparedDownload& prepared)
{
    protocol::validate_message(prepared);
    const auto preparation = download_preparation(completion, store.instance());
    const auto& dataset = completion.result().minutes().manifest_sha256();
    if (preparation.identity().SerializeAsString() != prepared.identity().SerializeAsString()
        || sha256_bytes(preparation.SerializeAsString()) != prepared.candidate_digest()
        || dataset != prepared.dataset_id())
        throw std::invalid_argument("prepared download does not match task completion");
    return store.prepare_publication(
        std::move(completion),
        prepared.identity(),
        prepared.candidate_digest(),
        dataset);
}

tasks::Store::Change
confirm_publication(tasks::Store& store, const proto::PublishedDownload& published)
{
    protocol::validate_message(published);
    proto::DownloadPreparation preparation;
    *preparation.mutable_identity() = published.decision().identity();
    *preparation.mutable_record() = published.record();
    if (sha256_bytes(preparation.SerializeAsString()) != published.decision().candidate_digest())
        throw std::invalid_argument("published download evidence changed");
    return store.confirm_publication(published.decision());
}

} // namespace asterion::data
