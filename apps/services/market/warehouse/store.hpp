#pragma once
#include <asterion/base/decimal.hpp>
#include <filesystem>
#include <map>
#include <mutex>
#include <span>
#include <variant>
#include <vector>

namespace asterion::market_data::warehouse {

enum class Kind
{
    text,
    date,
    boolean,
    integer,
    number,
    price
};

struct Column
{
    std::string_view name;
    Kind kind;
};

std::span<const Column> columns(std::string_view table);
// Storage numbers have the existing DECIMAL(28,6) range. Decimal is accepted
// for prices; exact decimal text also preserves wider provider quantities.
using Cell = std::variant<std::monostate, std::string, bool, std::int64_t, Decimal>;
using Row = std::vector<Cell>;

struct Partition
{
    std::string file, sha256;
    std::uint64_t rows = 0;
    std::vector<std::string> days;
    // The provider whose partition this is, when taken from another store.
    std::string source = {};
    bool operator==(const Partition&) const = default;
};

struct Version
{
    std::string identity, provider, parent;
    std::int64_t created_ns = 0;
    std::map<std::string, Partition> partitions;
};

// A version without its partitions: what it follows and its shards' digests.
struct VersionHead
{
    std::string identity, parent;
    std::int64_t created_ns = 0;
    std::map<std::string, std::string> shards;
};

// Partitions one version adds to another, replaces in it and removes from it.
struct Change
{
    std::uint32_t added = 0, replaced = 0, removed = 0;
};

class Writer;

// The same on-disk contract used by research: immutable partition files,
// versions/<sha256>.json, and an atomically published current pointer. A
// version lists its partitions in shards, one per table, exchange and month
// (shards/<sha256>.json, named by their content), so a commit writes only the
// shards it changed and no manifest grows with the whole history.
// Construction/reads create nothing. Market owns the installation lock;
// one Store per source serializes its writers. Reads never take that lock.
class Store
{
public:
    Store(std::filesystem::path directory, std::string provider);
    Version current() const;
    // Empty before the first commit.
    std::string current_identity() const;
    Version version(std::string_view identity) const;
    VersionHead head(std::string_view identity) const;
    // Only shards whose digests differ are read.
    Change compare(const VersionHead& version, const VersionHead& base) const;
    std::filesystem::path file(const Version&, std::string_view key) const;
    // One partition of a version, reading only the shard that holds it.
    std::pair<Partition, std::filesystem::path>
    locate(std::string_view identity, std::string_view key) const;
    std::map<std::string, Partition>
    shard(const std::string& name, const std::string& digest) const;
    std::vector<Row> read(const Version&, std::string_view key) const;

    const std::filesystem::path& root() const
    {
        return root_;
    }

    const std::string& provider() const
    {
        return provider_;
    }

    Writer writer();

private:
    friend class Writer;
    std::filesystem::path path_of(const Partition&) const;

    std::filesystem::path root_;
    std::string provider_;
    std::mutex writes_;
};

class Writer
{
public:
    explicit Writer(Store&);
    // Replace a complete partition, publishing none of the transaction until commit.
    void put(std::string key, std::span<const Row> rows, std::vector<std::string> days = {});
    // Takes the partition `key` of another store's version whole, linking its
    // file instead of copying it.
    void adopt(const std::string& key, const Store& from, const Version& version);
    // Starts the next version empty; commit() still publishes only a change.
    void clear();
    Version commit();

private:
    Store& store_;
    std::unique_lock<std::mutex> lock_;
    Version base_, pending_;
};

// Data paths are service-owned; provider/partition/version identities arrive
// from clients. Validate them before any filesystem access.
void require_provider(std::string_view provider);
// The shard a partition belongs to: its table, exchange and month. Shards sort
// as their partitions do.
std::string shard_of(const std::string& key);
void require_identity(std::string_view identity);
void require_key(std::string_view key);
void require_day(std::string_view day);
std::filesystem::path source_directory(const std::filesystem::path&, std::string_view provider);

} // namespace asterion::market_data::warehouse
