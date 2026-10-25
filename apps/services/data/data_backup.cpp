#include "data_backup.hpp"
#include "observation_store.hpp"
#include "material_store.hpp"
#include "task_store.hpp"
#include "history_archive.hpp"
#include "history_tables.hpp"
#include "history_minutes.hpp"
#include "sqlite_database.hpp"
#include <asterion/foundation/error.hpp>
#include <asterion/foundation/id.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/protocol/trading.hpp>
#include <fstream>
#include <set>
#include <algorithm>

namespace asterion::data {
namespace fs = std::filesystem;

namespace {

std::string bytes(const fs::path& path, std::uintmax_t maximum)
{
    if (fs::is_symlink(path) || !fs::is_regular_file(path) || fs::file_size(path) > maximum)
        throw std::invalid_argument("unsupported Data backup metadata");
    std::ifstream input(path, std::ios::binary);
    std::string value{std::istreambuf_iterator<char>(input), {}};
    if (input.bad())
        throw std::runtime_error("Data backup metadata read failed");
    return value;
}

bool digest(const std::string& text)
{
    return text.size() == 64 && text.find_first_not_of("0123456789abcdef") == std::string::npos;
}

bool runtime_file(const fs::path& path)
{
    const auto name = path.filename().string();
    if (name.ends_with("-wal") || name.ends_with("-journal")) {
        if (fs::file_size(path))
            throw Error(ErrorCode::unavailable, "Data requires owner recovery before cold backup");
        return true;
    }
    return name.ends_with("-shm") || name.starts_with(".tmp-");
}

} // namespace

ColdBackup
cold_backup(const fs::path& directory, const proto::DataOwner& owner, std::stop_token stop)
{
    validate_id(owner.node_id());
    validate_id(owner.service_id());
    if (!directory.is_absolute() || fs::is_symlink(directory) || !fs::is_directory(directory))
        throw std::invalid_argument("Data cold backup requires an existing owned root");
    const auto root = fs::canonical(directory);
    ColdBackup result;
    auto& snapshot = result.snapshot;
    snapshot.directory = root;
    snapshot.owner.set_node_id(owner.node_id());
    snapshot.owner.set_service_id(owner.service_id());
    snapshot.owner.set_domain("data");
    snapshot.ownership =
        std::make_unique<FileLock>(root, "data.lock", FileLock::Access::shared_existing);
    if (parse_json(bytes(root / "instance.json", 4096))
        != Json{{"data_instance", owner.service_id()}})
        throw std::invalid_argument("Data backup owner identity mismatch");
    const auto add = [&](const fs::path& relative, const std::string& format) {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "Data cold backup interrupted");
        const auto source = root / relative;
        if (fs::is_symlink(source) || !fs::is_regular_file(source))
            throw std::invalid_argument("Data backup evidence is missing");
        if (format == "coordination" && fs::file_size(source) != 0)
            throw std::invalid_argument("Data coordination slot is not empty");
        snapshot.files.push_back({source, relative, format});
        if (snapshot.files.size() > 100000)
            throw Error(ErrorCode::resource_exhausted, "Data backup exceeds file capacity");
    };
    const auto optional = [&](const fs::path& relative, const std::string& format) {
        if (fs::exists(root / relative))
            add(relative, format);
    };
    add("instance.json", "json");
    add("data.lock", "coordination");
    snapshot.exclusions =
        {"provider_credentials", "quota_windows", "runtime_sockets", "logs", "temporary_files"};
    for (const auto& entry : fs::directory_iterator(root)) {
        const auto name = entry.path().filename().string();
        if (name == "authorizations" || name == "budgets")
            continue; // Never open excluded secret stores.
        if (entry.is_symlink())
            throw std::invalid_argument("Data backup rejects symbolic links");
        if (entry.is_regular_file() && runtime_file(entry.path()))
            continue;
        if (name != "instance.json" && name != "data.lock" && name != "history" && name != "tables"
            && name != "downloads" && name != "acquisition-tasks" && name != "retentions.sqlite"
            && name != "retentions.required.pb" && name != "programs" && name != "observations"
            && name != "observations.required.pb" && name != "materials"
            && name != "materials.required.pb")
            throw std::invalid_argument("unknown Data root entry; owner inspection required");
    }
    for (const auto& database :
        {root / "retentions.sqlite",
            root / "history/retirements.sqlite",
            root / "tables/retirements.sqlite",
            root / "tables/ledger.sqlite",
            root / "acquisition-tasks/tasks.sqlite"})
        for (const auto& suffix : {"-wal", "-journal"}) {
            const fs::path journal(database.string() + suffix);
            if (fs::exists(journal))
                (void)runtime_file(journal);
        }
    history_files::Archive archive(
        root / "history",
        history_files::Archive::Access::cold_read_only);
    history_files::TableStore tables(
        root / "tables",
        history_files::TableStore::Access::cold_read_only);
    tables.verify_history(stop);
    snapshot
        .directories = {"history/index", "history/named-datasets", "tables/objects", "downloads"};
    optional("history/archive.lock", "coordination");
    optional("history/datasets.lock", "coordination");
    optional("history/retirements.sqlite", "sqlite");
    optional("history/retirements.required", "binary");
    const auto minute_files =
        [&](const proto::HistoryRecord& record, const Json& manifest, bool incomplete) {
            const auto path = fs::path(record.minute_result().directory());
            const auto relative = path.lexically_relative(root);
            snapshot.directories.push_back(relative.parent_path());
            if (!fs::exists(path)) {
                if (!incomplete)
                    throw std::invalid_argument("published minute payload missing");
                return;
            }
            optional(relative / "minutes.lock", "coordination");
            optional(relative / "minutes.json", "json");
            for (const auto& segment : manifest.at("segments")) {
                const auto file = segment.at("file").get<std::string>();
                if (fs::path(file).filename() != file)
                    throw std::invalid_argument("invalid minute segment path");
                if (fs::exists(path / file)) {
                    if (sha256_file(path / file) != segment.at("sha256").get<std::string>())
                        throw std::invalid_argument("minute backup segment is corrupt");
                    add(relative / file, "parquet");
                } else if (!incomplete)
                    throw std::invalid_argument("minute backup segment missing");
            }
        };
    const auto object = [&](const std::string& kind, const std::string& version) {
        resource::proto::Object value;
        *value.mutable_owner() = snapshot.owner;
        value.set_kind(kind);
        value.set_id(version);
        value.set_version(version);
        result.objects.push_back(std::move(value));
        if (result.objects.size() > 100000)
            throw Error(ErrorCode::resource_exhausted, "Data backup object count exceeds capacity");
    };
    for (const auto& item : archive.datasets({})) {
        const auto record = archive.get(item.id);
        object("minute_version", item.id);
        if (!fs::is_regular_file(fs::path(record.minute_result().directory()) / "minutes.lock"))
            throw std::invalid_argument(
                "minute coordination slot missing; owner recovery required");
        const auto manifest = history_files::inspect_minutes(record.minute_result().directory());
        if (!manifest.at("complete").get<bool>() || sha256_bytes(manifest.dump()) != item.id)
            throw std::invalid_argument("minute backup publication evidence mismatch");
        add(fs::path("history/index") / (item.id + ".pb"), "protobuf");
        minute_files(record, manifest, false);
    }
    std::string after;
    for (;;) {
        const auto page = archive.retirements(after, 256);
        for (const auto& retired : page) {
            const auto id = retired.record().minute_result().manifest_sha256();
            add(fs::path("history/index") / (id + ".pb"), "protobuf");
            sqlite::Database read(
                root / "history/retirements.sqlite",
                sqlite::Database::Access::cold_read_only);
            sqlite::Database::Statement row(
                read,
                "SELECT CASE WHEN length(manifest)<=8388608 THEN manifest END FROM retirements WHERE id=?");
            row.bind(1, id);
            if (!row.step())
                throw std::invalid_argument("minute retirement backup evidence missing");
            const auto raw = row.blob(0);
            if (sha256_bytes(raw) != id)
                throw std::invalid_argument("retired minute manifest corrupt");
            minute_files(retired.record(), parse_json(raw), true);
            after = id;
        }
        if (page.size() < 256)
            break;
    }
    const auto named_datasets = archive.named_datasets();
    for (const auto& named : named_datasets.items()) {
        add(fs::path("history/named-datasets") / (named.id() + ".pb"), "protobuf");
        for (const auto& selection : named.selections()) {
            if (!tables.sequence(selection.daily().snapshot()))
                throw std::invalid_argument("named dataset table dependency missing");
            for (const auto& id : selection.source_dataset_ids())
                (void)archive.get(id);
        }
    }
    add("tables/ledger.sqlite", "sqlite");
    optional("tables/retirements.sqlite", "sqlite");
    optional("tables/retirements.required", "binary");
    {
        sqlite::Database read(
            root / "tables/ledger.sqlite",
            sqlite::Database::Access::cold_read_only);
        sqlite::Database::Statement snapshots(
            read,
            "SELECT snapshot FROM table_revisions ORDER BY sequence");
        while (snapshots.step())
            if (tables.sequence(snapshots.text(0)))
                object("table_snapshot", snapshots.text(0));
    }

