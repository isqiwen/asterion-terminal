#include "task_store.hpp"
#include "task_events.hpp"
#include "task_payload.hpp"
#include "sqlite_database.hpp"
#include <asterion/foundation/error.hpp>
#include <asterion/foundation/id.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/file_lock.hpp>
#include <asterion/protocol/data.hpp>
#include <asterion/protocol/resource.hpp>
#include <algorithm>
#include <charconv>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <google/protobuf/util/json_util.h>

namespace asterion::tasks {
namespace wire = task::proto;
namespace fs = std::filesystem;

// A domain's kinds with their message fields resolved once.
class Kinds
{
public:
    struct Kind
    {
        const KindSpec* spec;
        const google::protobuf::FieldDescriptor *definition, *result, *finish;
    };

    explicit Kinds(const TaskDomain& source)
        : domain(source)
    {
        const auto field = [](const google::protobuf::Descriptor* message, std::string_view name) {
            const auto* found = message->FindFieldByName(std::string(name));
            // A missing field name is a programming error in the domain's table.
            if (!found || found->type() != google::protobuf::FieldDescriptor::TYPE_MESSAGE)
                throw std::logic_error("task kind names a missing message field");
            return found;
        };
        for (const auto& spec : domain.kinds())
            kinds_.emplace(
                spec.kind,
                Kind{
                    &spec,
                    field(wire::Task::descriptor(), spec.definition),
                    field(wire::TaskResponse::descriptor(), spec.result),
                    field(wire::TaskFinish::descriptor(), spec.finish)
                });
    }

    const TaskDomain& domain;

    const Kind& at(wire::TaskKind kind) const
    {
        const auto found = kinds_.find(kind);
        if (found == kinds_.end())
            throw Error(ErrorCode::conflict, "unsupported task kind");
        return found->second;
    }

    bool has(wire::TaskKind kind) const
    {
        return kinds_.contains(kind);
    }

    const KindSpec& spec(wire::TaskKind kind) const
    {
        return *at(kind).spec;
    }

    const Kind* manifest(std::string_view type) const
    {
        for (const auto& [_, kind] : kinds_)
            if (kind.spec->manifest == type)
                return &kind;
        return nullptr;
    }

    // The kind whose definition a submitted task carries.
    wire::TaskKind of(const wire::Task& task) const
    {
        for (const auto& [kind, resolved] : kinds_)
            if (task.GetReflection()->HasField(task, resolved.definition))
                return kind;
        throw std::invalid_argument("task requires an explicit input type");
    }

    bool reserved(std::string_view name) const
    {
        return std::ranges::find(domain.directories(), name) != domain.directories().end();
    }

private:
    std::map<wire::TaskKind, Kind> kinds_;
};

namespace {

google::protobuf::Message& mutable_definition(const Kinds& kinds, wire::Task& task)
{
    return *task.GetReflection()->MutableMessage(&task, kinds.at(task.kind()).definition);
}

google::protobuf::Message&
mutable_result(const Kinds& kinds, wire::TaskResponse& response, wire::TaskKind kind)
{
    return *response.GetReflection()->MutableMessage(&response, kinds.at(kind).result);
}

void validate_task_id(const Kinds& kinds, const std::string& id)
{
    validate_id(id);
    if (id.find_first_of("/\\") != std::string::npos || id == "." || id == ".."
        || id == "manager.lock")
        throw std::invalid_argument("invalid task id");
    if (id == "history" || id == "event-segments" || kinds.reserved(id))
        throw std::invalid_argument("reserved task identifier");
}

void safe(const fs::path& path)
{
    if (fs::is_symlink(path))
        throw std::invalid_argument("task store rejects symbolic links");
}

void prepare(const Kinds& kinds, wire::Task& task)
{
    task.set_kind(kinds.of(task));
    kinds.domain.describe(task);
    task.set_state(wire::QUEUED);
}

const google::protobuf::Message& definition(const Kinds& kinds, const wire::Task& task)
{
    const auto* field = kinds.at(task.kind()).definition;
    if (!task.GetReflection()->HasField(task, field))
        throw std::invalid_argument("task kind and input disagree");
    return task.GetReflection()->GetMessage(task, field);
}

// Lists, dispatch and durable transitions need bounded metadata, never a copy
// of the immutable task dataset: every field but the definition. Definitions
// are loaded from owned files on demand.
wire::Task summary(const wire::Task& task)
{
    wire::Task result;
    const auto* reflection = task.GetReflection();
    std::vector<const google::protobuf::FieldDescriptor*> fields;
    reflection->ListFields(task, &fields);
    for (const auto* field : fields) {
        if (field->real_containing_oneof())
            continue;
        using Type = google::protobuf::FieldDescriptor::CppType;
        switch (field->cpp_type()) {
            case Type::CPPTYPE_MESSAGE:
                reflection->MutableMessage(&result, field)
                    ->CopyFrom(reflection->GetMessage(task, field));
                break;
            case Type::CPPTYPE_STRING:
                reflection->SetString(&result, field, reflection->GetString(task, field));
                break;
            case Type::CPPTYPE_UINT32:
                reflection->SetUInt32(&result, field, reflection->GetUInt32(task, field));
                break;
            case Type::CPPTYPE_INT64:
                reflection->SetInt64(&result, field, reflection->GetInt64(task, field));
                break;
            case Type::CPPTYPE_ENUM:
                reflection->SetEnumValue(&result, field, reflection->GetEnumValue(task, field));
                break;
            default:
                throw std::logic_error("task summary has an unsupported field type");
        }
    }
    return result;
}

// What a definition was admitted as. prepare() derives it once, when the task is
// submitted; the index keeps it from then on.
void validate_metadata(const wire::TaskMetadata& metadata)
{
    if (metadata.name().size() > 256 || metadata.note().size() > 8192
        || metadata.name().find_first_of("\r\n") != std::string::npos
        || metadata.name().find('\0') != std::string::npos
        || metadata.note().find('\0') != std::string::npos)
        throw std::invalid_argument("invalid experiment name or note");
    if (metadata.has_derivation()) {
        const auto& source = metadata.derivation();
        validate_id(source.source_id());
        validate_id(source.source_instance());
        payload::validate_digest(source.source_result_digest());
        if (source.data_choice() == "snapshot")
            payload::validate_digest(source.snapshot());
        else if (source.data_choice() != "fixed" || !source.snapshot().empty())
            throw std::invalid_argument("invalid experiment derivation");
    }
}

Json metadata_json(const wire::TaskMetadata& metadata)
{
    std::string value;
    if (!google::protobuf::util::MessageToJsonString(metadata, &value).ok())
        throw std::invalid_argument("invalid experiment metadata");
    return Json::parse(value);
}

wire::TaskMetadata read_metadata(const Json& value)
{
    wire::TaskMetadata metadata;
    if (!google::protobuf::util::JsonStringToMessage(value.dump(), &metadata).ok())
        throw std::invalid_argument("invalid experiment metadata");
    validate_metadata(metadata);
    return metadata;
}

void admit_as(wire::Task& task, const wire::Task& admitted)
{
    if (admitted.has_table_refresh())
        *task.mutable_table_refresh() = admitted.table_refresh();
    task.set_total(admitted.total());
    task.set_trading_day(admitted.trading_day());
    task.set_instrument(admitted.instrument());
    task.set_source_name(admitted.source_name());
    task.set_data_source(admitted.data_source());
    task.set_minute_interval_minutes(admitted.minute_interval_minutes());
}

void transition(
    const Kinds& kinds,
    const wire::Task& before,
    const std::string& old_token,
    const wire::Task& after,
    const std::string& token)
{
    if (before.has_retirement())
        throw Error(ErrorCode::conflict, "retired task cannot execute again");
    const auto a = before.state(), b = after.state();
    const bool start = a == wire::QUEUED && b == wire::RUNNING;
    const bool launch_failed = a == wire::QUEUED && b == wire::FAILED;
    const bool retry =
        (a == wire::FAILED || a == wire::INTERRUPTED || a == wire::CANCELLED) && b == wire::QUEUED;
    const bool running = a == wire::RUNNING
        && (b == wire::RUNNING || b == wire::CANCEL_REQUESTED || b == wire::SUCCEEDED
            || b == wire::PUBLISHING || b == wire::FAILED || b == wire::INTERRUPTED);
    const bool cancelling = a == wire::CANCEL_REQUESTED
        && (b == wire::CANCEL_REQUESTED || b == wire::CANCELLED || b == wire::INTERRUPTED);
    if (!(start || retry || running || cancelling || (a == wire::PUBLISHING && b == wire::SUCCEEDED)
            || (a == wire::QUEUED && (b == wire::CANCELLED || b == wire::FAILED))))
        throw std::invalid_argument("invalid durable task transition");
    if (after.attempt() > 100
        || after.attempt() != before.attempt() + (start || launch_failed ? 1U : 0U)
        || after.completed() > after.total())
        throw std::invalid_argument("invalid task attempt or progress");
    if (start || retry) {
        if (after.completed() != 0)
            throw std::invalid_argument("new attempt has progress");
    } else if (after.completed() < before.completed())
        throw std::invalid_argument("task progress moved backwards");
    if (start) {
        if (token.size() != 32 || token.find_first_not_of("0123456789abcdef") != std::string::npos
            || token == old_token)
            throw std::invalid_argument("invalid new attempt token");
    } else if (retry || b == wire::INTERRUPTED || b == wire::PUBLISHING) {
        if (!token.empty())
            throw std::invalid_argument("inactive attempt retains token");
    } else if (token != old_token)
        throw std::invalid_argument("task token changed mid-attempt");
    const bool download = kinds.spec(after.kind()).publishes;
    if (b == wire::PUBLISHING || (download && b == wire::SUCCEEDED)) {
        const auto& publication = after.publication();
        const auto& identity = publication.identity();
        validate_id(identity.data_instance());
        validate_id(identity.task_instance());
        validate_id(publication.publication_id());
        if (!download || identity.task_id() != after.id() || identity.attempt() != after.attempt()
            || publication.candidate_digest().size() != 64
            || publication.candidate_digest().find_first_not_of("0123456789abcdef")
                != std::string::npos
            || after.history_dataset_id().size() != 64
            || after.history_dataset_id().find_first_not_of("0123456789abcdef") != std::string::npos
            || (b == wire::SUCCEEDED && a != wire::PUBLISHING))
            throw std::invalid_argument("invalid task publication decision");
    } else if (after.has_publication() || !after.history_dataset_id().empty())
        throw std::invalid_argument("task retains an unexpected publication decision");
    if (a == wire::PUBLISHING
        && (before.publication().SerializeAsString() != after.publication().SerializeAsString()
            || before.history_dataset_id() != after.history_dataset_id()
            || before.result_digest() != after.result_digest()))
        throw std::invalid_argument("task publication identity changed");
    if (b == wire::SUCCEEDED || b == wire::PUBLISHING) {
        if (after.completed() != after.total() || after.result_digest().size() != 64
            || after.result_digest().find_first_not_of("0123456789abcdef") != std::string::npos)
            throw std::invalid_argument("invalid committed result identity");
    } else if (!after.result_digest().empty())
        throw std::invalid_argument("unfinished task has result digest");
    if (after.error().size() > 1024)
        throw std::invalid_argument("task error exceeds limit");
}

// Parse the exact bytes already checked against their durable digest.
wire::Task parse_input(const Kinds& kinds, wire::Task task, const std::string& raw)
{
    payload::parse(raw, mutable_definition(kinds, task));
    kinds.domain.verify_input(task);
    return task;
}

// The owner published this input; reading it again does not re-verify its digest.
wire::Task
read_input(const Kinds& kinds, wire::Task task, const fs::path& path, std::uint64_t bytes)
{
    return parse_input(
        kinds,
        std::move(task),
        payload::load(path, payload::max_input_bytes, bytes));
}

// Cold backup inspection reads files it did not write and verifies them.
wire::Task read_verified_input(
    const Kinds& kinds,
    wire::Task task,
    const fs::path& path,
    const std::string& digest,
    std::uint64_t bytes)
{
    return parse_input(
        kinds,
        std::move(task),
        payload::read(path, digest, payload::max_input_bytes, bytes));
}

wire::Task attach_definition(const Kinds& kinds, wire::Task task, wire::Task source)
{
    if (task.kind() != source.kind())
        throw std::logic_error("task definition comes from another task kind");
    auto& target = mutable_definition(kinds, task);
    target.GetReflection()->Swap(&target, &mutable_definition(kinds, source));
    return task;
}

bool active(wire::TaskState state)
{
    return state == wire::RUNNING || state == wire::CANCEL_REQUESTED;
}

bool occupies_slot(wire::TaskState state)
{
    return state == wire::QUEUED || active(state) || state == wire::PUBLISHING;
}

} // namespace

struct Store::Impl
{
    struct Entry
    {
        wire::Task task; // Metadata only; immutable input stays on disk.
        std::string input_digest;
        std::uint64_t input_bytes = 0;
        std::string token;
        std::uint64_t event_sequence = 0;
    };

