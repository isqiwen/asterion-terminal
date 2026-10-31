#include "data_backup.hpp"
#include "research_submission.hpp"
#include "data_submission.hpp"
#include "domain_backup.hpp"
#include "data_store.hpp"
#include "data_retentions.hpp"
#include "task_store.hpp"
#include "history_providers.hpp"
#include <asterion/system/native_plugin.hpp>
#include "data/history_fixture.hpp"
#include <asterion/system/durable_file.hpp>
#include <asterion/system/process/child.hpp>
#include <asterion/system/process/artifact.hpp>
#include <asterion/base/error.hpp>
#include <gtest/gtest.h>
#include <algorithm>

using namespace asterion;
namespace fs = std::filesystem;

namespace {

struct Folder
{
    fs::path path = fs::temp_directory_path() / ("asterion-domain-backup-" + unique_process_id());

    Folder()
    {
        fs::create_directory(path);
        path = fs::canonical(path);
    }

    ~Folder()
    {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

data::proto::DataOwner owner()
{
    data::proto::DataOwner value;
    value.set_node_id("fixture-node");
    value.set_service_id("historical-data");
    return value;
}

void seal(const fs::path& root)
{
    data::Store store(root, "historical-data");
    data::Retentions references(root, owner(), store);
}

} // namespace

TEST(DataColdBackup, ExcludesSecretsAndValidatesIndependentOwnerAndPayloadWithoutOriginalLocation)
{
    Folder directory;
    // Literal path bytes must never become SQLite URI options in cold reads.
    const auto root = directory.path / "source ?#%";
    fs::create_directory(root);
    (void)test::seed_history(root, {100, 101, 102}, "cold-data");
    seal(root);
    replace_file_durably(
        root / "authorizations" / "synthetic.private",
        "SYNTHETIC_SECRET_NOT_FOR_BACKUP");
    auto exported = data::cold_backup(root, owner());
    EXPECT_THROW(data::Store busy(root, "historical-data"), std::runtime_error);
    for (const auto& file : exported.snapshot.files)
        EXPECT_FALSE(file.relative.generic_string().starts_with("authorizations/"));
    std::vector<backup_files::Snapshot> sources;
    sources.push_back(std::move(exported.snapshot));
    backup_files::Cut cut(std::move(sources));
    const auto plan = cut.preview("cold-data-one");
    const auto saved = directory.path / "backup";
    (void)cut.save("cold-data-one", saved, sha256_bytes(plan.dump()));
    const auto restored = directory.path / "restored";
    (void)backup_files::restore_files(saved, restored);
    fs::rename(root, directory.path / "original-preserved");
    const auto copied = restored / "owners" / "0";
    const auto before = sha256_file(copied / "tables" / "ledger.sqlite");
    auto verified = data::cold_backup(copied, owner());
    EXPECT_FALSE(verified.snapshot.files.empty());
    EXPECT_TRUE(verified.references.empty());
    EXPECT_EQ(sha256_file(copied / "tables" / "ledger.sqlite"), before);
    EXPECT_FALSE(fs::exists(copied / "authorizations"));
    EXPECT_EQ(
        sha256_file(directory.path / "original-preserved" / "authorizations" / "synthetic.private"),
        sha256_bytes("SYNTHETIC_SECRET_NOT_FOR_BACKUP"));
    auto changed = owner();
    changed.set_node_id("another-node");
    EXPECT_THROW(data::cold_backup(copied, changed), std::invalid_argument);
    EXPECT_EQ(backup_files::inspect(saved), plan);
}

TEST(DataColdBackup, KeepsReferenceAuthorityAndRejectsUnknownFilesOrUnsealedIdentity)
{
    Folder directory;
    const auto selected = test::seed_history(directory.path, {100, 101, 102}, "cold-held");
    EXPECT_THROW(data::cold_backup(directory.path, owner()), Error);
    data::proto::DataRetention request;
    *request.mutable_source() = owner();
    auto* consumer = request.mutable_consumer();
    consumer->mutable_owner()->set_node_id("fixture-node");
    consumer->mutable_owner()->set_service_id("research-one");
    consumer->mutable_owner()->set_domain("research");
    consumer->set_kind("backtest");
    consumer->set_id("retained-run");
    consumer->set_version(std::string(64, 'a'));
    auto* version = request.add_versions();
    version->set_kind(data::proto::DataVersion::MINUTE_VERSION);
    version->set_version(selected.at("source_dataset_ids")[0].get<std::string>());
    data::proto::DataRetention fixed;
    {
        data::Store store(directory.path, "historical-data");
        data::Retentions references(directory.path, owner(), store);
        fixed = references.prepare(request).retention();
    }
    auto exported = data::cold_backup(directory.path, owner());
    ASSERT_EQ(exported.references.size(), 1U);
    EXPECT_EQ(
        exported.references[0].consumer.SerializeAsString(),
        request.consumer().SerializeAsString());
    EXPECT_EQ(exported.references[0].retention_digest, sha256_bytes(fixed.SerializeAsString()));
    ASSERT_EQ(exported.snapshot.dependencies.size(), 1U);
    EXPECT_FALSE(exported.snapshot.dependencies[0].included);
    replace_file_durably(directory.path / "unclassified.private", "synthetic unrelated file");
    EXPECT_THROW(data::cold_backup(directory.path, owner()), std::invalid_argument);
}

TEST(DataColdBackup, DeclaresQueuedAcquisitionAndItsMissingProgramWithoutRunningIt)
{
    Folder directory;
    const auto selected = test::seed_history(directory.path, {100, 101, 102}, "cold-queued");
    data::proto::DownloadAuthorization authorization;
    {
        data::Store store(directory.path, "historical-data");
        data::Retentions references(directory.path, owner(), store);
        const auto record =
            store.archive().get(selected.at("source_dataset_ids")[0].get<std::string>());
        authorization.set_data_instance("historical-data");
        authorization.set_task_instance("historical-data");
        authorization.set_task_id("queued-download");
        authorization.set_id(sha256_bytes("historical-data/historical-data/queued-download"));
        authorization.set_provider_id("fixture.provider");
        authorization.set_provider_artifact(std::string(64, 'b'));
        *authorization.mutable_minutes() = record.minutes();
    }
    fs::create_directory(directory.path / "acquisition-tasks");
    {
        tasks::Store store(
            asterion::data::task_domain(),
            directory.path / "acquisition-tasks",
            {"historical-data"});
        auto submission = task_submission(store, authorization);
        store.admit_submission(submission);
        submission.prepare_files();
        (void)store.commit(store.register_submission(submission));
    }
    const auto before = sha256_file(directory.path / "acquisition-tasks" / "tasks.sqlite");
    const auto exported = data::cold_backup(directory.path, owner());
    EXPECT_TRUE(std::ranges::any_of(exported.snapshot.files, [](const auto& file) {
        return file.relative == "acquisition-tasks/queued-download/input.pb";
    }));
    EXPECT_TRUE(std::ranges::any_of(exported.snapshot.dependencies, [](const auto& dependency) {
        return dependency.object.kind() == "provider_module" && !dependency.included;
    }));
    EXPECT_EQ(sha256_file(directory.path / "acquisition-tasks" / "tasks.sqlite"), before);
}

TEST(DataColdBackup, RestoresRetiredAcquisitionEvidenceWithoutResurrectingExecution)
{
    Folder directory, backup, restored;
    const auto selected = test::seed_history(directory.path, {100, 101}, "cold-retired");
    data::proto::DownloadAuthorization authorization;
    {
        data::Store store(directory.path, "historical-data");
        data::Retentions references(directory.path, owner(), store);
        const auto record =
            store.archive().get(selected.at("source_dataset_ids")[0].get<std::string>());
        authorization.set_data_instance("historical-data");
        authorization.set_task_instance("historical-data");
        authorization.set_task_id("retired-download");
        authorization.set_id(sha256_bytes("retired-authorization"));
        authorization.set_provider_id("fixture.provider");
        authorization.set_provider_artifact(std::string(64, 'b'));
        *authorization.mutable_minutes() = record.minutes();
    }
    fs::create_directory(directory.path / "acquisition-tasks");
    resource::proto::Object decision;
    {
        tasks::Store store(
            asterion::data::task_domain(),
            directory.path / "acquisition-tasks",
            {"historical-data"});
        auto submission = task_submission(store, authorization);
        store.admit_submission(submission);
        submission.prepare_files();
        (void)store.commit(store.register_submission(submission));
        store.commit(store.cancel(authorization.task_id()));
        decision = store.retirement(authorization.task_id(), owner().node_id()).object();
        EXPECT_EQ(decision.kind(), "minutes");
        store.commit(store.retire(decision));
    }
    const auto original =
        directory.path / "acquisition-tasks" / authorization.task_id() / "input.pb";
    const auto digest = sha256_file(original);
    {
        auto exported = data::cold_backup(directory.path, owner());
        std::vector<backup_files::Snapshot> sources;
        sources.push_back(std::move(exported.snapshot));
        backup_files::Cut cut(std::move(sources));
        const auto plan = cut.preview("retired-acquisition");
        (void)cut.save("retired-acquisition", backup.path / "bundle", sha256_bytes(plan.dump()));
    }
    (void)backup_files::restore_files(backup.path / "bundle", restored.path / "candidate");
    const auto copied = restored.path / "candidate/owners/0";
    {
        auto verified = data::cold_backup(copied, owner());
        EXPECT_FALSE(verified.snapshot.files.empty());
    }
    tasks::Store store(
        asterion::data::task_domain(),
        copied / "acquisition-tasks",
        {"historical-data"});
    const auto recovered = store.retirement(authorization.task_id(), owner().node_id());
    ASSERT_EQ(recovered.state(), task::proto::TaskRetirement::RETIRED);
    EXPECT_EQ(recovered.object().SerializeAsString(), decision.SerializeAsString());
    EXPECT_THROW((void)store.retry(authorization.task_id()), Error);
    EXPECT_EQ(store.get(authorization.task_id()).download_authorization(), authorization.id());
    EXPECT_EQ(
        sha256_file(copied / "acquisition-tasks" / authorization.task_id() / "input.pb"),
        digest);
    EXPECT_EQ(sha256_file(original), digest);
}

TEST(DataColdBackup, PreservesTableRetirementAndRejectsMissingRecoveryAuthority)
{
    Folder source, backup, restored;
    (void)test::seed_history(source.path, {100, 101}, "table-retirement-backup");
    seal(source.path);
    std::string retired, object, head;
    {
        history_files::TableStore tables(source.path / "tables");
        const auto& daily = history_table("futures.daily");
        const history_files::TableSeries series{"futures.daily", 1, "fixture.daily.v1"};
        const auto partition = [&](const std::string& close) {
            TableSourceRow row(daily.columns.size());
            row[0] = "SHFE/rb/2026-10";
            row[4] = close;
            return table_partition(daily, "SHFE", "2025-01-01", {row});
        };
        const auto a = partition("100"), b = partition("200");
        const auto first =
            tables.record(a, series, tables.write(a), std::string(64, 'a'), 1000).revision;
        retired = first.snapshot;
        object = first.object;
        head =
            tables.record(b, series, tables.write(b), std::string(64, 'a'), 2000).revision.snapshot;
        const auto decision = tables.preview_retirement(retired);
        ASSERT_EQ(decision.objects, std::vector<std::string>{object});
        tables.begin_retirement(decision);
        tables.clean_retirement(retired);
        tables.finish_retirement(retired);
    }
    const auto object_path =
        fs::path("tables/objects") / object.substr(0, 2) / (object + ".parquet");
    ASSERT_FALSE(fs::exists(source.path / object_path));
    {
        auto exported = data::cold_backup(source.path, owner());
        EXPECT_TRUE(std::ranges::none_of(exported.objects, [&](const auto& item) {
            return item.id() == retired;
        }));
        std::vector<backup_files::Snapshot> sources;
        sources.push_back(std::move(exported.snapshot));
        backup_files::Cut cut(std::move(sources));
        const auto plan = cut.preview("table-retirement");
        (void)cut.save("table-retirement", backup.path / "bundle", sha256_bytes(plan.dump()));
    }
    (void)backup_files::restore_files(backup.path / "bundle", restored.path / "candidate");
    const auto copied = restored.path / "candidate/owners/0";
    {
        auto verified = data::cold_backup(copied, owner());
        EXPECT_FALSE(verified.snapshot.files.empty());
    }
    {
        history_files::TableStore tables(
            copied / "tables",
            history_files::TableStore::Access::cold_read_only);
        EXPECT_FALSE(tables.sequence(retired));
        EXPECT_EQ(tables.head(), head);
        ASSERT_TRUE(tables.retirement(retired)->complete);
        EXPECT_NO_THROW(tables.verify_history());
    }
    EXPECT_FALSE(fs::exists(copied / object_path));
    const auto before = sha256_file(copied / "tables/ledger.sqlite");
    fs::rename(copied / "tables/retirements.required", copied / "tables/preserved-guard");
    EXPECT_THROW(data::cold_backup(copied, owner()), std::runtime_error);
    EXPECT_EQ(sha256_file(copied / "tables/ledger.sqlite"), before);
    EXPECT_FALSE(fs::exists(source.path / object_path));
}

TEST(DataColdBackup, RetainsExactlyAuthorizedProviderAndRestoresWithoutLoadingOrAuthorizingIt)
{
    configure_native_plugins(PLUGIN_FIXTURE_DIRECTORY);
    Folder directory;
    seal(directory.path);
    data::proto::DownloadAuthorizationRequest request;
    request.set_task_instance("historical-data");
    request.set_task_id("program-download");
    auto* minutes = request.mutable_minutes();
    minutes->set_contract_id("SHFE/cu/2024-03");
    minutes->set_source("fixture.minutes");
    minutes->set_source_instrument("vendor/cu2403");
    minutes->set_interval_minutes(1);
    minutes->set_begin_ns(parse_shanghai_time("2024-03-01 09:01:00"));
    minutes->set_end_ns(parse_shanghai_time("2024-03-01 09:02:00"));
    minutes->set_requests_per_minute(60);
    const auto artifact = history_providers::artifact("fixture.minutes");
    const auto file =
        fs::path("programs") / (artifact + (current_platform().os == "macos" ? ".dylib" : ".so"));
    data::proto::DownloadAuthorization authorization;
    {
        data::Store store(directory.path, "historical-data");
        data::Retentions references(directory.path, owner(), store);
        std::stop_source cancel;
        cancel.request_stop();
        EXPECT_THROW(store.verify_authorization(request, cancel.get_token()), Error);
        EXPECT_FALSE(fs::exists(directory.path / file));
        const auto verified = store.verify_authorization(request);
        // Program publication precedes the durable authorization commit.
        EXPECT_EQ(sha256_file(directory.path / file), artifact);
        EXPECT_TRUE(fs::is_empty(directory.path / "authorizations"));
        authorization = store.authorize(verified);
        EXPECT_EQ(authorization.provider_artifact(), artifact);
        request.set_task_id("same-program-another-task");
        (void)store.verify_authorization(request);
        EXPECT_EQ(sha256_file(directory.path / file), artifact);
    }
    fs::create_directory(directory.path / "acquisition-tasks");
    {
        tasks::Store store(
            asterion::data::task_domain(),
            directory.path / "acquisition-tasks",
            {"historical-data"});
        auto submission = task_submission(store, authorization);
        store.admit_submission(submission);
        submission.prepare_files();
        (void)store.commit(store.register_submission(submission));
    }
    auto exported = data::cold_backup(directory.path, owner());
    ASSERT_TRUE(std::ranges::any_of(exported.objects, [&](const auto& object) {
        return object.kind() == "provider_module" && object.id() == artifact
            && object.version() == artifact;
    }));
    ASSERT_EQ(exported.snapshot.dependencies.size(), 1U);
    EXPECT_EQ(exported.snapshot.dependencies[0].object.id(), artifact);
    auto coordinated = terminal_backup::cold_cut({{exported.snapshot.owner, directory.path}});
    const auto closed = coordinated.preview("provider-dependency-closure");
    ASSERT_EQ(closed.at("dependencies").size(), 1U);
    EXPECT_TRUE(closed.at("dependencies")[0].at("included").get<bool>());

    EXPECT_TRUE(std::ranges::any_of(exported.snapshot.files, [&](const auto& source) {
        return source.relative == file && source.format == "binary";
    }));
    EXPECT_NE(
        std::ranges::find(
            exported.snapshot.limitations,
            "original_acquisition_program_identity_not_recorded"),
        exported.snapshot.limitations.end());
    std::vector<backup_files::Snapshot> sources;
    sources.push_back(std::move(exported.snapshot));
    backup_files::Cut cut(std::move(sources));
    const auto preview = cut.preview("provider-program");
    const auto saved = directory.path.parent_path() / ("ast-program-backup-" + unique_process_id());
    const auto restored =
        directory.path.parent_path() / ("ast-program-restore-" + unique_process_id());

    struct Cleanup
    {
        fs::path saved, restored;

        ~Cleanup()
        {
            std::error_code error;
            fs::remove_all(saved, error);
            fs::remove_all(restored, error);
        }
    } cleanup{saved, restored};

    (void)cut.save("provider-program", saved, sha256_bytes(preview.dump()));
    (void)backup_files::restore_files(saved, restored);
    const auto copied = restored / "owners/0";
    EXPECT_EQ(sha256_file(copied / file), artifact);
    EXPECT_EQ(fs::hard_link_count(copied / file), 1U);
    EXPECT_FALSE(fs::exists(copied / "authorizations"));
    const auto before = sha256_file(copied / "acquisition-tasks/tasks.sqlite");
    EXPECT_NO_THROW(data::cold_backup(copied, owner()));
    EXPECT_EQ(sha256_file(copied / "acquisition-tasks/tasks.sqlite"), before);
    replace_file_durably(copied / file, "synthetic corrupt program");
    EXPECT_THROW(data::cold_backup(copied, owner()), std::invalid_argument);
    EXPECT_THROW(
        history_providers::preserve_artifact(artifact, copied / "programs"),
        std::invalid_argument);
}
