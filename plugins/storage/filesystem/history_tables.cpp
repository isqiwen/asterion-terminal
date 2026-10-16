#include "history_tables.hpp"
#include "sqlite_database.hpp"
#include "table_parquet.hpp"
#include <asterion/domain/history_identity.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <map>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace asterion::history_files {
namespace {
namespace fs = std::filesystem;
using Statement = sqlite::Database::Statement;
constexpr auto fields =
    "sequence, dataset, schema, source, exchange, period, content, object, " "rows, bytes, acquired_at_ns, provider_artifact, snapshot";

std::string snapshot_bytes(const TableRevision& next, const std::string& previous)
{
    return previous + "\n" + std::to_string(next.sequence) + "\t" + next.series.table + "\t"
        + std::to_string(next.series.schema) + "\t" + next.series.source + "\t" + next.exchange
        + "\t" + next.period + "\t" + next.content + "\t" + next.object + "\t"
        + std::to_string(next.rows) + "\t" + std::to_string(next.bytes) + "\t"
        + std::to_string(next.acquired_at_ns) + "\t" + next.provider_artifact;
}

std::string response_link(const SourceResponse& response)
{
    return "\traw-response-v1\t" + response.format + "\t" + std::to_string(response.received_at_ns)
        + "\t" + sha256_bytes(response.body);
}

std::string read_response_file(const fs::path& path, std::uintmax_t bound)
{
    if (fs::is_symlink(path) || !fs::is_regular_file(path) || fs::file_size(path) > bound)
        throw std::invalid_argument("source response evidence is missing or corrupt");
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::invalid_argument("source response evidence is missing or corrupt");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

// The newest entry of each partition: what the head snapshot holds.
constexpr auto newest =
    "sequence = (SELECT max(sequence) FROM table_revisions x WHERE x.dataset = r.dataset AND " "x.schema = r.schema AND x.source = r.source AND x.exchange = r.exchange AND " "x.period = r.period)";

bool digest(std::string_view value)
{
    return value.size() == 64
        && value.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}

// Validate an existing ledger before enabling writes. An empty file is new.
void supported(sqlite::Database& ledger)
{
    Statement tables(ledger, "SELECT name FROM sqlite_master WHERE type = 'table'");
    if (!tables.step())
        return;
    if (tables.text(0) != "table_revisions" || tables.step())
        throw std::invalid_argument("unsupported history table ledger");
    Statement columns(ledger, std::string("SELECT ") + fields + " FROM table_revisions LIMIT 0");
}

void validate(const TableSeries& series)
{
    if (history_table(series.table).schema != series.schema)
        throw std::invalid_argument("unsupported history table schema");
    validate_history_source(series.source);
}

TableRevision revision(const Statement& row)
{
    TableRevision result;
    result.sequence = static_cast<std::uint64_t>(row.integer(0));
    result.series = {row.text(1), static_cast<unsigned>(row.integer(2)), row.text(3)};
    result.exchange = row.text(4);
    result.period = row.text(5);
    result.content = row.text(6);
    result.object = row.text(7);
    result.rows = static_cast<std::uint64_t>(row.integer(8));
    result.bytes = static_cast<std::uint64_t>(row.integer(9));
    result.acquired_at_ns = row.integer(10);
    result.provider_artifact = row.text(11);
    result.snapshot = row.text(12);
    if (!result.sequence || !digest(result.content) || !digest(result.object)
        || !digest(result.snapshot))
        throw std::invalid_argument("invalid history table ledger entry");
    return result;
}

Statement&
bind_partition(Statement& statement, const TableSeries& series, const std::string& exchange)
{
    return statement.bind(1, series.table)
        .bind(2, static_cast<std::int64_t>(series.schema))
        .bind(3, series.source)
        .bind(4, exchange);
}

} // namespace

TableStore::TableStore(fs::path root)
    : root_(std::move(root))
{
    if (!root_.is_absolute())
        throw std::invalid_argument("history table store requires an absolute directory");
    create_directories_durably(root_ / "objects");
    ledger_ = std::make_unique<sqlite::Database>(
        root_ / "ledger.sqlite",
        sqlite::Database::Access::writer,
        supported);
    sqlite::Database::Transaction transaction(*ledger_);
    ledger_->execute(
        "CREATE TABLE IF NOT EXISTS table_revisions(sequence INTEGER PRIMARY KEY," " dataset TEXT NOT NULL, schema INTEGER NOT NULL, source TEXT NOT NULL," " exchange TEXT NOT NULL, period TEXT NOT NULL, content TEXT NOT NULL," " object TEXT NOT NULL, rows INTEGER NOT NULL, bytes INTEGER NOT NULL," " acquired_at_ns INTEGER NOT NULL, provider_artifact TEXT NOT NULL," " snapshot TEXT NOT NULL UNIQUE) STRICT");
    ledger_->execute(
        "CREATE INDEX IF NOT EXISTS table_revisions_partition ON " "table_revisions(dataset, schema, source, exchange, period, sequence)");
    transaction.commit();
}

TableStore::~TableStore() = default;

fs::path TableStore::object_path(const std::string& object) const
{
    if (!digest(object))
        throw std::invalid_argument("invalid history table object");
    return root_ / "objects" / object.substr(0, 2) / (object + ".parquet");
}

std::optional<TableRevision> TableStore::current(
    const TableSeries& series,
    const std::string& exchange,
    const std::string& period) const
{
    validate(series);
    std::lock_guard lock(mutex_);
    Statement statement(
        *ledger_,
        std::string("SELECT ") + fields
            + " FROM table_revisions WHERE dataset = ? AND schema = ? AND " "source = ? AND exchange = ? AND period = ? " "ORDER BY sequence DESC LIMIT 1");
    bind_partition(statement, series, exchange).bind(5, period);
    if (!statement.step())
        return std::nullopt;
    return revision(statement);
}

TableStore::Object TableStore::write(const TablePartition& partition) const
{
    if (!partition.table || partition.rows.empty() || !digest(partition.content))
        throw std::invalid_argument("history table partition has no rows");
    // Unique per partition and content; the owner orders writers of one partition.
    const auto incoming = root_ / "objects" / ("incoming-" + partition.content + ".parquet");
    parquet::write_table(incoming, partition);
    Object result{sha256_file(incoming), fs::file_size(incoming)};
    const auto path = object_path(result.sha256);
    create_directories_durably(path.parent_path());
    publish_file_durably(incoming, path);
    return result;
}

TableStore::Recorded TableStore::record(
    const TablePartition& partition,
    const TableSeries& series,
    const Object& object,
    const std::string& provider_artifact,
    std::int64_t acquired_at_ns,
    const std::optional<SourceResponse>& response)
{
    validate(series);
    if (!partition.table || partition.table->id != series.table || partition.rows.empty()
        || !digest(partition.content) || !digest(provider_artifact) || acquired_at_ns <= 0
        || fs::is_symlink(object_path(object.sha256))
        || !fs::is_regular_file(object_path(object.sha256))
        || fs::file_size(object_path(object.sha256)) != object.bytes)
        throw std::invalid_argument("invalid history table revision");
    std::lock_guard lock(mutex_);
    sqlite::Database::Transaction transaction(*ledger_);
    Statement held(
        *ledger_,
        std::string("SELECT ") + fields
            + " FROM table_revisions WHERE dataset = ? AND schema = ? AND " "source = ? AND exchange = ? AND period = ? " "ORDER BY sequence DESC LIMIT 1");
    bind_partition(held, series, partition.exchange).bind(5, partition.period);
    if (held.step()) {
        auto existing = revision(held);
        if (existing.content == partition.content)
            return {std::move(existing), true};
    }
    Statement last(
        *ledger_,
        "SELECT sequence, snapshot FROM table_revisions " "ORDER BY sequence DESC LIMIT 1");
    TableRevision next;
    std::string previous;
    if (last.step()) {
        next.sequence = static_cast<std::uint64_t>(last.integer(0));
        previous = last.text(1);
    }
    ++next.sequence;
    next.series = series;
    next.exchange = partition.exchange;
    next.period = partition.period;
    next.content = partition.content;
    next.object = object.sha256;
    next.rows = partition.rows.size();
    next.bytes = object.bytes;
    next.acquired_at_ns = acquired_at_ns;
    next.provider_artifact = provider_artifact;
    // No field can contain a tab: identifiers, digests and numbers only.
    next.snapshot = sha256_bytes(
        snapshot_bytes(next, previous) + (response ? response_link(*response) : std::string{}));
    if (response) {
        if (response->body.empty() || response->body.size() > max_source_response_bytes
            || response->received_at_ns <= 0 || !valid_source_response_format(response->format))
            throw std::invalid_argument("invalid source response evidence");
        const auto hash = sha256_bytes(response->body);
        const auto object_path = root_ / "responses" / "objects" / (hash + ".json");
        const auto reference_path = root_ / "responses" / "revisions" / (next.snapshot + ".json");
        create_directories_durably(object_path.parent_path());
        create_directories_durably(reference_path.parent_path());
        if (fs::exists(object_path)) {
            if (read_response_file(object_path, max_source_response_bytes) != response->body)
                throw std::invalid_argument("source response evidence is missing or corrupt");
        } else
            replace_file_durably(object_path, response->body);
        const nlohmann::json reference = {
            {"kind", "raw_response_body"},
            {"format", response->format},
            {"received_at_ns", std::to_string(response->received_at_ns)},
            {"sha256", hash},
            {"bytes", response->body.size()},
            {"revision_snapshot", next.snapshot},
            {"normalized_content", next.content},
            {"normalized_object", next.object},
            {"provider_artifact", next.provider_artifact}
        };
        replace_file_durably(reference_path, reference.dump(2) + "\n");
    }
    Statement insert(
        *ledger_,
        std::string("INSERT INTO table_revisions(") + fields
            + ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    insert.bind(1, static_cast<std::int64_t>(next.sequence))
        .bind(2, series.table)
        .bind(3, static_cast<std::int64_t>(series.schema))
        .bind(4, series.source)
        .bind(5, next.exchange)
        .bind(6, next.period)
        .bind(7, next.content)
        .bind(8, next.object)
        .bind(9, static_cast<std::int64_t>(next.rows))
        .bind(10, static_cast<std::int64_t>(next.bytes))
        .bind(11, next.acquired_at_ns)
        .bind(12, next.provider_artifact)
        .bind(13, next.snapshot);
    insert.step();
    transaction.commit();
    return {std::move(next), false};
}

std::optional<SourceResponse> TableStore::response(const TableRevision& revision) const
{
    if (!digest(revision.snapshot))
        throw std::invalid_argument("invalid source response evidence");
    std::lock_guard lock(mutex_);
    Statement older(
        *ledger_,
        "SELECT snapshot FROM table_revisions WHERE sequence < ? ORDER BY sequence DESC LIMIT 1");
    older.bind(1, static_cast<std::int64_t>(revision.sequence));
    std::string previous;
    if (older.step())
        previous = older.text(0);
    const auto path = root_ / "responses" / "revisions" / (revision.snapshot + ".json");
    if (!fs::exists(path)) {
        if (sha256_bytes(snapshot_bytes(revision, previous)) != revision.snapshot)
            throw std::invalid_argument("source response evidence is missing or corrupt");
        return {};
    }
    try {
        const auto reference = nlohmann::json::parse(read_response_file(path, 8192));
        const auto hash = reference.at("sha256").get<std::string>();
        if (!digest(hash) || reference.at("kind") != "raw_response_body"
            || reference.at("revision_snapshot") != revision.snapshot
            || reference.at("normalized_content") != revision.content
            || reference.at("normalized_object") != revision.object
            || reference.at("provider_artifact") != revision.provider_artifact)
            throw std::invalid_argument("source response evidence is missing or corrupt");
        SourceResponse response{
            reference.at("format").get<std::string>(),
            read_response_file(
                root_ / "responses" / "objects" / (hash + ".json"),
                max_source_response_bytes),
            std::stoll(reference.at("received_at_ns").get<std::string>())
        };
        if (!valid_source_response_format(response.format) || response.received_at_ns <= 0
            || reference.at("bytes") != response.body.size() || hash != sha256_bytes(response.body)
            || sha256_bytes(snapshot_bytes(revision, previous) + response_link(response))
                != revision.snapshot)
            throw std::invalid_argument("source response evidence is missing or corrupt");
        return response;
    } catch (...) {
        throw std::invalid_argument("source response evidence is missing or corrupt");
    }
}

std::optional<TableRevision> TableStore::revision_unlocked(const std::string& snapshot) const
{
    Statement statement(
        *ledger_,
        std::string("SELECT ") + fields + " FROM table_revisions WHERE snapshot = ?");
    statement.bind(1, snapshot);
    if (!statement.step())
        return {};
    return revision(statement);
}

std::string TableStore::head() const
{
    std::lock_guard lock(mutex_);
    Statement last(*ledger_, "SELECT snapshot FROM table_revisions ORDER BY sequence DESC LIMIT 1");
    return last.step() ? last.text(0) : std::string();
}

std::vector<TableCoverage> TableStore::coverage() const
{
    std::lock_guard lock(mutex_);
    Statement statement(
        *ledger_,
        std::string(
            "SELECT dataset, schema, source, exchange, count(*), " "min(period), max(period), sum(rows), max(acquired_at_ns) " "FROM table_revisions r WHERE ")
            + newest
            + " GROUP BY dataset, schema, source, exchange " "ORDER BY dataset, schema, source, exchange");
    std::vector<TableCoverage> result;
    while (statement.step())
        result.push_back(
            {{statement.text(0), static_cast<unsigned>(statement.integer(1)), statement.text(2)},
                statement.text(3),
                statement.text(5),
                statement.text(6),
                static_cast<std::uint64_t>(statement.integer(4)),
                static_cast<std::uint64_t>(statement.integer(7)),
                statement.integer(8)});
    return result;
}

std::vector<std::string> TableStore::periods(
    const TableSeries& series,
    const std::string& exchange,
    const std::string& first,
    const std::string& last) const
{
    validate(series);
    std::lock_guard lock(mutex_);
    Statement statement(
        *ledger_,
        "SELECT DISTINCT period FROM table_revisions WHERE dataset = ? " "AND schema = ? AND source = ? AND exchange = ? AND " "period BETWEEN ? AND ? ORDER BY period");
    bind_partition(statement, series, exchange).bind(5, first).bind(6, last);
    std::vector<std::string> result;
    while (statement.step())
        result.push_back(statement.text(0));
    return result;
}

std::optional<std::uint64_t> TableStore::sequence(const std::string& snapshot) const
{
    if (!digest(snapshot))
        throw std::invalid_argument("invalid history table snapshot");
    std::lock_guard lock(mutex_);
    Statement statement(*ledger_, "SELECT sequence FROM table_revisions WHERE snapshot = ?");
    statement.bind(1, snapshot);
    if (!statement.step())
        return std::nullopt;
    return static_cast<std::uint64_t>(statement.integer(0));
}

std::vector<TableRevision> TableStore::revisions(
    const TableSeries& series,
    const std::string& exchange,
    const std::string& first,
    const std::string& last,
    std::uint64_t until) const
{
    validate(series);
    std::lock_guard lock(mutex_);
    Statement statement(
        *ledger_,
        std::string("SELECT ") + fields
            + " FROM table_revisions r WHERE dataset = ? AND schema = ? AND " "source = ? AND exchange = ? AND period BETWEEN ? AND ? AND sequence = " "(SELECT max(sequence) FROM table_revisions x WHERE " "x.dataset = r.dataset AND x.schema = r.schema AND " "x.source = r.source AND x.exchange = r.exchange AND " "x.period = r.period AND x.sequence <= ?) ORDER BY period");
    bind_partition(statement, series, exchange)
        .bind(5, first)
        .bind(6, last)
        .bind(7, static_cast<std::int64_t>(until));
    std::vector<TableRevision> result;
    while (statement.step())
        result.push_back(revision(statement));
    return result;
}

std::vector<std::vector<TableCell>> TableStore::rows(const TableRevision& revision) const
{
    validate(revision.series);
    const auto path = object_path(revision.object);
    if (fs::is_symlink(path) || !fs::is_regular_file(path) || sha256_file(path) != revision.object)
        throw std::invalid_argument("history table object digest mismatch");
    auto rows = parquet::read_table(path, history_table(revision.series.table));
    if (rows.size() != revision.rows)
        throw std::invalid_argument("history table object digest mismatch");
    return rows;
}

void TableStore::scan(
    std::span<const TableRevision> revisions,
    std::span<const std::size_t> columns,
    std::span<const std::string> prefixes,
    const std::function<void(std::size_t, std::vector<TableCell>&)>& visit) const
{
    if (revisions.empty())
        return;
    validate(revisions.front().series);
    // Partitions that held the same rows share one object: it is read once, and
    // its rows are those of every revision that holds it.
    std::vector<fs::path> files;
    std::vector<std::vector<std::size_t>> holders;
    std::map<std::string, std::size_t> objects;
    for (std::size_t i = 0; i < revisions.size(); ++i) {
        const auto& revision = revisions[i];
        if (revision.series != revisions.front().series)
            throw std::invalid_argument("history table scan reads one series");
        const auto [object, first] = objects.emplace(revision.object, files.size());
        if (first) {
            auto path = object_path(revision.object);
            if (fs::is_symlink(path) || !fs::is_regular_file(path)
                || sha256_file(path) != revision.object)
                throw std::invalid_argument("history table object digest mismatch");
            files.push_back(std::move(path));
            holders.emplace_back();
        }
        holders[object->second].push_back(i);
    }
    parquet::scan_tables(
        files,
        history_table(revisions.front().series.table),
        columns,
        prefixes,
        [&](std::size_t file, std::vector<TableCell>& cells) {
            // A visitor may take the cells, so each but the last has its own.
            const auto& held = holders[file];
            for (std::size_t i = 0; i + 1 < held.size(); ++i) {
                auto copy = cells;
                visit(held[i], copy);
            }
            visit(held.back(), cells);
        });
}

} // namespace asterion::history_files