    using ActiveEntries = std::map<std::string, Entry>;
    // The state owner retains only admitted work. SQLite is its durable projection
    // and the on-demand index for terminal history, not a second active-state owner.
    ActiveEntries current;

    struct PendingChange
    {
        Entry next;
        std::string record, event, manifest, kind;
        ActiveEntries::node_type active_entry;
        bool resumed_submission = false;
    };

    std::shared_ptr<const Kinds> kinds;
    fs::path root;
    Identity identity;
    std::unique_ptr<FileLock> owner;
    // Task index and state history: one row per task, one per state change.
    std::unique_ptr<sqlite::Database> database;
    std::unique_ptr<TaskEvents> event_log;

    // A submission interrupted after creating its directory but before its index
    // commit leaves the directory; it is not loaded and its ID stays reserved.
    std::uint32_t uncommitted = 0;
    std::optional<std::string> submitting;
    std::set<std::string> retiring;
    bool failed = false;
    bool read_only = false;
    std::shared_ptr<const Clock> clock;
    std::uint32_t last_sequence = 0;

    std::int64_t now_ms() const
    {
        const auto value = clock->utc_now() / 1000000;
        if (value <= 0)
            throw std::invalid_argument("task clock must report a positive UTC time");
        return value;
    }

    Impl(
        std::shared_ptr<const Kinds> resolved,
        fs::path directory,
        Identity binding,
        std::shared_ptr<const Clock> source,
        bool inspect,
        bool bounded_inspection = true,
        std::unique_ptr<FileLock> ownership = {},
        std::stop_token inspection_stop = {},
        bool cold_inspection = false)
        : kinds(std::move(resolved)),
          root(std::move(directory)),
          identity(std::move(binding)),
          owner(std::move(ownership)),
          read_only(inspect),
          clock(std::move(source))
    {
        if (!clock)
            throw std::invalid_argument("task store requires a clock");
        if (!root.is_absolute() || !fs::is_directory(root))
            throw std::invalid_argument("task store requires an existing absolute directory");
        safe(root);
        if (fs::exists(root / "history"))
            throw std::invalid_argument("task store contains an unsupported historical warehouse");
        validate_id(identity.instance);
        const Json expected{{"task_instance", identity.instance}};
        const auto identity_path = root / "instance.json";
        const auto check_identity = [&] {
            safe(identity_path);
            if (!fs::exists(identity_path))
                return false;
            if (!fs::is_regular_file(identity_path) || fs::file_size(identity_path) > 4096)
                throw std::invalid_argument("invalid task instance identity");
            std::ifstream stream(identity_path, std::ios::binary);
            const std::string bytes{std::istreambuf_iterator<char>(stream), {}};
            if (stream.fail())
                throw std::runtime_error("task instance identity read failed");
            if (parse_json(bytes) != expected)
                throw std::invalid_argument("task service instance or data binding mismatch");
            return true;
        };
        const auto require_new_directory = [&] {
            if (read_only)
                throw std::invalid_argument("task directory has no supported instance identity");
            for (const auto& file : fs::directory_iterator(root))
                if (file.path().filename() != "manager.lock" || !file.is_regular_file()
                    || file.is_symlink())
                    throw std::invalid_argument(
                        "task directory has no supported instance identity");
        };
        if (!check_identity())
            require_new_directory();
        const bool existing = fs::exists(root / "tasks.sqlite");
        if (!owner)
            owner = std::make_unique<FileLock>(
                root,
                "manager.lock",
                read_only      ? FileLock::Access::shared_existing
                    : existing ? FileLock::Access::exclusive_existing
                               : FileLock::Access::exclusive);
        if (!read_only && existing) {
            // Keep exclusive ownership across read-only validation and writer open.
            // Invalid stores are rejected before enabling writer journal state.
            Impl inspected(kinds, root, identity, clock, true, false, std::move(owner));
            owner = std::move(inspected.owner);
            current = std::move(inspected.current);
            uncommitted = inspected.uncommitted;
            last_sequence = inspected.last_sequence;
        }
        // Recheck after acquiring ownership: another process may have initialized
        // this new directory before we acquired its lock.
        if (!check_identity()) {
            require_new_directory();
            replace_file_durably(identity_path, expected.dump());
        } else if (!read_only) {
            sync_directory(root);
        }
        safe(root / "tasks.sqlite");
        safe(root / "tasks.sqlite-wal");
        safe(root / "tasks.sqlite-shm");
        database = std::make_unique<sqlite::Database>(
            root / "tasks.sqlite",
            cold_inspection ? sqlite::Database::Access::cold_read_only
                : read_only ? sqlite::Database::Access::read_only
                            : sqlite::Database::Access::writer);
        if (read_only)
            database->execute("BEGIN");
        else {
            sqlite::Database::Transaction schema(*database);
            database->execute(
                "CREATE TABLE IF NOT EXISTS tasks(id TEXT PRIMARY KEY," " sequence INTEGER NOT NULL UNIQUE, kind TEXT NOT NULL," " manifest TEXT NOT NULL, state INTEGER NOT NULL," " attempt INTEGER NOT NULL, updated_at_ms INTEGER NOT NULL, record BLOB " "NOT NULL) STRICT");
            database->execute("CREATE INDEX IF NOT EXISTS tasks_state ON tasks(state, sequence)");
            database->execute(
                "CREATE INDEX IF NOT EXISTS tasks_input ON tasks(kind, json_extract(manifest, '$.input_sha256'))");
            database->execute(
                "CREATE TABLE IF NOT EXISTS preparation_decisions(id TEXT PRIMARY KEY, input_digest TEXT NOT NULL, resource BLOB NOT NULL, evidence BLOB NOT NULL) STRICT");
            database->execute(
                "CREATE TABLE IF NOT EXISTS task_events(task_id TEXT NOT NULL" " REFERENCES tasks(id), sequence INTEGER NOT NULL, body TEXT NOT NULL," " PRIMARY KEY(task_id, sequence)) STRICT");
            schema.commit();
        }
        event_log = std::make_unique<TaskEvents>(*database, root / "event-segments", read_only);
        if (read_only || !existing)
            load(bounded_inspection, inspection_stop, cold_inspection);
        // Recovery retains bounded active metadata, one input and one event segment.
        // Terminal history is validated and then released, never copied into current.
        if (read_only)
            return;
        for (const auto& task : active_tasks()) {
            if (active(task.state())) {
                auto entry = find(task.id());
                auto next = summary(entry.task);
                next.set_state(wire::INTERRUPTED);
                next.set_error(
                    "task service restarted before confirmed completion; " "explicit retry required");
                commit(entry, next, "");
            }
        }
    }

