#include "risk_module.hpp"
#include "history_minutes.hpp"
#include <asterion/kernel/durable_file.hpp>
#include <fstream>
#include "task_store.hpp"
#include <asterion/protocol/task_execution.hpp>
#include <asterion/protocol/data.hpp>
#include "task_payload.hpp"
#include "sqlite_database.hpp"
#include "task_events.hpp"
#include <algorithm>
#include <asterion/foundation/decimal.hpp>
#include <asterion/foundation/error.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/file_lock.hpp>
#include <asterion/protocol/factor.hpp>
#include <map>
#include <limits>
#include <set>
#include <stdexcept>
#include <google/protobuf/util/json_util.h>

namespace asterion::tasks {
namespace wire = task::proto;
namespace fs = std::filesystem;

namespace {

data::proto::DownloadIdentity download_identity(const Identity& owner, const wire::Task& task)
{
    data::proto::DownloadIdentity result;
    result.set_data_instance(owner.data_instance);
    result.set_task_instance(owner.instance);
    result.set_task_id(task.id());
    result.set_attempt(task.attempt());
    return result;
}

void safe(const fs::path& path)
{
    if (fs::is_symlink(path))
        throw std::invalid_argument("task store rejects symbolic links");
}

// How a list names a dominant series: the product by the first and last month
// it is read from.
std::string series_name(const data::proto::BarDataset& first, const data::proto::BarDataset& last)
{
    const auto& contract = first.contract();
    const auto months = contract.symbol() == last.contract().symbol()
        ? contract.symbol()
        : contract.symbol() + "-" + last.contract().symbol();
    return contract.venue() + "/" + contract.product() + " (" + months + ")";
}

// What a list says a dataset was read from: its own revision, or for daily
// bars of the daily table the snapshot, which datasets read together name
// once.
void name_source(std::string& sources, const data::proto::BarDataset& dataset)
{
    const auto& name =
        dataset.source_dataset_ids().empty() ? dataset.daily().snapshot() : dataset.revision();
    if (!sources.ends_with(name))
        sources += (sources.empty() ? "" : " + ") + name;
}

void prepare(wire::Task& task)
{
    if (task.has_minutes()) {
        const auto range = history_files::minute_range(task.minutes());
        task.set_kind(wire::MINUTE_DOWNLOAD);
        task.set_source_name(task.minutes().source() + " " + task.minutes().contract_id());
        task.set_instrument(range.instrument.key());
        task.set_total(
            static_cast<unsigned>((range.end_ns - range.begin_ns) / 86400000000000LL + 1));
    } else if (task.has_table_update()) {
        const auto& update = task.table_update();
        protocol::validate_table_update(update);
        task.set_kind(wire::TABLE_UPDATE);
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
    } else if (task.has_factor()) {
        protocol::validate_factor_input(task.factor());
        task.set_kind(wire::FACTOR);
        std::string instruments, sources;
        for (const auto& series : task.factor().series()) {
            const auto separator = instruments.empty() ? "" : " + ";
            if (series.has_bars()) {
                const auto& c = series.bars().contract();
                instruments += separator + c.venue() + "/" + c.symbol();
                name_source(sources, series.bars());
            } else if (series.has_dominant()) {
                const auto& months = series.dominant().months();
                instruments += separator + series_name(*months.begin(), *months.rbegin());
                for (const auto& month : months)
                    name_source(sources, month);
            } else {
                // Every instrument of one snapshot names it once.
                instruments += separator + series.table().instrument();
                if (!sources.ends_with(series.table().snapshot()))
                    sources += separator + series.table().snapshot();
            }
        }
        task.set_instrument(instruments);
        task.set_source_name(sources);
        task.set_total(static_cast<unsigned>(protocol::factor_work_units(task.factor())));
        task.clear_trading_day();
    } else if (task.has_input()) {
        (void)protocol::decode_backtest(task.input(), protocol::DatasetView::metadata);
        task.set_kind(wire::BACKTEST);
        const auto& paper = task.input().paper();
        const auto dataset = [&](unsigned contract) -> const data::proto::BarDataset& {
            return paper.contracts(static_cast<int>(contract)).dataset();
        };
        // The series each contract is a month of, if any.
        std::vector<const data::proto::DominantSchedule*> series(
            static_cast<std::size_t>(paper.contracts_size()));
        for (const auto& schedule : task.input().series())
            for (const auto& roll : schedule.rolls())
                series[roll.contract()] = &schedule;
        // Contracts outside a series share their trading days; a series runs from
        // its first month's first day to its last month's last.
        std::string instruments, sources;
        auto first = dataset(0).days(0).trading_day(), last = first;
        for (unsigned c = 0; c < series.size(); ++c) {
            first = std::min(first, dataset(c).days(0).trading_day());
            last = std::max(last, dataset(c).days().rbegin()->trading_day());
            const auto separator = instruments.empty() ? "" : " + ";
            if (!series[c]) {
                const auto& contract = dataset(c).contract();
                instruments += separator + contract.venue() + "/" + contract.symbol();
                name_source(sources, dataset(c));
            } else if (series[c]->rolls(0).contract() == c) {
                // Named once, where its first month stands.
                instruments += separator
                    + series_name(dataset(c), dataset(series[c]->rolls().rbegin()->contract()));
                for (const auto& roll : series[c]->rolls())
                    name_source(sources, dataset(roll.contract()));
            }
        }
        task.set_instrument(instruments);
        task.set_source_name(sources);
        task.set_trading_day(first == last ? first : first + " / " + last);
        task.set_total(static_cast<unsigned>(protocol::backtest_work_units(task.input())));
    } else
        throw std::invalid_argument("task requires an explicit input type");
    task.set_data_source(
        task.has_minutes()            ? task.minutes().source()
            : task.has_table_update() ? task.table_update().provider()
                                      : std::string{});
    task.set_minute_interval_minutes(task.has_minutes() ? task.minutes().interval_minutes() : 0);
    task.set_state(wire::QUEUED);
}

const google::protobuf::Message& definition(const wire::Task& task)
{
    if (task.kind() == wire::MINUTE_DOWNLOAD && task.has_minutes())
        return task.minutes();
    if (task.kind() == wire::TABLE_UPDATE && task.has_table_update())
        return task.table_update();
    if (task.kind() == wire::FACTOR && task.has_factor())
        return task.factor();
    if (task.kind() == wire::BACKTEST && task.has_input())
        return task.input();
    throw std::invalid_argument("task kind and input disagree");
}

// Lists, dispatch and durable transitions need bounded metadata, never a copy
// of the immutable task dataset. Definitions are loaded from owned files on demand.
wire::Task summary(const wire::Task& task)
{
    wire::Task result;
    result.set_id(task.id());
    if (task.has_metadata())
        *result.mutable_metadata() = task.metadata();
    if (task.has_table_refresh())
        *result.mutable_table_refresh() = task.table_refresh();
    result.set_kind(task.kind());
    result.set_state(task.state());
    result.set_attempt(task.attempt());
    result.set_completed(task.completed());
    result.set_total(task.total());
    result.set_error(task.error());
    result.set_error_code(task.error_code());
    result.set_result_digest(task.result_digest());
    result.set_trading_day(task.trading_day());
    result.set_instrument(task.instrument());
    result.set_source_name(task.source_name());
    result.set_data_source(task.has_minutes() ? task.minutes().source() : task.data_source());
    result.set_history_dataset_id(task.history_dataset_id());
    if (task.has_publication())
        *result.mutable_publication() = task.publication();
    result.set_risk_artifact(task.risk_artifact());
    result.set_provider_artifact(task.provider_artifact());
    result.set_download_authorization(task.download_authorization());
    result.set_minute_interval_minutes(
        task.has_minutes() ? task.minutes().interval_minutes() : task.minute_interval_minutes());
    result.set_submission_sequence(task.submission_sequence());
    result.set_submitted_at_ms(task.submitted_at_ms());
    result.set_updated_at_ms(task.updated_at_ms());
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
    const wire::Task& before,
    const std::string& old_token,
    const wire::Task& after,
    const std::string& token)
{
    const auto a = before.state(), b = after.state();
    const bool start = a == wire::QUEUED && b == wire::RUNNING;
    const bool retry =
        (a == wire::FAILED || a == wire::INTERRUPTED || a == wire::CANCELLED) && b == wire::QUEUED;
    const bool running = a == wire::RUNNING
        && (b == wire::RUNNING || b == wire::CANCEL_REQUESTED || b == wire::SUCCEEDED
            || b == wire::PUBLISHING || b == wire::FAILED || b == wire::INTERRUPTED);
    const bool cancelling = a == wire::CANCEL_REQUESTED
        && (b == wire::CANCEL_REQUESTED || b == wire::CANCELLED || b == wire::INTERRUPTED);
    if (!(start || retry || running || cancelling || (a == wire::PUBLISHING && b == wire::SUCCEEDED)
            || (a == wire::QUEUED && b == wire::CANCELLED)))
        throw std::invalid_argument("invalid durable task transition");
    if (after.attempt() > 100 || after.attempt() != before.attempt() + (start ? 1U : 0U)
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
    const bool download = after.kind() == wire::MINUTE_DOWNLOAD;
    if (b == wire::PUBLISHING || (download && b == wire::SUCCEEDED)) {
        const auto& publication = after.publication();
        const auto& identity = publication.identity();
        validate_id(identity.data_instance());
        validate_id(identity.task_instance());
        validate_id(publication.publication_id());
        if (!download || identity.data_instance() == identity.task_instance()
            || identity.task_id() != after.id() || identity.attempt() != after.attempt()
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
wire::Task parse_input(wire::Task task, const std::string& raw)
{
    switch (task.kind()) {
        case wire::MINUTE_DOWNLOAD:
            payload::parse(raw, *task.mutable_minutes());
            break;
        case wire::TABLE_UPDATE:
            payload::parse(raw, *task.mutable_table_update());
            break;
        case wire::FACTOR:
            payload::parse(raw, *task.mutable_factor());
            break;
        case wire::BACKTEST:
            payload::parse(raw, *task.mutable_input());
            break;
        default:
            throw std::invalid_argument("unsupported task manifest type");
    }
    return task;
}

wire::Task
read_input(wire::Task task, const fs::path& path, const std::string& digest, std::uint64_t bytes)
{
    return parse_input(
        std::move(task),
        payload::read(path, digest, payload::max_input_bytes, bytes));
}

wire::Task attach_definition(wire::Task task, wire::Task source)
{
    switch (task.kind()) {
        case wire::MINUTE_DOWNLOAD:
            *task.mutable_minutes() = std::move(*source.mutable_minutes());
            break;
        case wire::TABLE_UPDATE:
            *task.mutable_table_update() = std::move(*source.mutable_table_update());
            break;
        case wire::FACTOR:
            *task.mutable_factor() = std::move(*source.mutable_factor());
            break;
        case wire::BACKTEST:
            *task.mutable_input() = std::move(*source.mutable_input());
            break;
        default:
            throw std::invalid_argument("unsupported task manifest type");
    }
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
    };

    fs::path root;
    Identity identity;
    std::unique_ptr<FileLock> owner;
    // Task index and state history: one row per task, one per state change.
    std::unique_ptr<sqlite::Database> database;
    std::unique_ptr<sqlite::TaskEvents> event_log;

    // A submission interrupted after creating its directory but before its index
    // commit leaves the directory; it is not loaded and its ID stays reserved.
    std::uint32_t uncommitted = 0;
    std::optional<std::string> submitting;
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

    explicit Impl(
        fs::path directory,
        Identity binding,
        std::shared_ptr<const Clock> source,
        bool inspect,
        bool bounded_inspection = true,
        std::unique_ptr<FileLock> ownership = {})
        : root(std::move(directory)),
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
        validate_id(identity.data_instance);
        const Json expected{
            {"task_instance", identity.instance},
            {"data_instance", identity.data_instance}
        };
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
            Impl inspected(root, identity, clock, true, false, std::move(owner));
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
            read_only ? sqlite::Database::Access::read_only : sqlite::Database::Access::writer);
        if (read_only)
            database->execute("BEGIN");
        else {
            sqlite::Database::Transaction schema(*database);
            database->execute(
                "CREATE TABLE IF NOT EXISTS tasks(id TEXT PRIMARY KEY," " sequence INTEGER NOT NULL UNIQUE, kind TEXT NOT NULL," " manifest TEXT NOT NULL, state INTEGER NOT NULL," " attempt INTEGER NOT NULL, updated_at_ms INTEGER NOT NULL, record BLOB " "NOT NULL) STRICT");
            database->execute("CREATE INDEX IF NOT EXISTS tasks_state ON tasks(state, sequence)");
            database->execute(
                "CREATE TABLE IF NOT EXISTS task_events(task_id TEXT NOT NULL" " REFERENCES tasks(id), sequence INTEGER NOT NULL, body TEXT NOT NULL," " PRIMARY KEY(task_id, sequence)) STRICT");
            schema.commit();
        }
        event_log =
            std::make_unique<sqlite::TaskEvents>(*database, root / "event-segments", read_only);
        if (read_only || !existing)
            load(bounded_inspection);
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

    void load(bool bounded_inspection)
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
            if (name == "event-segments" && item.is_directory())
                continue;
            if (item.is_regular_file()
                && (name == "instance.json" || name == "manager.lock" || name == "tasks.sqlite"
                    || name == "tasks.sqlite-wal" || name == "tasks.sqlite-shm"
                    || name == "tasks.sqlite-journal"))
                continue;
            validate_id(name);
            if (!item.is_directory())
                throw std::invalid_argument("unknown task store entry");
            if (!contains(name))
                ++uncommitted;
        }
        event_log->read([&](const std::string& id, std::uint64_t, const std::string&) {
            if (!contains(id))
                throw std::invalid_argument("invalid persisted task event identity");
        });
        sqlite::Database::Statement tasks(
            *database,
            "SELECT id, CASE WHEN length(CAST(manifest AS BLOB)) <= 65536 THEN manifest " "END, length(CAST(manifest AS BLOB)), sequence FROM tasks ORDER BY sequence");
        while (tasks.step()) {
            const auto id = tasks.text(0);
            validate_id(id);
            if (!fs::is_directory(root / id))
                throw std::invalid_argument("indexed task directory is missing");
            safe(root / id / "results");
            inspect_size(tasks.integer(2), 65536);
            const auto manifest = parse_json(tasks.text(1), 65536);
            Entry entry;
            auto manifest_fields = manifest;
            manifest_fields.erase("derivation");
            require_fields(
                manifest_fields,
                {"type",
                    "input_sha256",
                    "input_bytes",
                    "id",
                    "submission_sequence",
                    "submitted_at_ms",
                    "provider_artifact",
                    "risk_artifact",
                    "download_authorization"});
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
            if (manifest.at("type") == "minutes.task")
                entry.task.set_kind(wire::MINUTE_DOWNLOAD);
            else if (manifest.at("type") == "tables.task")
                entry.task.set_kind(wire::TABLE_UPDATE);
            else if (manifest.at("type") == "backtest.task")
                entry.task.set_kind(wire::BACKTEST);
            else if (manifest.at("type") == "factor.task")
                entry.task.set_kind(wire::FACTOR);
            else
                throw std::invalid_argument("unsupported task manifest type");
            entry.task.set_provider_artifact(manifest.at("provider_artifact").get<std::string>());
            entry.task.set_download_authorization(
                manifest.at("download_authorization").get<std::string>());
            if (entry.task.kind() == wire::MINUTE_DOWNLOAD
                || entry.task.kind() == wire::TABLE_UPDATE) {
                if (entry.task.download_authorization().size() != 64
                    || entry.task.download_authorization().find_first_not_of("0123456789abcdef")
                        != std::string::npos)
                    throw std::invalid_argument("invalid task download authorization");
                const auto& hash = entry.task.provider_artifact();
                if (hash.size() != 64 || !std::ranges::all_of(hash, [](char c) {
                        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                    }))
                    throw std::invalid_argument("invalid task provider artifact");
            } else if (!entry.task.provider_artifact().empty()
                || !entry.task.download_authorization().empty())
                throw std::invalid_argument("unexpected task provider artifact");
            entry.task.set_risk_artifact(manifest.at("risk_artifact").get<std::string>());
            if (entry.task.kind() == wire::BACKTEST) {
                const auto& hash = entry.task.risk_artifact();
                if (hash.size() != 64 || !std::ranges::all_of(hash, [](char c) {
                        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                    }))
                    throw std::invalid_argument("invalid risk plugin artifact");
            } else if (!entry.task.risk_artifact().empty())
                throw std::invalid_argument("unexpected task risk artifact");
            // Recovery checks durable bytes and index/event consistency. Claim and
            // result reads validate the fixed input against its admitted work.
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
                    transition(previous, old_token, entry.task, entry.token);
                });
            if (encode(entry) != encode(indexed))
                throw std::invalid_argument("task index does not match state history");
            if (entry.task.has_publication()
                && (entry.task.publication().identity().task_instance() != identity.instance
                    || entry.task.publication().identity().data_instance()
                        != identity.data_instance))
                throw std::invalid_argument("task publication belongs to another service binding");
            if (entry.task.state() == wire::SUCCEEDED || entry.task.state() == wire::PUBLISHING)
                check_result_digest(entry);
            if (occupies_slot(entry.task.state())) {
                if (current.size() == 1000)
                    throw std::invalid_argument("task active capacity exceeded");
                current.emplace(id, std::move(entry));
            }
        }
    }

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
        validate_id(id);
        if (const auto found = current.find(id); found != current.end())
            return found->second;
        sqlite::Database::Statement query(
            *database,
            "SELECT CASE WHEN length(record)<=65536 THEN record END, " "state, attempt, updated_at_ms, sequence FROM tasks WHERE id=?");
        query.bind(1, id);
        if (!query.step())
            throw std::out_of_range("task does not exist");
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
        return {
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
        transition(entry.task, entry.token, next, token);
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

    ResultRead prepare_read(const Entry& entry, bool deep, bool include_input) const
    {
        return ResultRead(
            identity,
            entry.task,
            root / entry.task.id(),
            entry.input_digest,
            entry.input_bytes,
            deep,
            include_input);
    }

    wire::TaskResponse read_result(const Entry& entry, bool deep = true) const
    {
        auto read = prepare_read(entry, deep, false);
        read.verify();
        return std::move(read.response_);
    }
};

struct Store::Change::State
{
    Impl* owner;
    std::optional<Impl::PendingChange> pending;
    Impl::Entry unchanged;
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
    return state_->pending.has_value();
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
    if (state.pending) {
        impl_->publish_active(
            state.pending->next.task.id(),
            std::move(state.pending->active_entry));
        if (!state.pending->manifest.empty()) {
            impl_->last_sequence = state.pending->next.task.submission_sequence();
            impl_->submitting.reset();
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

Store::Store(fs::path directory, Identity identity, std::shared_ptr<const Clock> clock)
    : Store(std::move(directory), std::move(identity), std::move(clock), false)
{}

Store::Store(
    fs::path directory,
    Identity identity,
    std::shared_ptr<const Clock> clock,
    bool read_only)
    : impl_(
          std::make_unique<Impl>(
              std::move(directory),
              std::move(identity),
              std::move(clock),
              read_only))
{}

Store::~Store() = default;

Store::ResultRead::ResultRead(
    Identity identity,
    wire::Task task,
    fs::path directory,
    std::string digest,
    std::uint64_t bytes,
    bool verify_content,
    bool include_input)
    : task_(std::move(task)),
      directory_(std::move(directory)),
      input_digest_(std::move(digest)),
      input_bytes_(bytes),
      identity_(std::move(identity)),
      verify_content_(verify_content),
      include_input_(include_input)
{}

void Store::ResultRead::verify()
{
    verified_ = false;
    // Parse the exact bytes whose digest was checked, never a second file read.
    const auto raw = payload::read(
        (directory_ / "results" / (std::to_string(task_.attempt()) + ".pb")),
        task_.result_digest(),
        payload::max_result_bytes);
    wire::TaskResponse result;
    const bool verify = verify_content_;
    auto task = verify || include_input_
        ? read_input(task_, directory_ / "input.pb", input_digest_, input_bytes_)
        : wire::Task{};
    if (!verify && !include_input_)
        payload::check(
            directory_ / "input.pb",
            input_digest_,
            payload::max_input_bytes,
            input_bytes_);
    if (task_.kind() == wire::MINUTE_DOWNLOAD) {
        payload::parse(raw, *result.mutable_minutes());

    } else if (task_.kind() == wire::TABLE_UPDATE) {
        payload::parse(raw, *result.mutable_table_update());
        if (verify)
            protocol::validate_table_update_result(
                task.table_update(),
                download_identity(identity_, task),
                result.table_update());

    } else if (task_.kind() == wire::FACTOR) {
        payload::parse(raw, *result.mutable_factor());
        if (verify)
            protocol::validate_factor_result(task.factor(), result.factor());
    } else {
        payload::parse(raw, *result.mutable_backtest());
        if (verify)
            protocol::validate_backtest_result(task.input(), result.backtest());
    }

    if (include_input_)
        *result.mutable_result_task() = std::move(task);
    response_ = std::move(result);
    verified_ = true;
}

Store::ResultRead Store::prepare_result(const std::string& id) const
{
    const auto entry = impl_->find(id);
    if (entry.task.state() != wire::SUCCEEDED)
        throw std::invalid_argument("task has no confirmed result");
    return impl_->prepare_read(entry, true, true);
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
    const auto& id = task_.id();
    validate_id(id);
    if (id.find_first_of("/\\") != std::string::npos || id == "." || id == ".."
        || id == "manager.lock")
        throw std::invalid_argument("invalid task id");
    if (id == "history" || id == "event-segments")
        throw std::invalid_argument("reserved task identifier");
    prepare(task_);
}

Store::Submission
Store::submission(const std::string& id, backtest::proto::BacktestInput input) const
{
    wire::Task task;
    task.set_id(id);
    *task.mutable_input() = std::move(input);
    task.set_risk_artifact(risk_providers::Module::selected().artifact());
    return Submission(impl_.get(), impl_->root, std::move(task));
}

Store::Submission Store::submission(const std::string& id, factor::proto::FactorInput input) const
{
    wire::Task task;
    task.set_id(id);
    *task.mutable_factor() = std::move(input);
    return Submission(impl_.get(), impl_->root, std::move(task));
}

Store::Submission Store::submission(const data::proto::DownloadAuthorization& authorization) const
{
    protocol::validate_message(authorization);
    validate_id(authorization.task_id());
    if (authorization.data_instance() != impl_->identity.data_instance
        || authorization.task_instance() != impl_->identity.instance
        || authorization.id().size() != 64
        || authorization.id().find_first_not_of("0123456789abcdef") != std::string::npos
        || authorization.provider_artifact().size() != 64
        || authorization.provider_artifact().find_first_not_of("0123456789abcdef")
            != std::string::npos)
        throw std::invalid_argument("invalid task download authorization");
    wire::Task task;
    task.set_id(authorization.task_id());
    task.set_download_authorization(authorization.id());
    task.set_provider_artifact(authorization.provider_artifact());
    if (authorization.has_minutes())
        *task.mutable_minutes() = authorization.minutes();
    else if (authorization.has_table_update())
        *task.mutable_table_update() = authorization.table_update();
    else
        throw std::invalid_argument("download definition is missing");
    return Submission(impl_.get(), impl_->root, std::move(task));
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
    if (entry.task.kind() != wire::BACKTEST && entry.task.kind() != wire::FACTOR)
        throw std::invalid_argument("experiment metadata requires a research task");
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

void Store::admit_submission(Submission& input)
{
    impl_->writable();
    if (input.owner_ != impl_.get() || input.admitted_)
        throw std::logic_error("invalid task submission admission");
    if (input.task_.metadata().has_derivation()) {
        auto* source = input.task_.mutable_metadata()->mutable_derivation();
        source->set_source_instance(impl_->identity.instance);
        validate_metadata(input.task_.metadata());
        const auto original = impl_->find(source->source_id()).task;
        if (source->source_id() == input.task_.id() || original.state() != wire::SUCCEEDED
            || original.kind() != input.task_.kind()
            || original.result_digest() != source->source_result_digest())
            throw std::invalid_argument("invalid experiment derivation");
    }
    const auto& id = input.task_.id();
    if (impl_->contains(id)) {
        const auto entry = impl_->find(id);
        input.existing_.emplace();
        *input.existing_->mutable_task() = entry.task;
        input.existing_->set_input_digest(entry.input_digest);
        input.existing_->set_input_bytes(entry.input_bytes);
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
    const auto directory = root_ / task_.id();
    if (existing_) {
        const auto current = read_input(
            existing_->task(),
            directory / "input.pb",
            existing_->input_digest(),
            existing_->input_bytes());
        if (current.kind() != task_.kind()
            || current.download_authorization() != task_.download_authorization()
            || current.provider_artifact() != task_.provider_artifact()
            || current.metadata().derivation().SerializeAsString()
                != task_.metadata().derivation().SerializeAsString()
            || definition(current).SerializeAsString() != definition(task_).SerializeAsString())
            throw std::invalid_argument("task id already belongs to different input");
    } else {
        safe(directory);
        if (!fs::create_directory(directory))
            throw std::invalid_argument("task directory exists without committed submission");
        created_ = true;
        fs::create_directory(directory / "results");
        if (task_.has_input()) {
            const auto module = risk_providers::Module::selected();
            if (module.artifact() != task_.risk_artifact())
                throw std::invalid_argument("risk plugin artifact changed before capture");
            module.capture(directory);
        }
        sync_directory(directory / "results");
        sync_directory(directory);
        sync_directory(root_);
        const auto stored =
            payload::publish(directory / "input.pb", definition(task_), payload::max_input_bytes);
        digest_ = stored.digest;
        bytes_ = stored.size;
    }
    prepared_ = true;
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
    auto state = std::make_unique<Change::State>();
    state->owner = impl_.get();
    auto& change = state->pending.emplace();
    change.next = {summary(task), input.digest_, input.bytes_, "", 0};
    change.record = Impl::encode(change.next);
    change.active_entry = Impl::active_entry(change.next);
    change.kind = task.kind() == wire::MINUTE_DOWNLOAD ? "minutes.task"
        : task.kind() == wire::TABLE_UPDATE            ? "tables.task"
        : task.kind() == wire::FACTOR                  ? "factor.task"
                                                       : "backtest.task";
    change.manifest = Json{
        {"provider_artifact", task.provider_artifact()},
        {"download_authorization", task.download_authorization()},
        {"risk_artifact", task.risk_artifact()},
        {"submission_sequence", task.submission_sequence()},
        {"submitted_at_ms", task.submitted_at_ms()},
        {"type", change.kind},
        {"input_sha256", input.digest_},
        {"input_bytes", input.bytes_},
        {"id", task.id()}
    }.dump();
    if (task.metadata().has_derivation()) {
        auto manifest = Json::parse(change.manifest);
        manifest["derivation"] = metadata_json(task.metadata()).at("derivation");
        change.manifest = manifest.dump();
    }
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

Store::InputRead::InputRead(wire::Task task, fs::path path, std::string digest, std::uint64_t bytes)
    : task_(std::move(task)),
      path_(std::move(path)),
      digest_(std::move(digest)),
      bytes_(bytes)
{}

void Store::InputRead::load()
{
    loaded_ = false;
    task_ = read_input(summary(task_), path_, digest_, bytes_);
    loaded_ = true;
}

void Store::InputRead::load_for_claim()
{
    // Invalid fixed input fails the claimed attempt and removes it from the
    // queue. Durable bytes and the originally admitted progress remain intact.
    const auto admitted = task_.total();
    try {
        load();
        prepare(task_);
        if (task_.total() != admitted)
            throw std::invalid_argument("task input does not match admitted work");
        if (task_.has_input() || task_.has_factor())
            execution_ = protocol::task_execution(task_, digest_);
    } catch (const std::exception& error) {
        refusal_ = error.what();
        loaded_ = true;
    }
    if (task_.has_input() || task_.has_factor())
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
    } else {
        *result.mutable_task() = confirm_input(std::move(read));
    }
    return result;
}

Store::InputRead Store::prepare_input(const std::string& id) const
{
    const auto entry = impl_->find(id);
    return InputRead(
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
    return attach_definition(std::move(task), std::move(read.task_));
}

wire::Task Store::describe(const std::string& id) const
{
    return summary(impl_->find(id).task);
}

Store::HistoryRead Store::prepare_history_usage(const std::string& dataset_id) const
{
    data::proto::HistoryUsage check;
    check.set_dataset_id(dataset_id);
    (void)protocol::decode_history_usage(check);
    return HistoryRead(impl_->root, dataset_id, impl_->last_sequence);
}

data::proto::HistoryUsage Store::history_usage(const std::string& dataset_id) const
{
    auto read = prepare_history_usage(dataset_id);
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
        const auto& id = entry.task().id();
        auto task = read_input(
            entry.task(),
            root_ / id / "input.pb",
            entry.input_digest(),
            entry.input_bytes());
        bool market = false;
        const auto inspect = [&](const data::proto::BarDataset& data) {
            market |= std::ranges::find(data.source_dataset_ids(), dataset_id)
                != data.source_dataset_ids().end();
        };
        auto kind = data::proto::HISTORY_REFERENCE_UNSPECIFIED;
        if (task.has_input()) {
            kind = data::proto::HISTORY_BACKTEST;
            for (const auto& contract : task.input().paper().contracts())
                inspect(contract.dataset());
        } else if (task.has_factor()) {
            kind = data::proto::HISTORY_FACTOR;
            for (const auto& series : task.factor().series()) {
                if (series.has_bars())
                    inspect(series.bars());
                for (const auto& month : series.dominant().months())
                    inspect(month);
            }
        }
        const bool output = task.history_dataset_id() == dataset_id && task.has_minutes();
        if (!market && !output)
            continue;
        auto* row = result.add_references();
        row->set_id(id);
        row->set_kind(output ? data::proto::HISTORY_DOWNLOAD : kind);
        if (market)
            row->add_roles(data::proto::HISTORY_MARKET);
        if (output)
            row->add_roles(data::proto::HISTORY_OUTPUT);
        if (result.references_size() > 10000)
            throw std::invalid_argument("historical usage exceeds reference limit");
    }
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

wire::TaskLaunches Store::dispatch(const wire::TaskDispatch& processes, bool data_available) const
{
    impl_->writable();
    protocol::validate_message(processes);
    if (!processes.has_launch_slots() || processes.launch_slots() > 2)
        throw std::invalid_argument("task dispatch requires an explicit allowance of 0..2 workers");
    std::set<std::string> running;
    for (const auto& id : processes.running()) {
        validate_id(id);
        if (!running.insert(id).second)
            throw std::invalid_argument("duplicate running task identity");
    }
    wire::TaskLaunches result;
    constexpr std::size_t concurrency = 2;
    if (running.size() >= concurrency)
        return result;
    const auto queue = active_tasks();
    for (const auto& task : queue) {
        if (static_cast<unsigned>(result.launches_size()) >= processes.launch_slots()
            || running.size() + static_cast<std::size_t>(result.launches_size()) >= concurrency)
            break;
        if (task.state() != wire::QUEUED || running.contains(task.id()) || !data_available)
            continue;
        auto* launch = result.add_launches();
        launch->set_task_id(task.id());
        switch (task.kind()) {
            case wire::BACKTEST:
                launch->set_program(wire::BACKTEST_PROGRAM);
                launch->set_risk_artifact(task.risk_artifact());
                break;
            case wire::FACTOR:
                launch->set_program(wire::FACTOR_PROGRAM);
                break;
            case wire::MINUTE_DOWNLOAD:
                launch->set_program(wire::DATA_PIPELINE_PROGRAM);
                launch->set_provider_artifact(task.provider_artifact());
                launch->set_minute_download(true);
                break;
            case wire::TABLE_UPDATE:
                launch->set_program(wire::DATA_PIPELINE_PROGRAM);
                launch->set_provider_artifact(task.provider_artifact());
                launch->set_table_update(true);
                break;
            default:
                throw std::invalid_argument("unsupported queued task kind");
        }
    }
    return result;
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
    Identity identity,
    wire::Task task,
    wire::TaskFinish result,
    fs::path directory,
    std::string input_digest,
    std::uint64_t input_bytes)
    : task_(std::move(task)),
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
        task_ = read_input(std::move(task_), directory_ / "input.pb", input_digest_, input_bytes_);
        if (result_.has_minutes() && task_.kind() == wire::MINUTE_DOWNLOAD && task_.has_minutes())
            protocol::validate_message(result_.minutes());
        else if (result_.has_table_update() && task_.kind() == wire::TABLE_UPDATE
            && task_.has_table_update())
            protocol::validate_table_update_result(
                task_.table_update(),
                download_identity(identity_, task_),
                result_.table_update());
        else if (result_.has_result() && task_.kind() == wire::BACKTEST && task_.has_input())
            protocol::validate_backtest_result(task_.input(), result_.result());
        else if (result_.has_factor() && task_.kind() == wire::FACTOR && task_.has_factor())
            protocol::validate_factor_result(task_.factor(), result_.factor());
        else
            throw std::invalid_argument("task result kind does not match its input");
        const google::protobuf::Message* output = nullptr;
        switch (result_.output_case()) {
            case wire::TaskFinish::kMinutes:
                output = &result_.minutes();
                break;
            case wire::TaskFinish::kTableUpdate:
                output = &result_.table_update();
                break;
            case wire::TaskFinish::kResult:
                output = &result_.result();
                break;
            case wire::TaskFinish::kFactor:
                output = &result_.factor();
                break;
            default:
                throw std::logic_error("missing verified task output");
        }
        // A prepared file is not a completion decision. Cancellation, lease expiry
        // or a newer attempt can win while this immutable snapshot is being written.
        // Retain such files as unconfirmed evidence, just as after a process crash.
        const auto path = directory_ / "results" / (std::to_string(task_.attempt()) + ".pb");
        result_digest_ = payload::publish(path, *output, payload::max_result_bytes).digest;
    }
    prepared_ = true;
}

Store::Completion Store::prepare_finish(const wire::TaskFinish& result)
{
    auto entry = impl_->fenced(result.id(), result.token());
    if (result.output_case() == wire::TaskFinish::OUTPUT_NOT_SET)
        throw std::invalid_argument("missing task result");
    return Completion(
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
    // Cancellation, interruption, retry and lease expiry may have happened
    // during verification. A verified result is not authority to bypass them.
    auto entry = impl_->fenced(result.id(), result.token());
    if (entry.task.state() == wire::CANCEL_REQUESTED) {
        return acknowledge_cancel(result.id(), result.token());
    }
    if (result.has_minutes())
        throw std::invalid_argument("download completion requires a prepared publication");
    auto next = summary(entry.task);
    next.set_state(wire::SUCCEEDED);
    next.set_completed(next.total());
    next.set_result_digest(completion.result_digest_);
    return impl_->change(entry, std::move(next), entry.token);
}

data::proto::DownloadPreparation Store::Completion::download_preparation() const
{
    if (!prepared_ || !result_.has_minutes())
        throw std::invalid_argument("download result has not been verified");
    data::proto::DownloadPreparation preparation;
    auto* identity = preparation.mutable_identity();
    identity->set_data_instance(identity_.data_instance);
    identity->set_task_instance(identity_.instance);
    identity->set_task_id(task_.id());
    identity->set_attempt(task_.attempt());
    auto* record = preparation.mutable_record();
    *record->mutable_minutes() = task_.minutes();
    *record->mutable_minute_result() = result_.minutes();
    return preparation;
}

Store::Change
Store::prepare_publication(Completion completion, const data::proto::PreparedDownload& prepared)
{
    const auto& result = completion.result_;
    auto entry = impl_->fenced(result.id(), result.token());
    if (entry.task.state() == wire::CANCEL_REQUESTED) {
        return acknowledge_cancel(result.id(), result.token());
    }
    protocol::validate_message(prepared);
    const auto preparation = completion.download_preparation();
    const auto& dataset = result.minutes().manifest_sha256();
    if (preparation.identity().SerializeAsString() != prepared.identity().SerializeAsString()
        || sha256_bytes(preparation.SerializeAsString()) != prepared.candidate_digest()
        || dataset != prepared.dataset_id())
        throw std::invalid_argument("prepared download does not match task completion");
    auto next = summary(entry.task);
    next.set_state(wire::PUBLISHING);
    next.set_completed(next.total());
    next.set_history_dataset_id(dataset);
    auto* publication = next.mutable_publication();
    *publication->mutable_identity() = prepared.identity();
    publication->set_candidate_digest(prepared.candidate_digest());
    publication->set_publication_id(unique_process_id());
    // The immutable result is already durable. This small journal transition
    // alone authorizes Data to expose the prepared candidate.
    next.set_result_digest(completion.result_digest_);
    return impl_->change(entry, std::move(next), "");
}

std::vector<data::proto::DownloadPublication> Store::pending_publications() const
{
    std::vector<data::proto::DownloadPublication> result;
    for (const auto& task : active_tasks())
        if (task.state() == wire::PUBLISHING && task.kind() == wire::MINUTE_DOWNLOAD)
            result.push_back(task.publication());
    return result;
}

Store::Change Store::confirm_publication(const data::proto::PublishedDownload& published)
{
    protocol::validate_message(published);
    auto entry = impl_->find(published.decision().identity().task_id());
    if ((entry.task.state() != wire::PUBLISHING && entry.task.state() != wire::SUCCEEDED)
        || !entry.task.has_publication()
        || entry.task.publication().SerializeAsString() != published.decision().SerializeAsString())
        throw std::invalid_argument("published download does not match task decision");
    data::proto::DownloadPreparation preparation;
    *preparation.mutable_identity() = published.decision().identity();
    *preparation.mutable_record() = published.record();
    if (sha256_bytes(preparation.SerializeAsString()) != published.decision().candidate_digest())
        throw std::invalid_argument("published download evidence changed");
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

data::proto::HistoryUsage
Store::inspect_history_usage(const fs::path& directory, Identity identity, const std::string& id)
{
    Store store(directory, std::move(identity), std::make_shared<SystemClock>(), true);
    return store.history_usage(id);
}

} // namespace asterion::tasks