    for (const auto& entry : fs::recursive_directory_iterator(root / "tables")) {
        if (entry.is_symlink())
            throw std::invalid_argument("table backup rejects symbolic links");
        if (!entry.is_regular_file())
            continue;
        const auto local = entry.path().lexically_relative(root / "tables");
        if (local == "ledger.sqlite" || local == "retirements.sqlite"
            || local == "retirements.required" || runtime_file(entry.path()))
            continue;
        const auto name = entry.path().filename().string();
        if (name.starts_with("incoming-"))
            continue;
        const auto top = local.begin()->string();
        bool known = false;
        std::string format;
        if (top == "objects"
            && local.parent_path()
                == fs::path("objects") / entry.path().stem().string().substr(0, 2)
            && entry.path().extension() == ".parquet" && digest(entry.path().stem().string())) {
            known = sha256_file(entry.path()) == entry.path().stem().string();
            format = "parquet";
        } else if ((local.parent_path() == "responses/objects"
                       || local.parent_path() == "responses/revisions"
                       || local.parent_path() == "observations/objects")
            && entry.path().extension() == ".json" && digest(entry.path().stem().string())) {
            known = local.parent_path() == "responses/revisions"
                || sha256_file(entry.path()) == entry.path().stem().string();
            format = "json";
        } else if (local.parent_path().parent_path() == "observations/partitions"
            && digest(local.parent_path().filename().string()) && entry.path().extension().empty()
            && digest(name)) {
            known = digest(bytes(entry.path(), 64));
            format = "binary";
        }
        if (!known)
            throw std::invalid_argument("unknown or corrupt table backup file");
        add(fs::path("tables") / local, format);
    }
    if (!fs::is_regular_file(root / "retentions.sqlite"))
        throw Error(
            ErrorCode::unavailable,
            "Data owner identity is not sealed; owner initialization required");
    if (fs::exists(root / "retentions.required.pb")) {
        if (bytes(root / "retentions.required.pb", 4096) != owner.SerializeAsString())
            throw std::invalid_argument("Data retention guard owner mismatch");
        if (!fs::exists(root / "retentions.sqlite"))
            throw std::invalid_argument("Data retention authority missing");
        add("retentions.required.pb", "protobuf");
    }
    if (fs::exists(root / "retentions.sqlite")) {
        sqlite::Database read(root / "retentions.sqlite", sqlite::Database::Access::cold_read_only);
        sqlite::Database::Statement identity(read, "SELECT body FROM retention_owner WHERE id=1");
        if (!identity.step() || identity.blob(0) != owner.SerializeAsString())
            throw std::invalid_argument("Data retention owner mismatch");
        sqlite::Database::Statement rows(
            read,
            "SELECT CASE WHEN length(body)<=1048576 THEN body END,phase FROM retentions");
        unsigned count = 0;
        while (rows.step()) {
            if (++count > 100000)
                throw Error(
                    ErrorCode::resource_exhausted,
                    "Data reference backup exceeds capacity");
            proto::DataRetention retention;
            const auto body = rows.blob(0);
            if (!retention.ParseFromString(body))
                throw std::invalid_argument("invalid Data retention backup record");
            protocol::validate_message(retention);
            if (retention.source().SerializeAsString() != owner.SerializeAsString()
                || rows.integer(1) < 1 || rows.integer(1) > 3
                || !fs::exists(root / "retentions.required.pb"))
                throw std::invalid_argument("Data retention backup state mismatch");
            const auto& consumer = retention.consumer();
            validate_id(consumer.owner().node_id());
            validate_id(consumer.owner().service_id());
            validate_id(consumer.kind());
            validate_id(consumer.id());
            if (!digest(consumer.version()) || retention.versions_size() < 1
                || retention.versions_size() > 4096)
                throw std::invalid_argument("invalid retained consumer or version count");
            std::set<std::pair<int, std::string>> versions;
            for (const auto& version : retention.versions()) {
                if (!digest(version.version())
                    || (version.kind() != proto::DataVersion::MINUTE_VERSION
                        && version.kind() != proto::DataVersion::TABLE_SNAPSHOT))
                    throw std::invalid_argument("unsupported retained Data version");
                versions.emplace(version.kind(), version.version());
            }
            if (versions.size() != static_cast<std::size_t>(retention.versions_size()))
                throw std::invalid_argument("duplicate retained Data versions");
            auto key = consumer;
            key.clear_version();
            sqlite::Database::Statement edges(
                read,
                "SELECT kind,version FROM retained_versions WHERE consumer=? ORDER BY kind,version");
            edges.bind_blob(1, key.SerializeAsString());
            for (const auto& [kind, version] : versions)
                if (!edges.step() || edges.integer(0) != kind || edges.text(1) != version)
                    throw std::invalid_argument("Data retention edges disagree with evidence");
            if (edges.step())
                throw std::invalid_argument("Data retention has undeclared edges");
            if (rows.integer(1) == 3)
                continue;
            for (const auto& version : retention.versions()) {
                if (version.kind() == proto::DataVersion::MINUTE_VERSION)
                    (void)archive.get(version.version());
                else if (version.kind() != proto::DataVersion::TABLE_SNAPSHOT
                    || !tables.sequence(version.version()))
                    throw std::invalid_argument("Data retained version missing");
            }
            result.references.push_back({retention.consumer(), sha256_bytes(body)});
            snapshot.dependencies.push_back({retention.consumer(), false});
        }
        add("retentions.sqlite", "sqlite");
    }
    if (fs::exists(root / "downloads"))
        for (const auto& directory : fs::directory_iterator(root / "downloads")) {
            if (directory.is_symlink() || !directory.is_directory()
                || !digest(directory.path().filename().string()))
                throw std::invalid_argument("unknown Data acquisition evidence");
            snapshot.directories.push_back(directory.path().lexically_relative(root));
            for (const auto& file : fs::directory_iterator(directory.path())) {
                const auto name = file.path().filename().string();
                if (name != "allocation.pb" && name != "prepared.pb" && name != "publication.pb")
                    throw std::invalid_argument("unknown Data acquisition receipt");
                add(file.path().lexically_relative(root), "protobuf");
            }
        }
    if (fs::exists(root / "observations") || fs::exists(root / "observations.required.pb")) {
        Observations observations(root, snapshot.owner);
        const auto operations = observations.operations(stop);
        add("observations.required.pb", "protobuf");
        std::set<fs::path> captured;
        std::set<std::string> published;
        for (const auto& directory : fs::directory_iterator(root / "observations")) {
            const auto name = directory.path().filename().string();
            if (directory.is_symlink() || !directory.is_directory()
                || (name != "operations" && name != "objects" && name != "staging"))
                throw std::invalid_argument("invalid observation archive directory");
        }
        snapshot.directories.push_back("observations/operations");
        snapshot.directories.push_back("observations/objects");
        snapshot.directories.push_back("observations/staging");
        for (const auto& operation : operations) {
            add(fs::path("observations/operations") / (operation.id() + ".pb"), "protobuf");
            bool available = false;
            for (const auto& relative :
                {fs::path("observations/objects") / (operation.source().version() + ".pb"),
                    fs::path("observations/staging") / (operation.id() + ".pb")}) {
                if (!fs::exists(root / relative) && !fs::is_symlink(root / relative))
                    continue;
                market::proto::ObservationCapture capture;
                if (!capture.ParseFromString(
                        bytes(root / relative, protocol::max_observation_bytes)))
                    throw std::invalid_argument("invalid observation archive protobuf");
                protocol::validate_observation(capture);
                if (capture.reference().SerializeAsString()
                    != operation.source().SerializeAsString())
                    throw Error(ErrorCode::conflict, "observation payload identity mismatch");
                available = true;
                if (captured.insert(relative).second)
                    add(relative, "protobuf");
            }
            if (operation.state() == proto::ObservationOperation::PUBLISHED) {
                (void)observations.read(operation.id());
                if (published.insert(operation.archive().id()).second)
                    object("observation_archive", operation.archive().id());
            } else if (!available) {
                snapshot.dependencies.push_back({operation.source(), false});
            }
        }
    }
    if (fs::exists(root / "materials") || fs::exists(root / "materials.required.pb")) {
        Materials materials(root, snapshot.owner);
        const auto receipts = materials.receipts(stop);
        add("materials.required.pb", "protobuf");
        snapshot.directories.push_back("materials/objects");
        snapshot.directories.push_back("materials/receipts");
        for (const auto& entry : fs::directory_iterator(root / "materials"))
            if (entry.is_symlink() || !entry.is_directory()
                || (entry.path().filename() != "objects" && entry.path().filename() != "receipts"))
                throw std::invalid_argument(
                    "unknown material directory; owner inspection required");
        std::set<std::string> captured;
        for (const auto& receipt : receipts) {
            // One verification per shared original while this cut owns the physical root.
            const auto& original = receipt.evidence().content();
            if (captured.insert(original.version()).second) {
                (void)materials.read(receipt.reference().id());
                add(fs::path("materials/objects") / original.version(), "binary");
                result.objects.push_back(original);
            } else if (fs::file_size(root / "materials/objects" / original.version())
                != receipt.evidence().bytes())
                throw std::invalid_argument("material receipt size differs from shared original");
            add(fs::path("materials/receipts") / (receipt.reference().id() + ".pb"), "protobuf");
            result.objects.push_back(receipt.reference());
            resource::proto::Object program;
            *program.mutable_owner() = snapshot.owner;
            program.set_kind("provider_module");
            program.set_id(receipt.evidence().request().provider_artifact());
            program.set_version(program.id());
            snapshot.dependencies.push_back({std::move(program), false});
        }
        bool unreferenced = false;
        for (const auto& entry : fs::directory_iterator(root / "materials/objects")) {
            const auto version = entry.path().filename().string();
            if (entry.is_symlink() || !entry.is_regular_file() || !digest(version))
                throw std::invalid_argument("unknown material original; owner inspection required");
            if (captured.contains(version))
                continue;
            if (fs::file_size(entry.path()) > protocol::max_material_bytes
                || sha256_file(entry.path()) != version)
                throw std::invalid_argument("unconfirmed material original is corrupt");
            add(entry.path().lexically_relative(root), "binary");
            unreferenced = true;
        }
        if (unreferenced)
            snapshot.limitations.push_back("unreferenced_material_original_retained");
    }
    if (fs::exists(root / "programs")) {
        if (!fs::is_directory(root / "programs"))
            throw std::invalid_argument("invalid Data program directory");
        for (const auto& entry : fs::directory_iterator(root / "programs")) {
            if (entry.is_symlink())
                throw std::invalid_argument("Data backup rejects symbolic links");
            if (entry.path().filename() == "staging" && entry.is_directory())
                continue; // Unpublished temporary bytes are not formal programs.
            const auto id = entry.path().stem().string();
            const auto extension = entry.path().extension().string();
            if (!entry.is_regular_file() || !digest(id)
                || (extension != ".so" && extension != ".dylib")
                || fs::file_size(entry.path()) > max_artifact_bytes
                || sha256_file(entry.path()) != id)
                throw std::invalid_argument("invalid retained Data provider program");
            const auto platform = artifact_platform(entry.path());
            if ((platform.os == "macos" ? ".dylib" : ".so") != extension)
                throw std::invalid_argument("Data provider program platform mismatch");
            add(entry.path().lexically_relative(root), "binary");
            object("provider_module", id);
        }
    }
    if (fs::exists(root / "acquisition-tasks")) {
        const auto program_dependency = [&](const task::proto::StoredTaskRecord& record,
                                            const task::proto::TaskExecution&) {
            if (std::ranges::find(
                    snapshot.limitations,
                    "original_acquisition_program_identity_not_recorded")
                == snapshot.limitations.end())
                snapshot.limitations.push_back(
                    "original_acquisition_program_identity_not_recorded");
            const auto& artifact = record.task().provider_artifact();
            if (artifact.empty())
                return;
            resource::proto::Object required;
            *required.mutable_owner() = snapshot.owner;
            required.set_kind("provider_module");
            required.set_id(artifact);
            required.set_version(artifact);
            snapshot.dependencies.push_back({std::move(required), false});
        };
        const auto catalogue = tasks::Store::inspect_backup(
            root / "acquisition-tasks",
            {owner.service_id()},
            program_dependency,
            program_dependency,
            {},
            stop);
        for (const auto& file : catalogue.files)
            add(fs::path("acquisition-tasks") / file,
                file.filename() == "manager.lock"   ? "coordination"
                    : file.extension() == ".sqlite" ? "sqlite"
                    : file.extension() == ".json"   ? "json"
                                                    : "protobuf");
        for (const auto& path : catalogue.directories)
            snapshot.directories.push_back(fs::path("acquisition-tasks") / path);
    }
    std::set<fs::path> declared;
    for (const auto& file : snapshot.files)
        declared.insert(file.relative);
    for (const auto& base : {"history", "acquisition-tasks", "observations", "materials"}) {
        if (!fs::exists(root / base))
            continue;
        for (const auto& entry : fs::recursive_directory_iterator(root / base)) {
            if (entry.is_symlink())
                throw std::invalid_argument("Data backup rejects symbolic links");
            if (entry.is_regular_file() && !declared.contains(entry.path().lexically_relative(root))
                && !runtime_file(entry.path()))
                throw std::invalid_argument("undeclared Data evidence; owner inspection required");
        }
    }
    std::ranges::sort(snapshot.directories);
    snapshot.directories.erase(
        std::unique(snapshot.directories.begin(), snapshot.directories.end()),
        snapshot.directories.end());
    return result;
}

} // namespace asterion::data