    void load(bool bounded_inspection, std::stop_token inspection_stop, bool cold_inspection)
    {
        std::uint64_t inspection_bytes = 0, inspection_records = 0;
        const auto inspect_size = [&](std::int64_t bytes, std::int64_t maximum) {
            if (bytes <= 0 || bytes > maximum)
                throw std::invalid_argument("invalid task record size");
            if (read_only && bounded_inspection
                && ((inspection_bytes += bytes) > 256 * 1024 * 1024
                    || ++inspection_records > 100000))
                throw std::invalid_argument("task reference inspection limit exceeded");
        };
        for (const auto& item : fs::directory_iterator(root)) {
            safe(item.path());
            const auto name = item.path().filename().string();
            if ((name == "event-segments" || kinds->reserved(name)) && item.is_directory())
                continue;
            if (item.is_regular_file()
                && (name == "instance.json" || name == "manager.lock" || name == "tasks.sqlite"
                    || name == "tasks.sqlite-wal" || name == "tasks.sqlite-shm"
                    || name == "tasks.sqlite-journal"))
                continue;
            validate_id(name);
            if (!item.is_directory())
                throw std::invalid_argument("unknown task store entry");
            if (!contains(name) && !abandoned_preparation(name))
                ++uncommitted;
        }
        {
            sqlite::Database::Statement mixed(
                *database,
                "SELECT d.id FROM preparation_decisions d JOIN tasks t USING(id) LIMIT 1");
            if (mixed.step())
                throw std::invalid_argument("task is both admitted and abandoned");
        }
        event_log->read([&](const std::string& id, std::uint64_t, const std::string&) {
            if (inspection_stop.stop_requested())
                throw Error(ErrorCode::cancelled, "task cold inspection interrupted");
            if (!contains(id))
                throw std::invalid_argument("invalid persisted task event identity");
        });
        sqlite::Database::Statement tasks(
            *database,
            "SELECT id, CASE WHEN length(CAST(manifest AS BLOB)) <= 65536 THEN manifest " "END, length(CAST(manifest AS BLOB)), sequence FROM tasks ORDER BY sequence");
        while (tasks.step()) {
            if (inspection_stop.stop_requested())
                throw Error(ErrorCode::cancelled, "task cold inspection interrupted");
            const auto id = tasks.text(0);
            validate_id(id);
            if (!fs::is_directory(root / id))
                throw std::invalid_argument("indexed task directory is missing");
            safe(root / id / "results");
            inspect_size(tasks.integer(2), 65536);
            const auto manifest = parse_json(tasks.text(1), 65536);
            Entry entry;
            // Shared fields here; the remainder belongs to the domain.
            auto shared = Json::object(), extra = Json::object();
            for (const auto& [key, value] : manifest.items())
                (key == "type" || key == "input_sha256" || key == "input_bytes" || key == "id"
                            || key == "submission_sequence" || key == "submitted_at_ms"
                            || key == "derivation"
                        ? shared
                        : extra)[key] = value;
            auto required = shared;
            required.erase("derivation");
            require_fields(
                required,
                {"type",
                    "input_sha256",
                    "input_bytes",
                    "id",
                    "submission_sequence",
                    "submitted_at_ms"});
            if (manifest.at("id") != id)
                throw std::invalid_argument("unsupported task manifest");
            if (!manifest.at("submission_sequence").is_number_unsigned()
                || manifest.at("submission_sequence") < 1
                || manifest.at("submission_sequence") > std::numeric_limits<std::uint32_t>::max()
                || !manifest.at("submitted_at_ms").is_number_integer()
                || manifest.at("submitted_at_ms") <= 0
                || manifest.at("submitted_at_ms") > 9223372036854LL)
                throw std::invalid_argument("invalid task submission metadata");
            if (!manifest.at("input_bytes").is_number_unsigned() || manifest.at("input_bytes") < 1
                || manifest.at("input_bytes") > payload::max_input_bytes)
                throw std::invalid_argument("invalid task input size");
            entry.input_bytes = manifest.at("input_bytes").get<std::uint64_t>();
            entry.input_digest = manifest.at("input_sha256").get<std::string>();
            payload::validate_digest(entry.input_digest);
            inspect_size(static_cast<std::int64_t>(entry.input_bytes), payload::max_input_bytes);
            entry.task.set_submission_sequence(
                manifest.at("submission_sequence").get<std::uint32_t>());
            entry.task.set_submitted_at_ms(manifest.at("submitted_at_ms").get<std::int64_t>());
            entry.task.set_updated_at_ms(entry.task.submitted_at_ms());
            if (entry.task.submission_sequence() != ++last_sequence
                || tasks.integer(3) != last_sequence)
                throw std::invalid_argument("task submission sequence has missing records");
            entry.task.set_id(id);
            const auto* kind = manifest.at("type").is_string()
                ? kinds->manifest(manifest.at("type").get<std::string>())
                : nullptr;
            if (!kind)
                throw std::invalid_argument("unsupported task manifest type");
            entry.task.set_kind(kind->spec->kind);
            kinds->domain.read_manifest(extra, entry.task, identity.instance);
            // Opening checks index/event consistency. Only cold backup inspection,
            // reading files this process did not write, verifies payload digests.
            if (cold_inspection)
                payload::check(
                    root / id / "input.pb",
                    entry.input_digest,
                    payload::max_input_bytes,
                    entry.input_bytes);
            if (manifest.contains("derivation")) {
                auto metadata = read_metadata(Json{{"derivation", manifest.at("derivation")}});
                *entry.task.mutable_metadata() = std::move(metadata);
            }
            const auto indexed = find(id);
            admit_as(entry.task, indexed.task);
            entry.task.set_state(wire::QUEUED);
            event_log->read_task(
                id,
                [&](const std::string&, std::uint64_t sequence, const std::string& raw) {
                    if (sequence != ++entry.event_sequence)
                        throw std::invalid_argument("task state history has a gap");
                    inspect_size(static_cast<std::int64_t>(raw.size()), 65536);
                    auto event = parse_json(raw, 65536);
                    if (event.at("type") == "metadata") {
                        require_fields(event, {"type", "metadata"});
                        auto metadata = read_metadata(event.at("metadata"));
                        if (metadata.revision() != entry.task.metadata().revision() + 1
                            || metadata.derivation().SerializeAsString()
                                != entry.task.metadata().derivation().SerializeAsString())
                            throw std::invalid_argument("invalid experiment metadata revision");
                        *entry.task.mutable_metadata() = std::move(metadata);
                        return;
                    }
                    if (event.at("type") == "retirement") {
                        require_fields(event, {"type", "object", "updated_at_ms"});
                        if (!event.at("updated_at_ms").is_number_integer()
                            || event.at("updated_at_ms") <= 0
                            || event.at("updated_at_ms") > 9223372036854LL)
                            throw std::invalid_argument("invalid task retirement time");
                        entry.task.set_updated_at_ms(event.at("updated_at_ms").get<std::int64_t>());
                        if (entry.task.has_retirement() || occupies_slot(entry.task.state())
                            || !google::protobuf::util::JsonStringToMessage(
                                event.at("object").dump(),
                                entry.task.mutable_retirement())
                                .ok())
                            throw std::invalid_argument("invalid persisted task retirement");
                        const auto& object = entry.task.retirement();
                        const auto domain = kinds->domain.name();
                        const auto kind = kinds->spec(entry.task.kind()).resource;
                        protocol::validate_message(object);
                        validate_id(object.owner().node_id());
                        if (object.owner().service_id() != identity.instance
                            || object.owner().domain() != domain || object.id() != id
                            || object.version() != entry.input_digest || object.kind() != kind)
                            throw std::invalid_argument("task retirement identity mismatch");
                        return;
                    }
                    // Failure codes are optional observations. Service interruptions may have
                    // no code; do not guess one from diagnostic text.
                    const auto code = event.value("error_code", std::string());
                    if (!code.empty() && !parse_error_code(code))
                        throw std::invalid_argument("invalid task failure error code");
                    event.erase("error_code");
                    entry.task.set_error_code(code);
                    require_fields(
                        event,
                        {"type",
                            "state",
                            "attempt",
                            "token",
                            "completed",
                            "error",
                            "digest",
                            "updated_at_ms",
                            "publication",
                            "history_dataset_id"});
                    if (event.at("type") != "state")
                        throw std::invalid_argument("unsupported task event type");
                    const auto state = event.at("state").get<int>();
                    if (!wire::TaskState_IsValid(state) || state == 0
                        || !event.at("attempt").is_number_unsigned()
                        || !event.at("completed").is_number_unsigned())
                        throw std::invalid_argument("invalid persisted task state");
                    const auto completed = event.at("completed").get<unsigned>();
                    if (completed > entry.task.total())
                        throw std::invalid_argument("invalid persisted task progress");
                    if (!event.at("updated_at_ms").is_number_integer()
                        || event.at("updated_at_ms") <= 0
                        || event.at("updated_at_ms") > 9223372036854LL)
                        throw std::invalid_argument("invalid task update time");
                    const auto previous = summary(entry.task);
                    entry.task.set_updated_at_ms(event.at("updated_at_ms").get<std::int64_t>());
                    const auto old_token = entry.token;
                    entry.task.set_state(static_cast<wire::TaskState>(state));
                    entry.task.set_attempt(event.at("attempt").get<unsigned>());
                    entry.task.set_completed(completed);
                    entry.task.set_error(event.at("error").get<std::string>());
                    entry.task.set_result_digest(event.at("digest").get<std::string>());
                    entry.task.set_history_dataset_id(
                        event.at("history_dataset_id").get<std::string>());
                    entry.task.clear_publication();
                    if (!event.at("publication").is_null()
                        && !google::protobuf::util::JsonStringToMessage(
                            event.at("publication").dump(),
                            entry.task.mutable_publication())
                            .ok())
                        throw std::invalid_argument("invalid persisted publication decision");
                    entry.token = event.at("token").get<std::string>();
                    transition(*kinds, previous, old_token, entry.task, entry.token);
                });
            if (encode(entry) != encode(indexed))
                throw std::invalid_argument("task index does not match state history");
            if (entry.task.has_publication()
                && (entry.task.publication().identity().task_instance() != identity.instance
                    || entry.task.publication().identity().data_instance() != identity.instance))
                throw std::invalid_argument("task publication belongs to another service binding");
            if (cold_inspection
                && (entry.task.state() == wire::SUCCEEDED
                    || entry.task.state() == wire::PUBLISHING))
                check_result_digest(entry);
            if (occupies_slot(entry.task.state())) {
                if (current.size() == 1000)
                    throw std::invalid_argument("task active capacity exceeded");
                current.emplace(id, std::move(entry));
            }
        }
    }

    static wire::StoredTaskRecord stored(const Entry& entry)
    {
        wire::StoredTaskRecord record;
        *record.mutable_task() = entry.task;
        record.set_input_digest(entry.input_digest);
        record.set_input_bytes(entry.input_bytes);
        return record;
    }

    // The records a domain's admission rules consult.
    class View final : public Records
    {
    public:
        explicit View(const Impl& owner)
            : owner_(owner)
        {}

        wire::StoredTaskRecord find(const std::string& id) const override
        {
            return stored(owner_.find(id));
        }

        bool retired(const std::string& id) const override
        {
            return owner_.find(id).task.has_retirement() || owner_.retiring.contains(id);
        }

        std::optional<wire::StoredTaskRecord>
        same_input(wire::TaskKind kind, const std::string& digest) const override
        {
            sqlite::Database::Statement same(
                *owner_.database,
                "SELECT id FROM tasks WHERE kind=? AND json_extract(manifest,'$.input_sha256')=? LIMIT 1");
            same.bind(1, std::string(owner_.kinds->spec(kind).manifest));
            same.bind(2, digest);
            if (!same.step())
                return std::nullopt;
            return stored(owner_.find(same.text(0)));
        }

    private:
        const Impl& owner_;
    };

    static std::string encode(const Entry& entry)
    {
        wire::StoredTaskRecord record;
        *record.mutable_task() = entry.task;
        record.set_input_digest(entry.input_digest);
        record.set_input_bytes(entry.input_bytes);
        record.set_token(entry.token);
        record.set_event_sequence(entry.event_sequence);
        if (record.ByteSizeLong() > 65536)
            throw std::invalid_argument("task metadata exceeds limit");
        return record.SerializeAsString();
    }

    std::optional<resource::proto::Object> abandoned_preparation(const std::string& id) const
    {
        validate_task_id(*kinds, id);
        sqlite::Database::Statement query(
            *database,
            "SELECT input_digest,CASE WHEN length(resource)<=4096 THEN resource END FROM preparation_decisions WHERE id=?");
        query.bind(1, id);
        if (!query.step())
            return {};
        resource::proto::Object object;
        if (!object.ParseFromString(query.blob(1)))
            throw std::invalid_argument("invalid preparation release decision");
        protocol::validate_message(object);
        validate_id(object.owner().node_id());
        if (object.id() != id || object.owner().service_id() != identity.instance
            || object.version() != query.text(0) || object.owner().domain() != kinds->domain.name())
            throw std::invalid_argument("preparation release decision identity mismatch");
        payload::validate_digest(object.version());
        return object;
    }

