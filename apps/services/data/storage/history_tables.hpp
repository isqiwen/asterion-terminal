#pragma once
#include <asterion/domain/history_tables.hpp>
#include <asterion/domain/source_response.hpp>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>

namespace asterion::sqlite {

class Database;

} // namespace asterion::sqlite

namespace asterion::history_files {

// One table from one source under one column definition. Another source or
// another definition of the same table is another series: they never mix.
struct TableSeries
{
    std::string table;
    unsigned schema = 0;
    std::string source; // the source's normalization identity
    auto operator<=>(const TableSeries&) const = default;
};

// One entry of the ledger: the content a partition held from this sequence on.
struct TableRevision
{
    std::uint64_t sequence = 0;
    TableSeries series;
    std::string exchange, period;
    std::string content, object; // SHA-256 of the typed rows, and of the file
    std::uint64_t rows = 0, bytes = 0;
    std::int64_t acquired_at_ns = 0;
    std::string provider_artifact;
    std::string snapshot; // identifies the ledger up to and including this entry
};

// Separate evidence of one explicit refresh attempt, not an original import.
struct TableObservationContext
{
    std::string data_instance, task_instance, task_id;
    std::uint32_t attempt = 0;
    std::string provider, provider_artifact, selected_snapshot, selected_content;
    auto operator<=>(const TableObservationContext&) const = default;
};

struct TableRefreshObservation
{
    std::string id, operation;
    TableObservationContext context;
    TableRevision revision;
    SourceResponse response;
    std::int64_t recorded_at_ns = 0;
    bool unchanged = false; // receipt of this delivery, not a claim about original import
};

struct TableCoverage
{
    TableSeries series;
    std::string exchange, first_period, last_period;
    std::uint64_t partitions = 0, rows = 0;
    std::int64_t acquired_at_ns = 0; // the most recent
};

// Market-wide tables as immutable partition files and an append-only ledger:
//
//   <root>/ledger.sqlite
//   <root>/objects/<first two digits>/<object>.parquet
//
// A partition is one exchange and one period of a series. Recording appends a
// revision; earlier revisions and their files stay. A snapshot is a prefix of
// the ledger, so a reader that keeps a snapshot keeps exactly what it read.
// One process owns the ledger; calls may come from any of its threads.
class TableStore final
{
public:
    enum class Access
    {
        writer,
        read_only,
        cold_read_only // Caller holds the stopped domain ownership lock.
    };
    explicit TableStore(std::filesystem::path root, Access = Access::writer);
    // Current schema, every historical snapshot link, object and raw response.
    // Cold backup adapters hold Data's ownership lock while verifying and copying.
    void verify_history(std::stop_token = {}) const;

    struct Retirement
    {
        std::string snapshot;
        std::uint64_t sequence = 0;
        std::vector<std::string> objects;
        bool complete = false;
    };

    std::optional<Retirement> retirement(const std::string&) const;
    std::vector<Retirement> retirements() const;
    Retirement preview_retirement(const std::string&, std::stop_token = {}) const;
    void begin_retirement(const Retirement&);
    void clean_retirement(const std::string&, std::stop_token = {});
    void finish_retirement(const std::string&);
    std::uint64_t retirement_bytes(const Retirement&) const;
    ~TableStore();
    // The revision a partition currently holds.
    std::optional<TableRevision>
    current(const TableSeries&, const std::string& exchange, const std::string& period) const;

    struct Object
    {
        std::string sha256;
        std::uint64_t bytes = 0;
    };

    // Writes a partition's file. Nothing reads it until record() names it.
    Object write(const TablePartition&) const;

    struct Recorded
    {
        TableRevision revision;
        bool unchanged = false; // the partition already held this content
    };

    Recorded record(
        const TablePartition&,
        const TableSeries&,
        const Object&,
        const std::string& provider_artifact,
        std::int64_t acquired_at_ns,
        const std::optional<SourceResponse>& response = {});
    // Verifies exact bytes and their hash-chain binding; absent only for
    // revisions that were originally recorded without raw evidence.
    std::optional<SourceResponse> response(const TableRevision&) const;
    std::optional<TableRefreshObservation> refresh_observation(
        const TableObservationContext&,
        const TableSeries&,
        const std::string& exchange,
        const std::string& period) const;
    TableRefreshObservation record_refresh_observation(
        const TableObservationContext&,
        const TableRevision&,
        const SourceResponse&,
        std::int64_t recorded_at_ns,
        bool unchanged);
    std::vector<TableObservationContext>
    snapshot_observers(const std::string&, std::stop_token = {}) const;
    std::optional<TableRefreshObservation> latest_refresh_observation(
        const TableSeries&,
        const std::string& exchange,
        const std::string& period) const;
    // The snapshot after the last entry; empty for an empty ledger.
    std::string head() const;
    std::vector<TableRevision> snapshot_catalogue(const std::string& after, unsigned limit) const;
    std::vector<TableCoverage> coverage() const;
    // Periods of one exchange that hold a revision, ascending, within [first, last].
    std::vector<std::string> periods(
        const TableSeries&,
        const std::string& exchange,
        const std::string& first,
        const std::string& last) const;
    // Where a snapshot ends in the ledger; nothing for a snapshot it never had.
    std::optional<std::uint64_t> sequence(const std::string& snapshot) const;
    // What a snapshot holds of one exchange within [first, last]: for each period
    // the newest revision up to the snapshot's last entry, ascending by period.
    std::vector<TableRevision> revisions(
        const TableSeries&,
        const std::string& exchange,
        const std::string& first,
        const std::string& last,
        std::uint64_t until) const;
    // The typed rows of a revision, after its file's digest has been checked.
    std::vector<std::vector<TableCell>> rows(const TableRevision&) const;
    // Revisions of one series read together, after every file's digest has been
    // checked: of each row whose first column starts with one of `prefixes`
    // (every row when there are none), the cells of `columns`, which index the
    // table's columns. `visit` is given the row's revision as an index into
    // `revisions`; rows come in no particular order.
    void scan(
        std::span<const TableRevision> revisions,
        std::span<const std::size_t> columns,
        std::span<const std::string> prefixes,
        const std::function<void(std::size_t, std::vector<TableCell>&)>& visit) const;

private:
    std::vector<Retirement> retirements_unlocked() const;
    std::vector<std::string> reclaimable_unlocked(const std::string&, std::stop_token = {}) const;
    void check_publishable_unlocked(const std::string&) const;
    std::optional<TableRevision> revision_unlocked(const std::string& snapshot) const;
    TableRefreshObservation
    observation_unlocked(const std::string& id, const std::string& operation) const;
    std::filesystem::path object_path(const std::string& object) const;
    std::filesystem::path root_;
    Access access_;
    void writable() const;
    mutable std::mutex mutex_;
    std::unique_ptr<sqlite::Database> ledger_;
};

} // namespace asterion::history_files
