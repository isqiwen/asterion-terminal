#include "history_archive.hpp"
#include "history_minutes.hpp"
#include "sqlite_database.hpp"
#include <asterion/foundation/error.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/file_lock.hpp>
#include <asterion/protocol/trading.hpp>
#include <algorithm>
#include <sys/stat.h>

namespace asterion::history_files {
namespace fs = std::filesystem;

namespace {

void revision(const std::string& id)
{
    if (id.size() != 64 || id.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::invalid_argument("invalid minute retirement revision");
}

fs::path authority(const fs::path& root)
{
    return root / "retirements.sqlite";
}

fs::path guard(const fs::path& root)
{
    return root / "retirements.required";
}

} // namespace

void Archive::check_retirement_authority() const
{
    for (const auto& path : {authority(root_), guard(root_)})
        if (fs::is_symlink(path) || (fs::exists(path) && !fs::is_regular_file(path)))
            throw std::invalid_argument("invalid minute retirement authority");
    if (!fs::exists(authority(root_))) {
        if (fs::exists(guard(root_)))
            throw std::runtime_error("minute retirement authority missing; recovery required");
        return;
    }
    sqlite::Database read(
        authority(root_),
        access_ == Access::cold_read_only ? sqlite::Database::Access::cold_read_only
                                          : sqlite::Database::Access::read_only);
    sqlite::Database::Statement rows(read, "SELECT 1 FROM retirements LIMIT 1");
    if (rows.step() && !fs::exists(guard(root_)))
        throw std::runtime_error("minute retirement guard missing; recovery required");
}

std::optional<data::proto::HistoryRetirement> Archive::retirement(const std::string& id) const
{
    std::lock_guard lock(retirement_mutex_);
    revision(id);
    check_retirement_authority();
    if (!fs::exists(authority(root_)))
        return {};
    sqlite::Database read(
        authority(root_),
        access_ == Access::cold_read_only ? sqlite::Database::Access::cold_read_only
                                          : sqlite::Database::Access::read_only);
    sqlite::Database::Statement row(read, "SELECT record,complete FROM retirements WHERE id=?");
    row.bind(1, id);
    if (!row.step())
        return {};
    data::proto::HistoryRetirement result;
    if (!result.mutable_record()->ParseFromString(row.blob(0)))
        throw std::invalid_argument("invalid minute retirement evidence");
    protocol::validate_message(result.record());
    if (result.record().minute_result().manifest_sha256() != id)
        throw std::invalid_argument("minute retirement identity mismatch");
    result.mutable_record()->mutable_minute_result()->set_directory(
        resolve_directory(result.record().minutes(), result.record().minute_result().directory())
            .string());
    result.set_complete(row.integer(1) != 0);
    (void)retirement_directory(result.record());
    return result;
}

std::vector<data::proto::HistoryRetirement>
Archive::retirements(const std::string& after, unsigned limit) const
{
    std::lock_guard lock(retirement_mutex_);
    if (!after.empty())
        revision(after);
    if (!limit || limit > 257)
        throw std::invalid_argument("invalid minute retirement page size");
    check_retirement_authority();
    if (!fs::exists(authority(root_)))
        return {};
    std::vector<std::string> ids;
    {
        sqlite::Database read(
            authority(root_),
            access_ == Access::cold_read_only ? sqlite::Database::Access::cold_read_only
                                              : sqlite::Database::Access::read_only);
        sqlite::Database::Statement rows(
            read,
            "SELECT id FROM retirements WHERE id>? ORDER BY id LIMIT ?");
        rows.bind(1, after).bind(2, static_cast<std::int64_t>(limit));
        while (rows.step())
            ids.push_back(rows.text(0));
    }
    std::vector<data::proto::HistoryRetirement> result;
    for (const auto& id : ids)
        result.push_back(*retirement(id));
    return result;
}

fs::path Archive::retirement_directory(const data::proto::HistoryRecord& record) const
{
    const auto range = minute_range(record.minutes());
    const fs::path path(record.minute_result().directory());
    const auto expected = root_ / range.instrument.venue / range.instrument.product
        / range.instrument.delivery_month / range.source
        / (std::to_string(range.interval_minutes) + "m") / path.filename();
    if (path != expected || path.filename() == "." || path.filename() == ".."
        || path.filename().empty())
        throw std::invalid_argument("minute retirement requires an owned acquisition directory");
    auto ancestor = path;
    while (ancestor != root_) {
        if (fs::is_symlink(ancestor))
            throw std::invalid_argument("minute retirement rejects symbolic links");
        ancestor = ancestor.parent_path();
    }
    return path;
}

std::uint64_t Archive::retirement_payload_bytes(
    const data::proto::HistoryRecord& record,
    std::stop_token stop) const
{
    const auto directory = retirement_directory(record);
    if (!fs::exists(directory))
        return 0;
    std::uint64_t bytes = 0;
    unsigned count = 0;
    for (const auto& entry : fs::recursive_directory_iterator(directory)) {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "minute retirement preview interrupted");
        if (++count > 20000)
            throw Error(
                ErrorCode::resource_exhausted,
                "minute retirement exceeds preview capacity");
        struct stat value{};
        if (::lstat(entry.path().c_str(), &value) != 0)
            throw std::runtime_error("minute payload size is unavailable");
        if (!S_ISREG(value.st_mode) && !S_ISDIR(value.st_mode))
            throw std::invalid_argument("minute payload contains an unsupported file");
        // Shared links do not promise independently reclaimable blocks.
        if (S_ISREG(value.st_mode) && value.st_nlink == 1)
            bytes += static_cast<std::uint64_t>(value.st_blocks) * 512;
    }
    return bytes;
}

void Archive::begin_retirement(
    const data::proto::HistoryRecord& record,
    const std::string& manifest)
{
    std::lock_guard metadata_lock(retirement_mutex_);
    writable();
    const auto& id = record.minute_result().manifest_sha256();
    revision(id);
    if (retirement(id))
        return;
    if (get(id).SerializeAsString() != record.SerializeAsString() || sha256_bytes(manifest) != id)
        throw Error(ErrorCode::conflict, "minute retirement evidence changed");
    (void)retirement_directory(record);
    for (const auto& other : datasets({}))
        if (other.id != id
            && get(other.id).minute_result().directory() == record.minute_result().directory())
            throw Error(ErrorCode::conflict, "minute payload has another published identity");
    FileLock lock(root_, "archive.lock");
    sqlite::Database database(authority(root_));
    database.execute(
        "CREATE TABLE IF NOT EXISTS retirements(id TEXT PRIMARY KEY,record BLOB NOT NULL,manifest BLOB NOT NULL,complete INTEGER NOT NULL CHECK(complete IN (0,1))) STRICT");
    if (!fs::exists(guard(root_)))
        replace_file_durably(guard(root_), "minute-retirement-authority\n");
    else
        sync_file_durably(guard(root_));
    sqlite::Database::Transaction transaction(database);
    sqlite::Database::Statement write(
        database,
        "INSERT INTO retirements(id,record,manifest,complete) VALUES(?,?,?,0)");
    write.bind(1, id).bind_blob(2, record.SerializeAsString()).bind_blob(3, manifest).step();
    transaction.commit();
    sync_directory(root_);
}

void Archive::clean_retirement(const std::string& id, std::stop_token stop)
{
    writable();
    const auto decision = retirement(id);
    if (!decision)
        throw Error(ErrorCode::conflict, "minute version has no durable retirement decision");
    if (decision->complete())
        return;
    const auto directory = retirement_directory(decision->record());
    if (!fs::exists(directory)) {
        sync_directory(directory.parent_path());
        return;
    }
    // Existing controlled readers finish before physical cleanup; no new Data
    // read or reference can pass the durable seal.
    FileLock lock(directory, "minutes.lock");
    std::vector<fs::path> entries;
    for (const auto& entry : fs::recursive_directory_iterator(directory)) {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "minute retirement cleanup interrupted");
        if (entry.is_symlink() || (!entry.is_regular_file() && !entry.is_directory()))
            throw std::invalid_argument("minute retirement encountered an unsupported file");
        entries.push_back(entry.path());
        if (entries.size() > 20000)
            throw Error(
                ErrorCode::resource_exhausted,
                "minute retirement exceeds cleanup capacity");
    }
    std::ranges::sort(entries, std::greater<>{});
    for (const auto& path : entries) {
        if (stop.stop_requested())
            throw Error(ErrorCode::cancelled, "minute retirement cleanup interrupted");
        fs::remove(path);
    }
    fs::remove(directory);
    sync_directory(directory.parent_path());
}

void Archive::finish_retirement(const std::string& id)
{
    std::lock_guard metadata_lock(retirement_mutex_);
    writable();
    const auto decision = retirement(id);
    if (!decision)
        throw Error(ErrorCode::conflict, "minute version has no retirement decision");
    if (decision->complete())
        return;
    if (fs::exists(retirement_directory(decision->record())))
        throw Error(ErrorCode::conflict, "minute retirement cleanup is incomplete");
    sync_directory(fs::path(decision->record().minute_result().directory()).parent_path());
    sqlite::Database database(authority(root_));
    sqlite::Database::Transaction transaction(database);
    sqlite::Database::Statement write(database, "UPDATE retirements SET complete=1 WHERE id=?");
    write.bind(1, id).step();
    transaction.commit();
}

} // namespace asterion::history_files