    void persist_abandonment(const resource::proto::Object& object, const std::string& evidence)
    {
        sqlite::Database::Transaction transaction(*database);
        sqlite::Database::Statement accepted(*database, "SELECT 1 FROM tasks WHERE id=?");
        accepted.bind(1, object.id());
        if (accepted.step())
            throw Error(ErrorCode::conflict, "admitted task cannot be abandoned as a preparation");
        sqlite::Database::Statement insert(
            *database,
            "INSERT INTO preparation_decisions(id,input_digest,resource,evidence) VALUES(?,?,?,?)");
        insert.bind(1, object.id())
            .bind(2, object.version())
            .bind_blob(3, object.SerializeAsString())
            .bind_blob(4, evidence)
            .step();
        transaction.commit();
    }

    bool contains(const std::string& id) const
    {
        sqlite::Database::Statement query(*database, "SELECT 1 FROM tasks WHERE id=?");
        query.bind(1, id);
        return query.step();
    }

    std::uint32_t active_count() const
    {
        return static_cast<std::uint32_t>(current.size());
    }

    void admit() const
    {
        if (active_count() + (submitting ? 1 : 0) >= 1000)
            throw std::invalid_argument("task active capacity reached");
    }

    std::vector<wire::Task> active_tasks() const
    {
        std::vector<wire::Task> result;
        result.reserve(current.size());
        for (const auto& [id, entry] : current)
            result.push_back(entry.task);
        std::ranges::sort(result, {}, &wire::Task::submission_sequence);
        return result;
    }

    void writable() const
    {
        if (read_only)
            throw std::logic_error("task store is read only");
        if (failed)
            throw std::runtime_error("task store requires recovery");
    }

    Entry find(const std::string& id) const
    {
        auto entry = lookup(id);
        if (!entry)
            throw Error(ErrorCode::not_found, "unknown task");
        return std::move(*entry);
    }

    std::optional<Entry> lookup(const std::string& id) const
    {
        validate_id(id);
        if (const auto found = current.find(id); found != current.end())
            return found->second;
        sqlite::Database::Statement query(
            *database,
            "SELECT CASE WHEN length(record)<=65536 THEN record END, " "state, attempt, updated_at_ms, sequence FROM tasks WHERE id=?");
        query.bind(1, id);
        if (!query.step())
            return std::nullopt;
        wire::StoredTaskRecord stored;
        const auto raw = query.blob(0);
        if (raw.empty() || !stored.ParseFromString(raw))
            throw std::invalid_argument("invalid task metadata record");
        protocol::validate_message(stored);
        if (stored.task().id() != id || stored.task().state() != query.integer(1)
            || stored.task().attempt() != query.integer(2)
            || stored.task().updated_at_ms() != query.integer(3)
            || stored.task().submission_sequence() != query.integer(4))
            throw std::invalid_argument("task index does not match state history");
        return Entry{
            stored.task(),
            stored.input_digest(),
            stored.input_bytes(),
            stored.token(),
            stored.event_sequence()
        };
    }

    static ActiveEntries::node_type active_entry(const Entry& entry)
    {
        if (!occupies_slot(entry.task.state()))
            return {};
        ActiveEntries prepared;
        prepared.emplace(entry.task.id(), entry);
        return prepared.extract(prepared.begin());
    }

    PendingChange prepare_change(const Entry& entry, wire::Task next, const std::string& token)
    {
        writable();
        next.set_updated_at_ms(now_ms());
        transition(*kinds, entry.task, entry.token, next, token);
        PendingChange change;
        change.next = {
            std::move(next),
            entry.input_digest,
            entry.input_bytes,
            token,
            entry.event_sequence + 1
        };
        change.record = encode(change.next);
        change.active_entry = active_entry(change.next);
        const auto& task = change.next.task;
        Json publication = nullptr;
        if (task.has_publication()) {
            std::string json;
            if (!google::protobuf::util::MessageToJsonString(task.publication(), &json).ok())
                throw std::logic_error("cannot encode task publication decision");
            publication = parse_json(json);
        }
        auto event = Json{
            {"type", "state"},
            {"publication", publication},
            {"history_dataset_id", task.history_dataset_id()},
            {"updated_at_ms", task.updated_at_ms()},
            {"state", static_cast<int>(task.state())},
            {"attempt", task.attempt()},
            {"token", token},
            {"completed", task.completed()},
            {"error", task.error()},
            {"digest", task.result_digest()}
        };
        if (!task.error_code().empty())
            event["error_code"] = task.error_code();
        change.event = event.dump();
        return change;
    }

    // Persistence consumes an already decided and encoded transition. It neither
    // reads current state nor chooses cancellation, attempt or completion semantics.
    void persist(const PendingChange& change)
    {
        const auto& next = change.next.task;
        if (!change.manifest.empty()) {
            sqlite::Database::Transaction transaction(*database);
            sqlite::Database::Statement insert(
                *database,
                "INSERT INTO tasks VALUES(?, ?, ?, ?, ?, 0, ?, ?)");
            insert.bind(1, next.id())
                .bind(2, static_cast<std::int64_t>(next.submission_sequence()))
                .bind(3, change.kind)
                .bind(4, change.manifest)
                .bind(5, static_cast<std::int64_t>(next.state()))
                .bind(6, next.submitted_at_ms())
                .bind_blob(7, change.record)
                .step();
            transaction.commit();
            return;
        }
        event_log->rotate();
        sqlite::Database::Transaction transaction(*database);
        event_log->append(next.id(), change.next.event_sequence, change.event);
        sqlite::Database::Statement update(
            *database,
            "UPDATE tasks SET state=?, attempt=?, updated_at_ms=?, record=? WHERE id=?");
        update.bind(1, static_cast<std::int64_t>(next.state()))
            .bind(2, static_cast<std::int64_t>(next.attempt()))
            .bind(3, next.updated_at_ms())
            .bind_blob(4, change.record)
            .bind(5, next.id())
            .step();
        transaction.commit();
    }

    void publish_active(const std::string& id, ActiveEntries::node_type prepared)
    {
        current.erase(id);
        if (prepared)
            current.insert(std::move(prepared));
    }

    void commit(Entry& entry, wire::Task next, const std::string& token)
    {
        auto change = prepare_change(entry, std::move(next), token);
        try {
            persist(change);
        } catch (...) {
            failed = true;
            throw;
        }
        // The map node and result are allocated before persistence. Confirmation
        // only transfers ownership and cannot allocate after the durable barrier.
        publish_active(change.next.task.id(), std::move(change.active_entry));
        entry = std::move(change.next);
    }

    Change change(const Entry&, wire::Task, const std::string&);
    Change unchanged(Entry);

    Entry fenced(const std::string& id, const std::string& token)
    {
        writable();
        auto entry = find(id);
        if (token.empty() || token != entry.token || !active(entry.task.state()))
            throw std::invalid_argument("stale or inactive task attempt");
        return entry;
    }

    fs::path result_path(const Entry& entry) const
    {
        return root / entry.task.id() / "results" / (std::to_string(entry.task.attempt()) + ".pb");
    }

    void check_result_digest(const Entry& entry) const
    {
        payload::check(result_path(entry), entry.task.result_digest(), payload::max_result_bytes);
    }

    ResultRead prepare_read(const Entry& entry, bool include_input) const
    {
        return ResultRead(
            kinds,
            entry.task,
            root / entry.task.id(),
            entry.input_digest,
            entry.input_bytes,
            include_input);
    }
};

struct Store::Change::State
{
    Impl* owner;
    std::optional<Impl::PendingChange> pending;
    Impl::Entry unchanged;
    std::optional<resource::proto::Object> abandonment;
    std::string abandonment_evidence;
    bool written = false, confirmed = false;

