#include "gc.hpp"
#include "store.hpp"
#include <asterion/system/durable_file.hpp>
#include <asterion/system/process/file_lock.hpp>
#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace asterion::warehouse {
namespace fs = std::filesystem;

namespace {

std::string trim(std::string value)
{
    const auto is_space = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    while (!value.empty() && is_space(static_cast<unsigned char>(value.front())))
        value.erase(value.begin());
    while (!value.empty() && is_space(static_cast<unsigned char>(value.back())))
        value.pop_back();
    return value;
}

std::vector<std::pair<fs::path, std::string>> discover_stores(const fs::path& data)
{
    std::vector<std::pair<fs::path, std::string>> stores;
    if (fs::is_directory(data / "warehouse"))
        stores.emplace_back(data, "tushare");
    const auto sources = data / "sources";
    if (fs::is_directory(sources)) {
        std::vector<std::string> providers;
        for (const auto& entry : fs::directory_iterator(sources)) {
            if (!entry.is_directory())
                continue;
            const auto name = entry.path().filename().string();
            if (name.starts_with('.') || !fs::is_directory(entry.path() / "warehouse"))
                continue;
            require_provider(name);
            providers.push_back(name);
        }
        std::sort(providers.begin(), providers.end());
        for (const auto& provider : providers)
            stores.emplace_back(sources / provider, provider);
    }
    return stores;
}

StoreGcReport gc_store(
    const fs::path& base,
    const std::string& provider,
    std::size_t keep_last,
    const std::set<std::string>& pinned,
    bool apply)
{
    Store store(base, provider);
    StoreGcReport report;
    report.provider = provider;

    std::set<std::string> keep_versions;
    if (const auto head_id = store.current_identity(); !head_id.empty()) {
        std::string cursor = head_id;
        for (std::size_t n = 0; n < keep_last && !cursor.empty(); ++n) {
            keep_versions.insert(cursor);
            const auto head = store.head(cursor);
            if (head.parent.empty() || !store.has_version(head.parent))
                break;
            cursor = head.parent;
        }
    }
    for (const auto& id : pinned) {
        require_identity(id);
        if (!store.has_version(id))
            throw std::invalid_argument("pinned version does not exist in " + provider + ": " + id);
        keep_versions.insert(id);
    }

    // Verify every kept version, shard, and Parquet file before planning any deletion.
    std::set<std::string> keep_shards;
    std::set<std::string> keep_files;
    for (const auto& id : keep_versions) {
        const auto head = store.head(id);
        for (const auto& [name, digest] : head.shards) {
            keep_shards.insert(digest);
            for (const auto& [_, part] : store.shard(name, digest)) {
                const auto file = store.root() / part.file;
                if (!fs::is_regular_file(file))
                    throw std::runtime_error(
                        "kept version " + id + " references missing file: " + file.string());
                keep_files.insert(part.file);
            }
        }
    }
    report.kept_versions = keep_versions.size();
    report.kept_shards = keep_shards.size();
    report.kept_files = keep_files.size();

    std::vector<fs::path> dead_versions;
    if (const auto dir = store.root() / "versions"; fs::is_directory(dir)) {
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".json")
                continue;
            const auto stem = entry.path().stem().string();
            if (stem.size() == 64 && !keep_versions.contains(stem))
                dead_versions.push_back(entry.path());
        }
    }
    std::vector<fs::path> dead_shards;
    if (const auto dir = store.root() / "shards"; fs::is_directory(dir)) {
        for (const auto& entry : fs::directory_iterator(dir)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".json")
                continue;
            const auto stem = entry.path().stem().string();
            if (stem.size() == 64 && !keep_shards.contains(stem))
                dead_shards.push_back(entry.path());
        }
    }
    std::vector<fs::path> dead_files;
    if (const auto dir = store.root() / "files"; fs::is_directory(dir)) {
        for (const auto& entry : fs::recursive_directory_iterator(dir)) {
            if (!entry.is_regular_file() || entry.path().extension() != ".parquet")
                continue;
            const auto rel = fs::relative(entry.path(), store.root()).generic_string();
            if (!keep_files.contains(rel))
                dead_files.push_back(entry.path());
        }
    }

    auto account_and_remove = [&](const std::vector<fs::path>& paths, std::size_t& count) {
        for (const auto& path : paths) {
            const auto size = fs::file_size(path);
            const auto links = fs::hard_link_count(path);
            report.unlinked_bytes += size;
            if (links <= 1)
                report.freed_bytes += size;
            ++count;
            if (apply)
                fs::remove(path);
        }
    };

    // Delete manifests before shards and shards before Parquet files so an
    // interrupted run never leaves a surviving version with a dangling reference.
    account_and_remove(dead_versions, report.removed_versions);
    account_and_remove(dead_shards, report.removed_shards);
    account_and_remove(dead_files, report.removed_files);

    if (apply) {
        if (const auto dir = store.root() / "files"; fs::is_directory(dir)) {
            std::vector<fs::path> subdirs;
            for (const auto& entry : fs::recursive_directory_iterator(dir))
                if (entry.is_directory())
                    subdirs.push_back(entry.path());
            std::sort(subdirs.begin(), subdirs.end(), [](const fs::path& a, const fs::path& b) {
                return a.native().size() > b.native().size();
            });
            for (const auto& subdir : subdirs)
                if (fs::is_empty(subdir))
                    fs::remove(subdir);
        }
        for (const char* sub : {"versions", "shards", "files"}) {
            if (const auto dir = store.root() / sub; fs::is_directory(dir))
                sync_directory(dir);
        }
    }
    return report;
}

} // namespace

std::set<std::string> read_keep_versions_file(const fs::path& path)
{
    if (!fs::is_regular_file(path))
        throw std::invalid_argument("keep-versions file not found: " + path.string());
    std::ifstream input(path);
    std::set<std::string> versions;
    std::string line;
    while (std::getline(input, line)) {
        const auto cleaned = trim(line);
        if (cleaned.empty() || cleaned.starts_with('#'))
            continue;
        require_identity(cleaned);
        versions.insert(cleaned);
    }
    return versions;
}

GcReport collect_garbage(const GcOptions& options)
{
    if (options.keep_last < 1 || options.keep_source_last < 1)
        throw std::invalid_argument("keep_last and keep_source_last must be at least 1");
    if (!options.data_directory.is_absolute() || !fs::is_directory(options.data_directory))
        throw std::invalid_argument(
            "warehouse data directory does not exist: " + options.data_directory.string());

    // Refuse to run while asterion_warehouse holds the exclusive lock.
    FileLock owner(options.data_directory, "warehouse.lock");

    const auto stores = discover_stores(options.data_directory);
    const bool has_research = std::any_of(stores.begin(), stores.end(), [](const auto& item) {
        return item.second == "research";
    });
    if (!options.keep_versions.empty() && !has_research)
        throw std::invalid_argument("pinned versions requested but no research store exists");

    GcReport report;
    report.applied = options.apply;
    for (const auto& [base, provider] : stores) {
        const bool is_research = provider == "research";
        report.stores.push_back(gc_store(
            base,
            provider,
            is_research ? options.keep_last : options.keep_source_last,
            is_research ? options.keep_versions : std::set<std::string>{},
            options.apply));
    }
    return report;
}

} // namespace asterion::warehouse
