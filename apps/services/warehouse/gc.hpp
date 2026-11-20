#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace asterion::warehouse {

struct GcOptions
{
    std::filesystem::path data_directory;
    std::size_t keep_last = 5;
    std::size_t keep_source_last = 1;
    std::set<std::string> keep_versions;
    bool apply = false;
};

struct StoreGcReport
{
    std::string provider;
    std::size_t kept_versions = 0;
    std::size_t removed_versions = 0;
    std::size_t kept_shards = 0;
    std::size_t removed_shards = 0;
    std::size_t kept_files = 0;
    std::size_t removed_files = 0;
    std::uint64_t unlinked_bytes = 0;
    std::uint64_t freed_bytes = 0;
};

struct GcReport
{
    bool applied = false;
    std::vector<StoreGcReport> stores;
};

std::set<std::string> read_keep_versions_file(const std::filesystem::path& path);
GcReport collect_garbage(const GcOptions& options);

} // namespace asterion::warehouse