    const Impl::Entry& result() const
    {
        return pending ? pending->next : unchanged;
    }
};

Store::Change::Change(std::unique_ptr<State> state)
    : state_(std::move(state))
{}

Store::Change::Change(Change&&) noexcept = default;
Store::Change& Store::Change::operator=(Change&&) noexcept = default;
Store::Change::~Change() = default;

bool Store::Change::needs_write() const
{
    return state_->pending.has_value() || state_->abandonment.has_value();
}

const wire::Task& Store::Change::task() const
{
    return state_->result().task;
}

const std::string& Store::Change::token() const
{
    return state_->result().token;
}

void Store::Change::persist()
{
    if (state_->written)
        throw std::logic_error("task change is already persisted");
    if (state_->pending)
        state_->owner->persist(*state_->pending);
    if (state_->abandonment)
        state_->owner->persist_abandonment(*state_->abandonment, state_->abandonment_evidence);
    state_->written = true;
}

Store::Change Store::Impl::change(const Entry& entry, wire::Task next, const std::string& token)
{
    auto state = std::make_unique<Change::State>();
    state->owner = this;
    state->pending.emplace(prepare_change(entry, std::move(next), token));
    return Change(std::move(state));
}

Store::Change Store::Impl::unchanged(Entry entry)
{
    auto state = std::make_unique<Change::State>();
    state->owner = this;
    state->unchanged = std::move(entry);
    return Change(std::move(state));
}

void Store::confirm(Change& change)
{
    auto& state = *change.state_;
    if (state.owner != impl_.get() || !state.written || state.confirmed)
        throw std::logic_error("task change is not awaiting confirmation");
    if (state.abandonment) {
        if (impl_->submitting != state.abandonment->id() || !impl_->uncommitted)
            throw std::logic_error("preparation abandonment has no owned pending reservation");
        --impl_->uncommitted;
        impl_->submitting.reset();
    }
    if (state.pending) {
        if (state.pending->next.task.has_retirement())
            impl_->retiring.erase(state.pending->next.task.id());
        impl_->publish_active(
            state.pending->next.task.id(),
            std::move(state.pending->active_entry));
        if (!state.pending->manifest.empty()) {
            impl_->last_sequence = state.pending->next.task.submission_sequence();
            impl_->submitting.reset();
            if (state.pending->resumed_submission)
                --impl_->uncommitted;
        }
    }
    state.confirmed = true;
}

void Store::persistence_failed()
{
    impl_->failed = true;
}

bool Store::is_active(const std::string& id) const
{
    return impl_->current.contains(id);
}

Store::Change Store::commit(Change change)
{
    try {
        change.persist();
    } catch (...) {
        persistence_failed();
        throw;
    }
    confirm(change);
    return change;
}

Store::Store(
    const TaskDomain& domain,
    fs::path directory,
    Identity identity,
    std::shared_ptr<const Clock> clock)
    : Store(domain, std::move(directory), std::move(identity), std::move(clock), false)
{}

Store::Store(
    const TaskDomain& domain,
    fs::path directory,
    Identity identity,
    std::shared_ptr<const Clock> clock,
    bool read_only,
    bool bounded_inspection,
    bool cold_inspection,
    std::stop_token stop)
    : impl_(
          std::make_unique<Impl>(
              std::make_shared<const Kinds>(domain),
              std::move(directory),
              std::move(identity),
              std::move(clock),
              read_only,
              bounded_inspection,
              std::unique_ptr<FileLock>{},
              stop,
              cold_inspection))
{}

Store::~Store() = default;

sqlite::Database& Store::database(std::string_view service, bool writer)
{
    if (impl_->identity.instance != service)
        throw Error(ErrorCode::conflict, "task store owner mismatch");
    if (writer && impl_->read_only)
        throw Error(ErrorCode::permission_denied, "task store access is read only");
    return *impl_->database;
}

Store::ResultRead::ResultRead(
    std::shared_ptr<const Kinds> kinds,
    wire::Task task,
    fs::path directory,
    std::string digest,
    std::uint64_t bytes,
    bool include_input)
    : kinds_(std::move(kinds)),
      task_(std::move(task)),
      directory_(std::move(directory)),
      input_digest_(std::move(digest)),
      input_bytes_(bytes),
      include_input_(include_input)
{}

void Store::ResultRead::verify()
{
    verified_ = false;
    // The owner validated this result against its input when the task finished.
    // An ordinary read parses the published bytes; it does not verify them again.
    const auto raw = payload::load(
        directory_ / "results" / (std::to_string(task_.attempt()) + ".pb"),
        payload::max_result_bytes);
    wire::TaskResponse result;
    auto task = include_input_ ? read_input(*kinds_, task_, directory_ / "input.pb", input_bytes_)
                               : wire::Task{};
    payload::parse(raw, mutable_result(*kinds_, result, task_.kind()));

    if (include_input_) {
        *result.mutable_result_task() = std::move(task);
        result.set_result_input_sha256(input_digest_);
    }
    response_ = std::move(result);
    verified_ = true;
}

Store::ResultRead Store::prepare_result(const std::string& id) const
{
    const auto entry = impl_->find(id);
    if (entry.task.state() != wire::SUCCEEDED)
        throw std::invalid_argument("task has no confirmed result");
    return impl_->prepare_read(entry, true);
}

wire::TaskResponse Store::confirm_result(ResultRead read) const
{
    if (!read.verified_)
        throw std::invalid_argument("task result has not been verified");
    auto entry = impl_->find(read.task_.id());
    if (read.directory_ != impl_->root / entry.task.id() || entry.task.state() != wire::SUCCEEDED
        || entry.task.attempt() != read.task_.attempt()
        || entry.task.result_digest() != read.task_.result_digest()
        || entry.input_digest != read.input_digest_)
        throw std::invalid_argument("task result changed during verification");
    if (entry.task.has_metadata())
        *read.response_.mutable_result_task()->mutable_metadata() = entry.task.metadata();
    return std::move(read.response_);
}

Store::Submission::Submission(Impl* owner, fs::path root, wire::Task task)
    : owner_(owner),
      root_(std::move(root)),
      task_(std::move(task))
{
    validate_task_id(*owner_->kinds, task_.id());
    prepare(*owner_->kinds, task_);
}

Store::Submission Store::submission(wire::Task task) const
{
    return Submission(impl_.get(), impl_->root, std::move(task));
}

const std::string& Store::instance() const
{
    return impl_->identity.instance;
}

void Store::Submission::set_reuse_digest(std::string digest)
{
    reuse_digest_ = std::move(digest);
}

void Store::Submission::amend(const std::function<void(wire::Task&)>& change)
{
    if (admitted_ || prepared_)
        throw std::logic_error("task submission identity is already admitted");
    const auto id = task_.id();
    const auto kind = task_.kind();
    const auto definition = task_.definition_case();
    change(task_);
    if (task_.id() != id || task_.kind() != kind || task_.definition_case() != definition)
        throw std::logic_error("task submission amendment changed its definition");
}

void Store::Submission::set_derivation(wire::TaskDerivation derivation)
{
    if (admitted_)
        throw std::logic_error("task submission already admitted");
    *task_.mutable_metadata()->mutable_derivation() = std::move(derivation);
}

Store::Change Store::edit_metadata(const wire::TaskMetadataEdit& edit)
{
    impl_->writable();
    const auto entry = impl_->find(edit.id());
    if (!impl_->kinds->spec(entry.task.kind()).calculation)
        throw std::invalid_argument("experiment metadata requires a calculation task");
    if (edit.expected_revision() != entry.task.metadata().revision())
        throw Error(ErrorCode::conflict, "experiment metadata changed; reload before saving");
    auto next = entry;
    auto* metadata = next.task.mutable_metadata();
    if (metadata->revision() == std::numeric_limits<std::uint32_t>::max())
        throw std::invalid_argument("experiment metadata revision exhausted");
    metadata->set_revision(metadata->revision() + 1);
    metadata->set_name(edit.name());
    metadata->set_note(edit.note());
    validate_metadata(*metadata);
    auto state = std::make_unique<Change::State>();
    state->owner = impl_.get();
    auto& change = state->pending.emplace();
    next.event_sequence++;
    change.next = std::move(next);
    change.record = Impl::encode(change.next);
    change.active_entry = Impl::active_entry(change.next);
    change.event =
        Json{{"type", "metadata"}, {"metadata", metadata_json(change.next.task.metadata())}}.dump();
    return Change(std::move(state));
}

wire::PreparedTaskSubmission Store::prepared_submission_record(const std::string& id) const
{
    validate_task_id(*impl_->kinds, id);
    const auto directory = impl_->root / id;
    safe(directory);
    const auto path = directory / "prepared.pb";
    safe(path);
    if (!fs::is_regular_file(path) || fs::file_size(path) > 4 * 1024 * 1024)
        throw std::invalid_argument("task has no supported preparation record");
    std::ifstream stream(path, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(stream), {}};
    wire::PreparedTaskSubmission value;
    if (stream.bad() || !value.ParseFromString(bytes))
        throw std::invalid_argument("invalid task preparation record");
    protocol::validate_message(value);
    const auto& record = value.record();
    if (value.instance_id() != impl_->identity.instance || record.task().id() != id
        || record.task().definition_case() != wire::Task::DEFINITION_NOT_SET
        || record.task().state() != wire::QUEUED || record.task().attempt()
        || !record.token().empty() || record.event_sequence() || !record.input_bytes()
        || record.input_bytes() > payload::max_input_bytes)
        throw std::invalid_argument("task preparation ownership or state mismatch");
    payload::validate_digest(record.input_digest());
    impl_->kinds->domain.validate_preparation(value, impl_->identity.instance);
    if (impl_->kinds->spec(record.task().kind()).calculation
        && value.execution().input_sha256() != record.input_digest())
        throw std::invalid_argument("task preparation input identity mismatch");
    return value;
}

Store::Submission Store::resume_submission(const std::string& id) const
{
    const auto value = prepared_submission_record(id);
    const auto& record = value.record();
    const auto& kinds = *impl_->kinds;
    auto task =
        read_input(kinds, record.task(), impl_->root / id / "input.pb", record.input_bytes());
    Submission input(impl_.get(), impl_->root, std::move(task));
    if (summary(input.task_).SerializeAsString() != record.task().SerializeAsString())
        throw std::invalid_argument("prepared input does not match admitted work");
    if (kinds.spec(input.task_.kind()).calculation
        && kinds.domain.execution(input.task_, record.input_digest()).SerializeAsString()
            != value.execution().SerializeAsString())
        throw std::invalid_argument("prepared execution does not match fixed input");
    input.digest_ = record.input_digest();
    input.bytes_ = record.input_bytes();
    input.prepared_ = true;
    input.resumed_ = true;
    payload::sync_immutable(impl_->root / id / "input.pb");
    return input;
}

std::optional<resource::proto::Object> Store::abandoned_preparation(const std::string& id) const
{
    return impl_->abandoned_preparation(id);
}

wire::PreparedTaskSubmission Store::abandoned_preparation_record(const std::string& id) const
{
    const auto object = abandoned_preparation(id);
    sqlite::Database::Statement row(
        *impl_->database,
        "SELECT CASE WHEN length(evidence)<=4194304 THEN evidence END FROM preparation_decisions WHERE id=?");
    row.bind(1, id);
    wire::PreparedTaskSubmission evidence;
    if (!object || !row.step() || !evidence.ParseFromString(row.blob(0)))
        throw std::invalid_argument("invalid abandoned preparation evidence");
    protocol::validate_message(evidence);
    const auto& record = evidence.record();
    if (evidence.instance_id() != impl_->identity.instance || record.task().id() != id
        || record.input_digest() != object->version()
        || record.task().definition_case() != wire::Task::DEFINITION_NOT_SET
        || record.task().state() != wire::QUEUED || record.task().attempt()
        || !record.token().empty() || record.event_sequence() || !record.input_bytes()
        || record.input_bytes() > payload::max_input_bytes
        || (impl_->kinds->spec(record.task().kind()).calculation
            && evidence.execution().input_sha256() != record.input_digest()))
        throw std::invalid_argument("abandoned preparation ownership or state mismatch");
    return evidence;
}

Store::Change Store::abandon_preparation(
    const wire::PreparedTaskSubmission& prepared,
    const resource::proto::Object& resource)
{
    impl_->writable();
    protocol::validate_message(prepared);
    protocol::validate_message(resource);
    const auto& record = prepared.record();
    const auto& id = record.task().id();
    const auto& kinds = *impl_->kinds;
    validate_task_id(kinds, id);
    validate_id(resource.owner().node_id());
    payload::validate_digest(resource.version());
    if (record.task().state() != wire::QUEUED || record.task().attempt()
        || record.task().definition_case() != wire::Task::DEFINITION_NOT_SET
        || !record.token().empty() || record.event_sequence() || !kinds.has(record.task().kind()))
        throw Error(ErrorCode::conflict, "invalid unadmitted preparation state");
    const auto& spec = kinds.spec(record.task().kind());
    if (spec.calculation && prepared.execution().input_sha256() != record.input_digest())
        throw Error(ErrorCode::conflict, "preparation input identity mismatch");
    const auto domain = kinds.domain.name();
    const auto kind = spec.resource;
    if (prepared.instance_id() != impl_->identity.instance || resource.owner().domain() != domain
        || resource.owner().service_id() != impl_->identity.instance || resource.id() != id
        || resource.version() != record.input_digest() || resource.kind() != kind)
        throw Error(ErrorCode::conflict, "preparation abandonment identity mismatch");
    if (impl_->contains(id))
        throw Error(ErrorCode::conflict, "admitted task cannot be abandoned as a preparation");
    if (const auto existing = impl_->abandoned_preparation(id)) {
        if (existing->SerializeAsString() != resource.SerializeAsString())
            throw Error(ErrorCode::conflict, "preparation already has another release decision");
        return impl_->unchanged(
            {record.task(), record.input_digest(), record.input_bytes(), "", 0});
    }
    if (impl_->submitting)
        throw Error(ErrorCode::resource_exhausted, "task preparation is already being decided");
    if (!impl_->uncommitted)
        throw Error(ErrorCode::conflict, "preparation has no pending owned directory");
    auto state = std::make_unique<Change::State>();
    state->owner = impl_.get();
    state->unchanged = {record.task(), record.input_digest(), record.input_bytes(), "", 0};
    state->abandonment = resource;
    state->abandonment_evidence = prepared.SerializeAsString();
    impl_->submitting = id;
    return Change(std::move(state));
}

void Store::admit_submission(Submission& input)
{
    impl_->writable();
    if (impl_->contains(input.task_.id()) && impl_->find(input.task_.id()).task.has_retirement())
        throw Error(ErrorCode::conflict, "retired task identity cannot be reused");
    if (impl_->abandoned_preparation(input.task_.id()))
        throw Error(ErrorCode::conflict, "preparation was durably abandoned");
    if (input.owner_ != impl_.get() || input.admitted_)
        throw std::logic_error("invalid task submission admission");
    if (input.task_.metadata().has_derivation()) {
        input.task_.mutable_metadata()->mutable_derivation()->set_source_instance(
            impl_->identity.instance);
        validate_metadata(input.task_.metadata());
    }
    const auto& id = input.task_.id();
    std::optional<wire::StoredTaskRecord> existing;
    if (impl_->contains(id))
        existing = Impl::stored(impl_->find(id));
    const Impl::View records(*impl_);
    Admission admission{
        input.task_,
        records,
        impl_->identity.instance,
        input.reuse_digest_,
        existing ? &*existing : nullptr,
        {}
    };
    impl_->kinds->domain.admit(admission);
    input.reusable_ = std::move(admission.reusable);
    if (existing) {
        if (input.prepared_
            && (input.digest_ != existing->input_digest() || input.bytes_ != existing->input_bytes()
                || input.task_.program_artifact() != existing->task().program_artifact()
                || input.task_.metadata().derivation().SerializeAsString()
                    != existing->task().metadata().derivation().SerializeAsString()))
            throw Error(ErrorCode::conflict, "prepared submission differs from admitted task");
        input.existing_ = std::move(existing);
    } else {
        if (impl_->submitting)
            throw Error(ErrorCode::resource_exhausted, "task submission is already being prepared");
        impl_->admit();
        if (impl_->last_sequence == std::numeric_limits<std::uint32_t>::max())
            throw std::invalid_argument("task submission sequence exhausted");
        input.task_.set_submission_sequence(impl_->last_sequence + 1);
        input.task_.set_submitted_at_ms(impl_->now_ms());
        input.task_.set_updated_at_ms(input.task_.submitted_at_ms());
        impl_->submitting = id;
    }
    input.admitted_ = true;
}

void Store::Submission::prepare_files()
{
    if (!admitted_ || prepared_)
        throw std::logic_error("task submission is not awaiting file preparation");
    const auto& kinds = *owner_->kinds;
    const auto directory = root_ / task_.id();
    if (existing_) {
        const auto current =
            read_input(kinds, existing_->task(), directory / "input.pb", existing_->input_bytes());
        if (current.kind() != task_.kind()
            || current.download_authorization() != task_.download_authorization()
            || current.provider_artifact() != task_.provider_artifact()
            || !kinds.domain.same_submission(current, task_)
            || current.metadata().derivation().SerializeAsString()
                != task_.metadata().derivation().SerializeAsString()
            || definition(kinds, current).SerializeAsString()
                != definition(kinds, task_).SerializeAsString())
            throw std::invalid_argument("task id already belongs to different input");
    } else {
        safe(directory);
        if (!fs::create_directory(directory))
            throw std::invalid_argument("task directory exists without committed submission");
        created_ = true;
        fs::create_directory(directory / "results");
        kinds.domain.capture(task_, directory);
        sync_directory(directory / "results");
        sync_directory(directory);
        sync_directory(root_);
        std::optional<payload::Reference> reference;
        if (reusable_)
            reference = payload::Reference{
                root_ / reusable_->task().id() / "input.pb",
                {reusable_->input_digest(), reusable_->input_bytes()}
            };
        const auto stored = payload::publish(
            directory / "input.pb",
            definition(kinds, task_),
            payload::max_input_bytes,
            [&](const payload::Identity& input) {
                wire::PreparedTaskSubmission preparation;
                preparation.set_instance_id(owner_->identity.instance);
                auto* record = preparation.mutable_record();
                *record->mutable_task() = summary(task_);
                record->set_input_digest(input.digest);
                record->set_input_bytes(input.size);
                if (kinds.spec(task_.kind()).calculation)
                    *preparation.mutable_execution() = kinds.domain.execution(task_, input.digest);
                replace_file_durably(directory / "prepared.pb", preparation.SerializeAsString());
            },
            reference ? &*reference : nullptr);
        digest_ = stored.digest;
        bytes_ = stored.size;
    }
    prepared_ = true;
}

wire::TaskRetirement Store::retirement(const std::string& id, const std::string& node_id) const
{
    validate_id(node_id);
    const auto entry = impl_->find(id);
    const auto domain = impl_->kinds->domain.name();
    const auto kind = impl_->kinds->spec(entry.task.kind()).resource;
    wire::TaskRetirement result;
    *result.mutable_task() = summary(entry.task);
    auto* object = result.mutable_object();
    object->mutable_owner()->set_node_id(node_id);
    object->mutable_owner()->set_service_id(impl_->identity.instance);
    object->mutable_owner()->set_domain(std::string(domain));
    object->set_kind(std::string(kind));
    object->set_id(id);
    object->set_version(entry.input_digest);
    if (entry.task.has_retirement()) {
        if (entry.task.retirement().SerializeAsString() != object->SerializeAsString())
            throw Error(ErrorCode::conflict, "task retirement belongs to another owner");
        result.set_state(wire::TaskRetirement::RETIRED);
        return result;
    }
    result.set_active(occupies_slot(entry.task.state()));
    result.set_admission_busy(impl_->submitting.has_value() || impl_->retiring.contains(id));
    sqlite::Database::Statement consumers(
        *impl_->database,
        "SELECT id FROM tasks WHERE json_extract(manifest,'$.derivation.sourceId')=? AND json_extract(manifest,'$.derivation.sourceInstance')=? ORDER BY sequence");
    consumers.bind(1, id).bind(2, impl_->identity.instance);
    while (consumers.step()) {
        const auto consumer = impl_->find(consumers.text(0)).task;
        if (!consumer.has_retirement())
            result.add_consumers(consumer.id());
        if (result.consumers_size() == 257)
            break;
    }
    result.set_state(
        result.active() || result.admission_busy() || result.consumers_size()
            ? wire::TaskRetirement::BLOCKED
            : wire::TaskRetirement::AVAILABLE);
    return result;
}

Store::Change Store::retire(const resource::proto::Object& expected)
{
    impl_->writable();
    const auto preview = retirement(expected.id(), expected.owner().node_id());
    if (preview.object().SerializeAsString() != expected.SerializeAsString())
        throw Error(ErrorCode::conflict, "task retirement object changed");
    if (preview.state() == wire::TaskRetirement::RETIRED)
        return impl_->unchanged(impl_->find(expected.id()));
    if (preview.state() != wire::TaskRetirement::AVAILABLE)
        throw Error(ErrorCode::conflict, "task retirement is blocked by activity or references");
    auto state = std::make_unique<Change::State>();
    state->owner = impl_.get();
    auto& change = state->pending.emplace();
    change.next = impl_->find(expected.id());
    *change.next.task.mutable_retirement() = expected;
    change.next.task.set_updated_at_ms(impl_->now_ms());
    ++change.next.event_sequence;
    change.record = Impl::encode(change.next);
    std::string object;
    if (!google::protobuf::util::MessageToJsonString(expected, &object).ok())
        throw std::logic_error("cannot encode task retirement");
    change.event = Json{
        {"type", "retirement"},
        {"object", parse_json(object)},
        {"updated_at_ms", change.next.task.updated_at_ms()}
    }.dump();
    impl_->retiring.insert(expected.id());
    return Change(std::move(state));
}

wire::TaskExecution Store::Submission::execution() const
{
    if (!prepared_)
        throw std::logic_error("task submission files are not prepared");
    return owner_->kinds->domain.execution(task_, existing_ ? existing_->input_digest() : digest_);
}

Store::Change Store::register_submission(Submission& input)
{
    impl_->writable();
    if (input.owner_ != impl_.get() || !input.prepared_)
        throw std::logic_error("task submission files have not been prepared");
    const auto& task = input.task_;
    if (input.existing_)
        return impl_->unchanged(impl_->find(task.id()));
    if (impl_->submitting != task.id())
        throw std::logic_error("task submission has no reserved capacity");
    impl_->kinds->domain.confirm_admission(task, Impl::View(*impl_));
    auto state = std::make_unique<Change::State>();
    state->owner = impl_.get();
    auto& change = state->pending.emplace();
    if (input.resumed_ && !impl_->uncommitted)
        throw std::logic_error("resumed submission has no pending directory");
    change.resumed_submission = input.resumed_;
    change.next = {summary(task), input.digest_, input.bytes_, "", 0};
    change.record = Impl::encode(change.next);
    change.active_entry = Impl::active_entry(change.next);
    change.kind = std::string(impl_->kinds->spec(task.kind()).manifest);
    auto manifest = Json{
        {"submission_sequence", task.submission_sequence()},
        {"submitted_at_ms", task.submitted_at_ms()},
        {"type", change.kind},
        {"input_sha256", input.digest_},
        {"input_bytes", input.bytes_},
        {"id", task.id()}
    };
    if (task.metadata().has_derivation())
        manifest["derivation"] = metadata_json(task.metadata()).at("derivation");
    impl_->kinds->domain.write_manifest(task, manifest);
    change.manifest = manifest.dump();
    return Change(std::move(state));
}

bool Store::abandon_submission(Submission& input)
{
    if (input.owner_ != impl_.get())
        throw std::logic_error("invalid task submission admission");
    if (input.existing_ || impl_->submitting != input.task_.id())
        return false;
    impl_->submitting.reset();
    if (input.created_) {
        ++impl_->uncommitted;
        impl_->failed = true;
    }
    return input.created_;
}

void Store::release_prepared_reservation(Submission& input)
{
    impl_->writable();
    if (input.owner_ != impl_.get() || !input.admitted_ || !input.created_ || !input.prepared_
        || input.existing_ || input.resumed_ || impl_->submitting != input.task_.id())
        throw std::logic_error(
            "only a complete unregistered preparation can release its reservation");
    ++impl_->uncommitted;
    impl_->submitting.reset();
    input.created_ = false;
    input.admitted_ = false;
}

bool Store::has_submission() const
{
    return impl_->submitting.has_value();
}

wire::Task Store::get(const std::string& id) const
{
    auto read = prepare_input(id);
    read.load();
    return confirm_input(std::move(read));
}

Store::InputRead::InputRead(
    std::shared_ptr<const Kinds> kinds,
    wire::Task task,
    fs::path path,
    std::string digest,
    std::uint64_t bytes)
    : kinds_(std::move(kinds)),
      task_(std::move(task)),
      path_(std::move(path)),
      digest_(std::move(digest)),
      bytes_(bytes)
{}

void Store::InputRead::load()
{
    loaded_ = false;
    task_ = read_input(*kinds_, summary(task_), path_, bytes_);
    loaded_ = true;
}

void Store::InputRead::load_for_claim()
{
    // Invalid fixed input fails the claimed attempt and removes it from the
    // queue. Durable bytes and the originally admitted progress remain intact.
    const auto admitted = task_.total();
    // This identity is admitted metadata even when the calculation bytes are invalid.
    const auto& spec = kinds_->spec(task_.kind());
    kinds_->domain.admitted_execution(task_, execution_);
    try {
        load();
        prepare(*kinds_, task_);
        if (task_.total() != admitted)
            throw std::invalid_argument("task input does not match admitted work");
        if (spec.calculation)
            execution_ = kinds_->domain.execution(task_, digest_);
    } catch (const std::exception& error) {
        refusal_ = error.what();
        loaded_ = true;
    }
    if (spec.execution_carries_input)
        task_.clear_definition(); // Release full evidence on this file worker.
}

wire::TaskAttempt Store::confirm_attempt(InputRead read) const
{
    validate_input(read);
    if (!read.refusal_.empty())
        throw std::invalid_argument(read.refusal_);
    wire::TaskAttempt result;
    if (read.execution_.parameters_case() != wire::TaskExecution::PARAMETERS_NOT_SET) {
        *result.mutable_task() = describe(read.task_.id());
        *result.mutable_execution() = std::move(read.execution_);
        if (!impl_->kinds->spec(read.task_.kind()).execution_carries_input)
            *result.mutable_task() = attach_definition(
                *impl_->kinds,
                std::move(*result.mutable_task()),
                std::move(read.task_));
    } else {
        *result.mutable_task() = confirm_input(std::move(read));
    }
    return result;
}

Store::InputRead Store::prepare_input(const std::string& id) const
{
    const auto entry = impl_->find(id);
    return InputRead(
        impl_->kinds,
        entry.task,
        impl_->root / id / "input.pb",
        entry.input_digest,
        entry.input_bytes);
}

void Store::validate_input(const InputRead& read) const
{
    if (!read.loaded_)
        throw std::invalid_argument("task input has not been loaded");
    const auto entry = impl_->find(read.task_.id());
    if (read.path_ != impl_->root / entry.task.id() / "input.pb"
        || read.digest_ != entry.input_digest || read.bytes_ != entry.input_bytes
        || read.task_.kind() != entry.task.kind())
        throw std::invalid_argument("task input changed during loading");
}

wire::Task Store::confirm_input(InputRead read) const
{
    validate_input(read);
    const auto entry = impl_->find(read.task_.id());
    auto task = entry.task;
    return attach_definition(*impl_->kinds, std::move(task), std::move(read.task_));
}

wire::Task Store::describe(const std::string& id) const
{
    return summary(impl_->find(id).task);
}

std::optional<wire::Task> Store::find_task(const std::string& id) const
{
    if (auto entry = impl_->lookup(id))
        return summary(entry->task);
    return std::nullopt;
}

Store::HistoryRead Store::prepare_history_usage(const std::string& dataset_id) const
{
    data::proto::HistoryUsage check;
    check.set_dataset_id(dataset_id);
    (void)protocol::decode_history_usage(check);
    return HistoryRead(impl_->kinds, impl_->root, dataset_id, impl_->last_sequence);
}

data::proto::HistoryUsage
Store::history_usage(const std::string& dataset_id, bool active_only) const
{
    auto read = prepare_history_usage(dataset_id);
    read.active_only_ = active_only;
    while (next_history_page(read))
        read.load_page();
    return read.take();
}

bool Store::next_history_page(HistoryRead& read) const
{
    read.page_.clear();
    sqlite::Database::Statement rows(
        *impl_->database,
        "SELECT id FROM tasks WHERE sequence>? AND sequence<=? ORDER BY sequence LIMIT 200");
    rows.bind(1, static_cast<std::int64_t>(read.after_))
        .bind(2, static_cast<std::int64_t>(read.through_));
    while (rows.step()) {
        const auto entry = impl_->find(rows.text(0));
        auto& record = read.page_.emplace_back();
        *record.mutable_task() = entry.task;
        record.set_input_digest(entry.input_digest);
        record.set_input_bytes(entry.input_bytes);
        read.after_ = entry.task.submission_sequence();
    }
    return !read.page_.empty();
}

void Store::HistoryRead::load_page()
{
    auto& result = result_;
    const auto& dataset_id = result.dataset_id();
    for (const auto& entry : page_) {
        if (active_only_ && entry.task().has_retirement())
            continue;
        if (!kinds_->spec(entry.task().kind()).references_history)
            continue; // No Data-owned versions; leave unrelated account facts unread.
        const auto& id = entry.task().id();
        const auto task =
            read_input(*kinds_, entry.task(), root_ / id / "input.pb", entry.input_bytes());
        kinds_->domain.history_references(task, dataset_id, result);
        if (result.references_size() > 10000)
            throw std::invalid_argument("historical usage exceeds reference limit");
    }
}

void Store::visit(
    const std::string& after_id,
    const std::function<bool(const wire::StoredTaskRecord&)>& visitor) const
{
    sqlite::Database::Statement rows(
        *impl_->database,
        "SELECT id FROM tasks WHERE id>? ORDER BY id");
    rows.bind(1, after_id);
    while (rows.step())
        if (!visitor(Impl::stored(impl_->find(rows.text(0)))))
            return;
}

std::vector<wire::Task> Store::active_tasks() const
{
    return impl_->active_tasks();
}

wire::TaskList Store::list(unsigned limit, unsigned before_sequence) const
{
    if (!limit || limit > 200)
        throw std::invalid_argument("task page limit must be 1..200");
    wire::TaskList result;
    auto* capacity = result.mutable_capacity();
    capacity->set_retained_tasks(impl_->last_sequence);
    capacity->set_active_used(impl_->active_count());
    capacity->set_active_reserved(impl_->submitting ? 1 : 0);
    capacity->set_uncommitted(impl_->uncommitted);
    capacity->set_active_limit(1000);
    sqlite::Database::Statement page(
        *impl_->database,
        "SELECT id FROM tasks WHERE sequence<?1 ORDER BY sequence DESC LIMIT ?2");
    const auto before = before_sequence ? static_cast<std::int64_t>(before_sequence)
                                        : static_cast<std::int64_t>(impl_->last_sequence) + 1;
    page.bind(1, before).bind(2, limit + 1);
    while (page.step()) {
        if (result.tasks_size() == static_cast<int>(limit)) {
            result.set_next_before_sequence(
                result.tasks(result.tasks_size() - 1).submission_sequence());
            break;
        }
        *result.add_tasks() = summary(impl_->find(page.text(0)).task);
    }
    std::reverse(result.mutable_tasks()->begin(), result.mutable_tasks()->end());
    for (auto& task : active_tasks())
        *result.add_active_tasks() = std::move(task);
    sqlite::Database::Statement counts(
        *impl_->database,
        "SELECT state, COUNT(*) FROM tasks WHERE state IN (5,7) GROUP BY state");
    while (counts.step()) {
        if (counts.integer(0) == wire::FAILED)
            result.set_failed_count(static_cast<std::uint32_t>(counts.integer(1)));
        else
            result.set_interrupted_count(static_cast<std::uint32_t>(counts.integer(1)));
    }
    return result;
}

Store::Change Store::fail_start(
    const std::string& id,
    unsigned attempt,
    const std::string& error,
    const std::string& code)
{
    auto entry = impl_->find(id);
    const auto state = entry.task.state();
    const bool before_claim = state == wire::QUEUED && entry.task.attempt() + 1 == attempt;
    if (!before_claim)
        return impl_->unchanged(std::move(entry));
    auto next = summary(entry.task);
    next.set_state(wire::FAILED);
    next.set_attempt(attempt);
    next.set_error(error);
    next.set_error_code(code);
    return impl_->change(entry, std::move(next), "");
}

Store::Change Store::claim(const std::string& id)
{
    auto read = prepare_input(id);
    read.load();
    return claim(read);
}

Store::Change Store::claim(const InputRead& read)
{
    validate_input(read);
    auto entry = impl_->find(read.task_.id());
    if (entry.task.state() != wire::QUEUED || entry.task.attempt() >= 100)
        throw std::invalid_argument("task is not queued or attempt limit reached");
    auto next = summary(entry.task);
    next.set_state(wire::RUNNING);
    next.set_attempt(next.attempt() + 1);
    next.set_completed(0);
    next.clear_error();
    next.clear_error_code();
    next.clear_result_digest();
    const auto token = unique_process_id();
    return impl_->change(entry, std::move(next), token);
}

Store::Change Store::progress(const std::string& id, const std::string& token, unsigned completed)
{
    auto entry = impl_->fenced(id, token);
    if (completed < entry.task.completed() || completed > entry.task.total())
        throw std::invalid_argument("invalid task progress");
    if (completed == entry.task.completed())
        return impl_->unchanged(std::move(entry));
    auto next = summary(entry.task);
    next.set_completed(completed);
    return impl_->change(entry, std::move(next), token);
}

Store::Change Store::cancel(const std::string& id)
{
    auto entry = impl_->find(id);
    auto next = summary(entry.task);
    if (next.state() == wire::QUEUED)
        next.set_state(wire::CANCELLED);
    else if (next.state() == wire::RUNNING)
        next.set_state(wire::CANCEL_REQUESTED);
    if (next.state() == entry.task.state())
        return impl_->unchanged(std::move(entry));
    return impl_->change(entry, std::move(next), entry.token);
}

Store::Change Store::retry(const std::string& id)
{
    auto entry = impl_->find(id);
    if (entry.task.state() != wire::FAILED && entry.task.state() != wire::INTERRUPTED
        && entry.task.state() != wire::CANCELLED)
        throw std::invalid_argument("task is not retryable");
    if (entry.task.attempt() >= 100)
        throw std::invalid_argument("task attempt limit reached");
    impl_->admit();
    auto next = summary(entry.task);
    next.set_state(wire::QUEUED);
    next.set_completed(0);
    next.clear_error();
    next.clear_error_code();
    next.clear_result_digest();
    return impl_->change(entry, std::move(next), "");
}

Store::Completion::Completion(
    std::shared_ptr<const Kinds> kinds,
    Identity identity,
    wire::Task task,
    wire::TaskFinish result,
    fs::path directory,
    std::string input_digest,
    std::uint64_t input_bytes)
    : kinds_(std::move(kinds)),
      task_(std::move(task)),
      directory_(std::move(directory)),
      input_digest_(std::move(input_digest)),
      input_bytes_(input_bytes),
      result_(std::move(result)),
      identity_(std::move(identity))
{}

void Store::Completion::prepare_payload()
{
    prepared_ = false;
    if (task_.state() != wire::CANCEL_REQUESTED) {
        task_ = read_input(*kinds_, std::move(task_), directory_ / "input.pb", input_bytes_);
        const auto* field = kinds_->at(task_.kind()).finish;
        if (!result_.GetReflection()->HasField(result_, field))
            throw std::invalid_argument("task result kind does not match its input");
        kinds_->domain.validate_result(task_, result_, identity_.instance);
        const auto& output = result_.GetReflection()->GetMessage(result_, field);
        // A prepared file is not a completion decision. Cancellation, interruption
        // or a newer attempt can win while this immutable snapshot is being written.
        // Retain such files as unconfirmed evidence, just as after a process crash.
        const auto path = directory_ / "results" / (std::to_string(task_.attempt()) + ".pb");
        result_digest_ = payload::publish(path, output, payload::max_result_bytes).digest;
    }
    prepared_ = true;
}

Store::Completion Store::prepare_finish(const wire::TaskFinish& result)
{
    auto entry = impl_->fenced(result.id(), result.token());
    if (result.output_case() == wire::TaskFinish::OUTPUT_NOT_SET)
        throw std::invalid_argument("missing task result");
    return Completion(
        impl_->kinds,
        impl_->identity,
        std::move(entry.task),
        result,
        impl_->root / result.id(),
        entry.input_digest,
        entry.input_bytes);
}

Store::Change Store::finish(Completion completion)
{
    if (!completion.prepared_)
        throw std::invalid_argument("task result payload has not been prepared");
    const auto& result = completion.result_;
    // Cancellation, interruption or retry may have happened
    // during verification. A verified result is not authority to bypass them.
    auto entry = impl_->fenced(result.id(), result.token());
    if (entry.task.state() == wire::CANCEL_REQUESTED) {
        return acknowledge_cancel(result.id(), result.token());
    }
    if (impl_->kinds->spec(entry.task.kind()).publishes)
        throw std::invalid_argument("download completion requires a prepared publication");
    auto next = summary(entry.task);
    next.set_state(wire::SUCCEEDED);
    next.set_completed(next.total());
    next.set_result_digest(completion.result_digest_);
    return impl_->change(entry, std::move(next), entry.token);
}

Store::Change Store::prepare_publication(
    Completion completion,
    const data::proto::DownloadIdentity& identity,
    std::string candidate_digest,
    std::string dataset_id)
{
    const auto& result = completion.result_;
    auto entry = impl_->fenced(result.id(), result.token());
    if (entry.task.state() == wire::CANCEL_REQUESTED) {
        return acknowledge_cancel(result.id(), result.token());
    }
    if (!completion.prepared_ || !impl_->kinds->spec(entry.task.kind()).publishes)
        throw std::invalid_argument("task completion has no verified publication candidate");
    auto next = summary(entry.task);
    next.set_state(wire::PUBLISHING);
    next.set_completed(next.total());
    next.set_history_dataset_id(std::move(dataset_id));
    auto* publication = next.mutable_publication();
    *publication->mutable_identity() = identity;
    publication->set_candidate_digest(std::move(candidate_digest));
    publication->set_publication_id(unique_process_id());
    // The immutable result is already durable. This small journal transition
    // alone authorizes the service to expose the prepared candidate.
    next.set_result_digest(completion.result_digest_);
    return impl_->change(entry, std::move(next), "");
}

std::vector<data::proto::DownloadPublication> Store::pending_publications() const
{
    std::vector<data::proto::DownloadPublication> result;
    for (const auto& task : active_tasks())
        if (task.state() == wire::PUBLISHING)
            result.push_back(task.publication());
    return result;
}

Store::Change Store::confirm_publication(const data::proto::DownloadPublication& decision)
{
    auto entry = impl_->find(decision.identity().task_id());
    if ((entry.task.state() != wire::PUBLISHING && entry.task.state() != wire::SUCCEEDED)
        || !entry.task.has_publication()
        || entry.task.publication().SerializeAsString() != decision.SerializeAsString())
        throw std::invalid_argument("published download does not match task decision");
    if (entry.task.state() == wire::SUCCEEDED)
        return impl_->unchanged(std::move(entry));
    auto next = summary(entry.task);
    next.set_state(wire::SUCCEEDED);
    return impl_->change(entry, std::move(next), "");
}

Store::Change Store::fail(
    const std::string& id,
    const std::string& token,
    const std::string& error,
    const std::string& error_code)
{
    if (!error_code.empty() && !parse_error_code(error_code))
        throw std::invalid_argument("invalid task failure error code");
    auto entry = impl_->fenced(id, token);
    if (error.empty() || error.size() > 1024)
        throw std::invalid_argument("invalid task error");
    auto next = summary(entry.task);
    next.set_state(entry.task.state() == wire::CANCEL_REQUESTED ? wire::CANCELLED : wire::FAILED);
    next.set_error(error);
    next.set_error_code(next.state() == wire::FAILED ? error_code : std::string());
    return impl_->change(entry, std::move(next), token);
}

Store::Change
Store::interrupt(const std::string& id, const std::string& token, const std::string& error)
{
    auto entry = impl_->fenced(id, token);
    if (error.empty() || error.size() > 1024)
        throw std::invalid_argument("invalid interruption reason");
    auto next = summary(entry.task);
    next.set_state(wire::INTERRUPTED);
    next.set_error(error);
    return impl_->change(entry, std::move(next), "");
}

Store::Change Store::acknowledge_cancel(const std::string& id, const std::string& token)
{
    auto entry = impl_->fenced(id, token);
    if (entry.task.state() != wire::CANCEL_REQUESTED)
        throw std::invalid_argument("cancellation was not requested");
    auto next = summary(entry.task);
    next.set_state(wire::CANCELLED);
    return impl_->change(entry, std::move(next), token);
}

} // namespace asterion::tasks

