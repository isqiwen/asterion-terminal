#include "history_tables.hpp"
#include <asterion/database/sqlite/sqlite_database.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/foundation/error.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <map>
#include <set>
#include <tuple>
#include <sys/stat.h>

namespace asterion::history_files {
namespace fs = std::filesystem;

namespace {

using Json = nlohmann::json;

void digest(const std::string& value)
{
    if (value.size() != 64 || value.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::invalid_argument("invalid table retirement identity");
}

void interrupted(std::stop_token stop)
{
    if (stop.stop_requested())
        throw Error(ErrorCode::cancelled, "table retirement interrupted");
}

} // namespace

std::vector<TableStore::Retirement> TableStore::retirements_unlocked() const
{
    const auto file = root_ / "retirements.sqlite";
    const auto guard = root_ / "retirements.required";
    for (const auto& path : {file, guard})
        if (fs::is_symlink(path) || (fs::exists(path) && !fs::is_regular_file(path)))
            throw std::invalid_argument("invalid table retirement authority");
    if (!fs::exists(file)) {
        if (fs::exists(guard))
            throw std::runtime_error("table retirement authority missing; recovery required");
        return {};
    }
    sqlite::Database db(
        file,
        access_ == Access::cold_read_only ? sqlite::Database::Access::cold_read_only
                                          : sqlite::Database::Access::read_only);
    sqlite::Database::Statement rows(
        db,
        "SELECT snapshot,sequence,objects,complete FROM retirements ORDER BY sequence");
    std::vector<Retirement> result;
    while (rows.step()) {
        if (!fs::exists(guard))
            throw std::runtime_error("table retirement guard missing; recovery required");
        if (result.size() == 100000)
            throw Error(ErrorCode::resource_exhausted, "table retirement capacity exceeded");
        Retirement item{
            rows.text(0),
            static_cast<std::uint64_t>(rows.integer(1)),
            {},
            rows.integer(3) == 1
        };
        digest(item.snapshot);
        const auto raw = rows.text(2);
        if (raw.size() > 8 * 1024 * 1024 || rows.integer(1) <= 0
            || (rows.integer(3) != 0 && rows.integer(3) != 1))
            throw std::invalid_argument("invalid table retirement evidence");
        const auto objects = Json::parse(raw);
        if (!objects.is_array() || objects.size() > 100000)
            throw std::invalid_argument("invalid retired object list");
        for (const auto& object : objects) {
            const auto id = object.get<std::string>();
            digest(id);
            if (!item.objects.empty() && item.objects.back() >= id)
                throw std::invalid_argument("unordered retired objects");
            item.objects.push_back(id);
        }
        sqlite::Database::Statement source(
            *ledger_,
            "SELECT snapshot FROM table_revisions WHERE sequence=?");
        source.bind(1, static_cast<std::int64_t>(item.sequence));
        if (!source.step() || source.text(0) != item.snapshot)
            throw std::invalid_argument("table retirement chain identity mismatch");
        sqlite::Database::Statement head(*ledger_, "SELECT max(sequence) FROM table_revisions");
        head.step();
        if (item.sequence >= static_cast<std::uint64_t>(head.integer(0)))
            throw std::invalid_argument("retired table head is invalid");
        result.push_back(std::move(item));
    }
    return result;
}

std::vector<TableStore::Retirement> TableStore::retirements() const
{
    std::lock_guard lock(mutex_);
    return retirements_unlocked();
}

std::optional<TableStore::Retirement> TableStore::retirement(const std::string& snapshot) const
{
    digest(snapshot);
    std::lock_guard lock(mutex_);
    for (auto& item : retirements_unlocked())
        if (item.snapshot == snapshot)
            return item;
    return {};
}

std::vector<std::string>
TableStore::reclaimable_unlocked(const std::string& selected, std::stop_token stop) const
{
    std::set<std::string> retired;
    for (const auto& entry : retirements_unlocked())
        retired.insert(entry.snapshot);
    if (!selected.empty())
        retired.insert(selected);

    struct Revision
    {
        std::uint64_t sequence;
        std::tuple<std::string, std::int64_t, std::string, std::string, std::string> partition;
        std::string object;
    };

    std::vector<Revision> revisions;
    std::vector<std::uint64_t> available;
    sqlite::Database::Statement rows(
        *ledger_,
        "SELECT sequence,dataset,schema,source,exchange,period,object,snapshot FROM table_revisions ORDER BY sequence");
    while (rows.step()) {
        interrupted(stop);
        if (revisions.size() == 100000)
            throw Error(
                ErrorCode::resource_exhausted,
                "table retirement preview capacity exceeded");
        const auto sequence = static_cast<std::uint64_t>(rows.integer(0));
        if (!retired.contains(rows.text(7)))
            available.push_back(sequence);
        revisions.push_back(
            {sequence,
                {rows.text(1), rows.integer(2), rows.text(3), rows.text(4), rows.text(5)},
                rows.text(6)});
    }
    std::map<decltype(Revision::partition), std::uint64_t> next;
    std::set<std::string> all, live;
    for (auto i = revisions.rbegin(); i != revisions.rend(); ++i) {
        interrupted(stop);
        digest(i->object);
        all.insert(i->object);
        const auto until = next.find(i->partition);
        const auto visible = std::lower_bound(available.begin(), available.end(), i->sequence);
        if (visible != available.end() && (until == next.end() || *visible < until->second))
            live.insert(i->object);
        next[i->partition] = i->sequence;
    }
    std::vector<std::string> result;
    std::set_difference(
        all.begin(),
        all.end(),
        live.begin(),
        live.end(),
        std::back_inserter(result));
    return result;
}

TableStore::Retirement
TableStore::preview_retirement(const std::string& snapshot, std::stop_token stop) const
{
    digest(snapshot);
    std::lock_guard lock(mutex_);
    const auto decisions = retirements_unlocked();
    for (const auto& item : decisions) {
        if (item.snapshot == snapshot)
            return item;
        if (!item.complete)
            throw Error(ErrorCode::conflict, "resume pending table retirement first");
    }
    sqlite::Database::Statement source(
        *ledger_,
        "SELECT sequence FROM table_revisions WHERE snapshot=?");
    source.bind(1, snapshot);
    if (!source.step())
        throw Error(ErrorCode::not_found, "unknown table snapshot");
    Retirement result{snapshot, static_cast<std::uint64_t>(source.integer(0)), {}, false};
    sqlite::Database::Statement head(*ledger_, "SELECT max(sequence) FROM table_revisions");
    head.step();
    if (result.sequence == static_cast<std::uint64_t>(head.integer(0)))
        throw Error(ErrorCode::conflict, "current table head cannot retire");
    for (const auto& id : reclaimable_unlocked(snapshot, stop)) {
        const auto file = object_path(id);
        if (fs::is_symlink(file) || (fs::exists(file) && !fs::is_regular_file(file)))
            throw std::invalid_argument("invalid table retirement payload");
        if (fs::exists(file))
            result.objects.push_back(id);
        else if (std::ranges::none_of(decisions, [&](const auto& prior) {
                     return std::binary_search(prior.objects.begin(), prior.objects.end(), id);
                 }))
            throw std::invalid_argument("unaccounted missing table object");
    }
    return result;
}

void TableStore::begin_retirement(const Retirement& expected)
{
    writable();
    // Data holds its catalog writer across preview, sealing and cleanup.
    const auto current = preview_retirement(expected.snapshot);
    if (current.sequence != expected.sequence || current.objects != expected.objects
        || current.complete != expected.complete)
        throw Error(ErrorCode::conflict, "table retirement preview changed");
    std::lock_guard lock(mutex_);
    for (const auto& prior : retirements_unlocked())
        if (prior.snapshot == expected.snapshot)
            return;
    sqlite::Database db(root_ / "retirements.sqlite");
    db.execute(
        "CREATE TABLE IF NOT EXISTS retirements(snapshot TEXT PRIMARY KEY,sequence INTEGER NOT NULL UNIQUE,objects TEXT NOT NULL,complete INTEGER NOT NULL CHECK(complete IN (0,1))) STRICT");
    if (!fs::exists(root_ / "retirements.required"))
        replace_file_durably(root_ / "retirements.required", "table-retirement-authority\n");
    else
        sync_file_durably(root_ / "retirements.required");
    sqlite::Database::Transaction transaction(db);
    sqlite::Database::Statement row(db, "INSERT INTO retirements VALUES(?,?,?,0)");
    row.bind(1, expected.snapshot)
        .bind(2, static_cast<std::int64_t>(expected.sequence))
        .bind(3, Json(expected.objects).dump())
        .step();
    transaction.commit();
    sync_directory(root_);
}

void TableStore::check_publishable_unlocked(const std::string& object) const
{
    for (const auto& entry : retirements_unlocked())
        if (!entry.complete
            && std::binary_search(entry.objects.begin(), entry.objects.end(), object))
            throw Error(ErrorCode::conflict, "table object is sealed for cleanup");
}

std::uint64_t TableStore::retirement_bytes(const Retirement& decision) const
{
    std::uint64_t bytes = 0;
    for (const auto& id : decision.objects) {
        digest(id);
        const auto file = object_path(id);
        struct stat st{};
        if (!fs::exists(file))
            continue;
        if (::lstat(file.c_str(), &st) || !S_ISREG(st.st_mode))
            throw std::invalid_argument("invalid table retirement payload");
        if (st.st_nlink == 1)
            bytes += static_cast<std::uint64_t>(st.st_blocks) * 512;
    }
    return bytes;
}

void TableStore::clean_retirement(const std::string& snapshot, std::stop_token stop)
{
    writable();
    std::lock_guard lock(mutex_);
    const auto decisions = retirements_unlocked();
    const auto item = std::ranges::find(decisions, snapshot, &Retirement::snapshot);
    if (item == decisions.end())
        throw Error(ErrorCode::conflict, "table retirement decision missing");
    if (item->complete)
        return;
    const auto allowed = reclaimable_unlocked({}, stop);
    for (const auto& id : item->objects) {
        interrupted(stop);
        if (!std::binary_search(allowed.begin(), allowed.end(), id))
            throw Error(ErrorCode::conflict, "retired object has a live snapshot");
        const auto file = object_path(id);
        if (!fs::exists(file)) {
            sync_directory(file.parent_path());
            continue;
        }
        if (fs::is_symlink(file) || !fs::is_regular_file(file))
            throw std::invalid_argument("table retirement object is not a file");
        fs::remove(file);
        sync_directory(file.parent_path());
    }
}

void TableStore::finish_retirement(const std::string& snapshot)
{
    writable();
    std::lock_guard lock(mutex_);
    const auto decisions = retirements_unlocked();
    const auto item = std::ranges::find(decisions, snapshot, &Retirement::snapshot);
    if (item == decisions.end())
        throw Error(ErrorCode::conflict, "table retirement decision missing");
    if (item->complete)
        return;
    for (const auto& id : item->objects)
        if (fs::exists(object_path(id)) || fs::is_symlink(object_path(id)))
            throw Error(ErrorCode::conflict, "table cleanup incomplete");
    sqlite::Database db(root_ / "retirements.sqlite");
    sqlite::Database::Transaction transaction(db);
    sqlite::Database::Statement row(db, "UPDATE retirements SET complete=1 WHERE snapshot=?");
    row.bind(1, snapshot).step();
    transaction.commit();
}

} // namespace asterion::history_files
