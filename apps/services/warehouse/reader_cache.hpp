#pragma once
#include "freshness.hpp"
#include "reader.hpp"
#include <asterion/protocol/warehouse.hpp>
#include <deque>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace asterion::warehouse {

struct FileStamp
{
    bool operator==(const FileStamp&) const = default;

    bool exists = false;
    std::filesystem::file_time_type mtime{};
    std::uintmax_t size = 0;
};

using EmptyStamp = std::vector<FileStamp>;
using ProductCoverages = std::map<std::string, std::vector<::warehouse::ContractCoverage>>;

inline Store research_dataset(const std::filesystem::path& directory)
{
    return Store(source_directory(directory, "research"), "research");
}

inline EmptyStamp
empty_stamp(const std::filesystem::path& directory, std::initializer_list<const char*> tables)
{
    EmptyStamp result;
    result.reserve(tables.size());
    for (const auto* table : tables) {
        const auto path = empty_file(directory, table);
        std::error_code ec;
        const bool exists = std::filesystem::is_regular_file(path, ec);
        if (ec || !exists) {
            result.push_back({});
            continue;
        }
        const auto mtime = std::filesystem::last_write_time(path, ec);
        const auto size = ec ? 0 : std::filesystem::file_size(path, ec);
        result.push_back(ec ? FileStamp{} : FileStamp{true, mtime, size});
    }
    return result;
}

template <class Key, class Value>
struct BoundedCache
{
    std::optional<Value> get(const Key& key) const
    {
        for (const auto& [k, v] : entries)
            if (k == key)
                return v;
        return std::nullopt;
    }

    void put(Key key, Value value, std::size_t capacity)
    {
        for (auto& [k, v] : entries)
            if (k == key) {
                v = std::move(value);
                return;
            }
        if (entries.size() >= capacity)
            entries.pop_front();
        entries.emplace_back(std::move(key), std::move(value));
    }

    std::deque<std::pair<Key, Value>> entries;
};

inline ::warehouse::DatasetChange
change_of(const Store& store, const VersionHead& version, const VersionHead& base)
{
    const auto counts = store.compare(version, base);
    ::warehouse::DatasetChange out;
    out.mutable_dataset()->set_version(version.identity);
    out.mutable_dataset()->set_created_ms(version.created_ns / 1'000'000);
    out.set_base(base.identity);
    out.set_added(counts.added);
    out.set_replaced(counts.replaced);
    out.set_removed(counts.removed);
    for (const auto& table : counts.tables)
        out.add_tables(protocol::table_of(table));
    return out;
}

// Content-addressed versions, heads, diffs, and derived summaries/coverages are immutable
// for a given (version, day, empty-keys stamp); caching them avoids rescanning Parquet and shards.
struct Reader::Cache
{
    using Key = std::tuple<std::string, std::string, EmptyStamp>;
    using FilterKey = std::tuple<std::string, std::string, std::string, std::string>;
    using CoverageEntry =
        std::pair<std::shared_ptr<const ProductCoverages>, ::warehouse::DatasetCoverage>;

    struct FilteredCounts
    {
        std::uint64_t total = 0;
        std::vector<std::uint64_t> batch_counts;
    };

    std::shared_ptr<const Version> version(const Store& store, const std::string& identity)
    {
        if (identity.empty())
            return std::make_shared<Version>(Version{{}, store.provider(), {}, 0, {}});
        require_identity(identity);
        {
            std::lock_guard lock(mutex);
            if (auto found = versions.get(identity))
                return *found;
        }
        auto loaded = std::make_shared<Version>(store.version(identity));
        std::lock_guard lock(mutex);
        versions.put(identity, loaded, 2);
        return loaded;
    }

    VersionHead head(const Store& store, const std::string& identity)
    {
        require_identity(identity);
        {
            std::lock_guard lock(mutex);
            if (auto found = heads.get(identity))
                return *found;
        }
        auto loaded = store.head(identity);
        std::lock_guard lock(mutex);
        heads.put(identity, loaded, 64);
        return loaded;
    }

    ::warehouse::DatasetChange
    change(const Store& store, const VersionHead& version, const VersionHead& base)
    {
        const std::pair key{version.identity, base.identity};
        {
            std::lock_guard lock(mutex);
            if (auto found = changes.get(key))
                return *found;
        }
        auto computed = change_of(store, version, base);
        std::lock_guard lock(mutex);
        changes.put(key, computed, 64);
        return computed;
    }

    std::mutex mutex;
    BoundedCache<std::string, std::shared_ptr<const Version>> versions;
    BoundedCache<std::string, VersionHead> heads;
    BoundedCache<std::pair<std::string, std::string>, ::warehouse::DatasetChange> changes;
    BoundedCache<Key, ::warehouse::DatasetSummary> summaries;
    BoundedCache<Key, CoverageEntry> coverages;
    BoundedCache<FilterKey, FilteredCounts> filtered_counts;
};

} // namespace asterion::warehouse