namespace asterion::tasks {

data::proto::HistoryUsage Store::inspect_history_usage(
    const TaskDomain& domain,
    const fs::path& directory,
    Identity identity,
    const std::string& id)
{
    Store store(domain, directory, std::move(identity), std::make_shared<SystemClock>(), true);
    return store.history_usage(id);
}

} // namespace asterion::tasks

namespace asterion::tasks {

Store::BackupCatalogue Store::inspect_backup(
    const TaskDomain& domain,
    const fs::path& directory,
    Identity identity,
    const BackupVisitor& admitted,
    const BackupVisitor& pending,
    const std::function<void(const wire::PreparedTaskSubmission&, const resource::proto::Object&)>&
        abandoned,
    std::stop_token stop)
{
    Store store(
        domain,
        directory,
        identity,
        std::make_shared<SystemClock>(),
        true,
        false,
        true,
        stop);
    const auto& kinds = *store.impl_->kinds;
    BackupCatalogue result;
    const auto add = [&](const fs::path& path) {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "task cold inspection interrupted");
        safe(directory / path);
        if (!fs::is_regular_file(directory / path))
            throw std::invalid_argument("task backup evidence is missing");
        result.files.push_back(path);
        if (result.files.size() > 100000)
            throw Error(ErrorCode::resource_exhausted, "task backup file count exceeds capacity");
    };
    add("instance.json");
    add("tasks.sqlite");
    add("manager.lock");
    result.directories.push_back("event-segments");
    for (const auto& segment : fs::directory_iterator(directory / "event-segments"))
        add(fs::path("event-segments") / segment.path().filename());
    const auto task_files = [&](const std::string& id,
                                const wire::StoredTaskRecord& record,
                                bool optional_input,
                                bool released = false) {
        result.directories.push_back(id);
        if (!optional_input || fs::exists(directory / id / "input.pb")) {
            payload::check(
                directory / id / "input.pb",
                record.input_digest(),
                payload::max_input_bytes,
                record.input_bytes());
            add(fs::path(id) / "input.pb");
        }
        if (fs::exists(directory / id / "prepared.pb")) {
            (void)store.prepared_submission_record(id);
            add(fs::path(id) / "prepared.pb");
        }
        for (const auto& captured : domain.captured(record, directory / id, released))
            add(fs::path(id) / captured);
        const auto outputs = directory / id / "results";
        safe(outputs);
        if (released && !fs::exists(outputs))
            return;
        if (!fs::is_directory(outputs))
            throw std::invalid_argument("task result directory is missing");
        result.directories.push_back(fs::path(id) / "results");
        for (const auto& file : fs::directory_iterator(outputs)) {
            const auto text = file.path().stem().string();
            unsigned attempt = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), attempt);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !attempt
                || attempt > record.task().attempt()
                || file.path().filename() != std::to_string(attempt) + ".pb")
                throw std::invalid_argument("unknown task result evidence");
            add(fs::path(id) / "results" / file.path().filename());
        }
    };
    sqlite::Database::Statement rows(
        *store.impl_->database,
        "SELECT id FROM tasks ORDER BY sequence");
    while (rows.step()) {
        const auto id = rows.text(0);
        const auto entry = store.impl_->find(id);
        wire::StoredTaskRecord record;
        if (!record.ParseFromString(Impl::encode(entry)))
            throw std::logic_error("task backup record encoding failed");
        task_files(id, record, false);
        if (admitted) {
            const auto fixed = read_verified_input(
                kinds,
                entry.task,
                directory / id / "input.pb",
                entry.input_digest,
                entry.input_bytes);
            admitted(
                record,
                kinds.spec(fixed.kind()).calculation ? domain.execution(fixed, entry.input_digest)
                                                     : wire::TaskExecution{});
        }
    }
    sqlite::Database::Statement decisions(
        *store.impl_->database,
        "SELECT id FROM preparation_decisions ORDER BY id");
    while (decisions.step()) {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "task cold inspection interrupted");
        const auto id = decisions.text(0);
        const auto object = store.abandoned_preparation(id);
        const auto evidence = store.abandoned_preparation_record(id);
        const auto& record = evidence.record();
        if (fs::exists(directory / id)) {
            if (fs::exists(directory / id / "prepared.pb")
                && store.prepared_submission_record(id).SerializeAsString()
                    != evidence.SerializeAsString())
                throw std::invalid_argument("abandoned preparation evidence mismatch");
            task_files(id, record, true, true);
        }
        if (abandoned)
            abandoned(evidence, *object);
    }
    for (const auto& item : fs::directory_iterator(directory)) {
        if (!item.is_directory() || item.path().filename() == "event-segments"
            || kinds.reserved(item.path().filename().string()))
            continue;
        const auto id = item.path().filename().string();
        if (store.impl_->contains(id))
            continue;
        if (store.abandoned_preparation(id)) {
            result.directories.push_back(id);
            continue;
        }
        const auto preparation = store.prepared_submission_record(id);
        task_files(id, preparation.record(), true);
        if (pending)
            pending(preparation.record(), preparation.execution());
    }
    std::ranges::sort(result.files);
    result.files.erase(std::unique(result.files.begin(), result.files.end()), result.files.end());
    std::ranges::sort(result.directories);
    result.directories.erase(
        std::unique(result.directories.begin(), result.directories.end()),
        result.directories.end());
    return result;
}

} // namespace asterion::tasks
