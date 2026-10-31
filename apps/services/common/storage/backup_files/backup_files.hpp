#pragma once
#include <asterion/base/serialization.hpp>
#include <asterion/system/process/file_lock.hpp>
#include <asterion/resource.pb.h>
#include <filesystem>
#include <memory>
#include <stop_token>
#include <vector>
#include <utility>

namespace asterion::backup_files {

struct File
{
    std::filesystem::path source;
    std::filesystem::path relative;
    std::string format;
};

struct Dependency
{
    resource::proto::Object object;
    bool included = false;
};

// A domain adapter selects business files and holds its existing ownership lock.
// It never passes credentials, runtime state or an unchecked directory glob.
struct Snapshot
{
    resource::proto::Owner owner;
    std::filesystem::path directory;
    std::unique_ptr<FileLock> ownership;
    std::vector<File> files;
    std::vector<std::filesystem::path> directories;
    std::vector<Dependency> dependencies;
    std::vector<std::string> exclusions;
    std::vector<std::string> limitations;
    // Business-owner declarations for informed local preview; never object identity.
    std::vector<std::pair<std::string, std::string>> scope;
};

// One immutable local cold cut across the selected domains. No service or task
// framework: existing bounded client file workers own these operations.
class Cut
{
public:
    explicit Cut(std::vector<Snapshot>);
    Json preview(const std::string& operation_id, std::stop_token = {}) const;
    // A new destination or the same operation's verified incomplete directory.
    // expected_digest is the reviewed preview's SHA-256, not permission to skip checks.
    Json save(
        const std::string& operation_id,
        const std::filesystem::path& destination,
        const std::string& expected_digest,
        std::stop_token = {}) const;

private:
    std::vector<Snapshot> snapshots_;
};

// Byte integrity only. Domain adapters must still validate restored schema,
// object/reference closure and location resolution before reporting recovery.
Json inspect(const std::filesystem::path&, std::stop_token = {});
// Persisted operation scope only; callers must reacquire the owners and compare
// the current preview before resuming any writes. Never authorizes restore.
Json inspect_preparation(const std::filesystem::path&, std::stop_token = {});
Json restore_files(
    const std::filesystem::path& backup,
    const std::filesystem::path& destination,
    std::stop_token = {});

} // namespace asterion::backup_files
