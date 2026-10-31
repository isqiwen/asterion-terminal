#include "program_files.hpp"
#include <asterion/base/id.hpp>
#include <asterion/system/durable_file.hpp>
#include <asterion/system/process/child.hpp>
#include <asterion/protocol/trading.hpp>
#include <algorithm>
#include <fstream>

namespace asterion::programs {
namespace fs = std::filesystem;

namespace {

constexpr std::uintmax_t max_stored_bytes = 512ULL * 1024 * 1024;

void digest(const std::string& value)
{
    if (value.size() != 64 || value.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::invalid_argument("program archive requires an exact native artifact identity");
}

void cancelled(std::stop_token stop)
{
    if (stop.stop_requested())
        throw Error(ErrorCode::cancelled, "program archive operation cancelled");
}

bool temporary(const fs::directory_entry& entry)
{
    const auto name = entry.path().filename().string();
    return name.starts_with("owner.pb.tmp.") && name.size() > 13 && entry.is_regular_file()
        && !entry.is_symlink();
}

void native(const HostPlatform& value)
{
    if (!((value.os == "linux" && value.arch == "x86_64")
            || (value.os == "macos" && value.arch == "arm64")))
        throw std::invalid_argument("unsupported archived program platform");
}

} // namespace

Files::Files(fs::path directory, resource::proto::Owner owner)
    : directory_(std::move(directory)),
      owner_(std::move(owner))
{
    protocol::validate_message(owner_);
    validate_id(owner_.node_id());
    validate_id(owner_.service_id());
    validate_id(owner_.domain());
    if (!directory_.is_absolute() || fs::is_symlink(directory_))
        throw std::invalid_argument("program archive requires its absolute owned directory");
}

bool Files::authority() const
{
    if (!fs::exists(directory_) && !fs::is_symlink(directory_))
        return false;
    if (fs::is_symlink(directory_) || !fs::is_directory(directory_))
        throw std::invalid_argument("invalid program archive directory");
    const auto file = directory_ / "owner.pb";
    if (!fs::exists(file) && !fs::is_symlink(file)) {
        // No program may be published before its owner. Only an interrupted,
        // unacknowledged namespace initialization may be continued. Keep its temps.
        unsigned inspected = 0;
        for (const auto& entry : fs::directory_iterator(directory_)) {
            if (++inspected > 100000)
                throw Error(ErrorCode::resource_exhausted, "program archive scan exceeds capacity");
            if (!entry.is_symlink()
                && ((entry.path().filename() == "staging" && entry.is_directory())
                    || temporary(entry)))
                continue;
            throw std::invalid_argument("published program archive owner is missing");
        }
        return false;
    }
    if (fs::is_symlink(file) || !fs::is_regular_file(file) || fs::file_size(file) > 4096)
        throw std::invalid_argument("invalid program archive owner");
    std::ifstream input(file, std::ios::binary);
    resource::proto::Owner stored;
    if (!stored.ParseFromIstream(&input) || input.bad())
        throw std::invalid_argument("invalid program archive owner protobuf");
    protocol::validate_message(stored);
    if (stored.SerializeAsString() != owner_.SerializeAsString())
        throw Error(ErrorCode::conflict, "program archive owner mismatch");
    return true;
}

fs::path Files::path(const std::string& value) const
{
    digest(value);
    return directory_ / value;
}

Files::Entry Files::metadata(const std::string& value) const
{
    const auto file = path(value);
    if (fs::is_symlink(file) || !fs::is_regular_file(file))
        throw Error(ErrorCode::not_found, "fixed execution program is missing");
    const auto bytes = fs::file_size(file);
    if (!bytes || bytes > max_stored_bytes)
        throw std::invalid_argument("invalid archived program size");
    const auto platform = artifact_platform(file);
    native(platform);
    return {value, bytes, platform};
}

Files::Entry Files::find(const std::string& value) const
{
    digest(value);
    if (!authority())
        throw Error(ErrorCode::not_found, "program archive is not initialized");
    return metadata(value);
}

Files::Entry Files::verify(const std::string& value, std::stop_token stop) const
{
    cancelled(stop);
    auto result = find(value);
    if (sha256_file(path(value)) != value)
        throw std::invalid_argument("archived program content differs from its identity");
    cancelled(stop);
    return result;
}

Files::Entry
Files::preserve(const fs::path& source, const std::string& expected, std::stop_token stop)
{
    cancelled(stop);
    digest(expected);
    if (!source.is_absolute() || fs::is_symlink(source) || !fs::is_regular_file(source)
        || !fs::file_size(source) || fs::file_size(source) > max_artifact_bytes)
        throw std::invalid_argument("invalid selected execution program");
    const auto platform = artifact_platform(source);
    const auto host = current_platform();
    if (platform.os != host.os || platform.arch != host.arch || sha256_file(source) != expected)
        throw Error(
            ErrorCode::conflict,
            "loaded execution program differs from the Agent artifact");
    if (!authority()) {
        create_directories_durably(directory_ / "staging");
        replace_file_durably(directory_ / "owner.pb", owner_.SerializeAsString());
    } else
        sync_file_durably(directory_ / "owner.pb");
    cancelled(stop);
    const auto destination = path(expected);
    if (fs::exists(destination) || fs::is_symlink(destination)) {
        auto result = verify(expected, stop);
        sync_file_durably(destination);
        return result;
    }
    create_directories_durably(directory_ / "staging");
    const auto staging = directory_ / "staging" / (unique_process_id() + ".tmp");
    fs::copy_file(source, staging, fs::copy_options::none);
    fs::permissions(
        staging,
        fs::perms::owner_read | fs::perms::owner_write,
        fs::perm_options::replace);
    if (sha256_file(staging) != expected)
        throw Error(ErrorCode::conflict, "execution program changed during capture");
    cancelled(stop);
    publish_file_durably(staging, destination);
    return verify(expected, stop);
}

std::vector<Files::Entry> Files::catalogue(const std::string& after, unsigned limit) const
{
    if (!after.empty())
        digest(after);
    if (!limit || limit > 200)
        throw std::invalid_argument("program catalogue limit must be 1 to 200");
    if (!authority())
        return {};
    std::vector<std::string> ids;
    unsigned inspected = 0;
    for (const auto& entry : fs::directory_iterator(directory_)) {
        if (++inspected > 100000)
            throw Error(ErrorCode::resource_exhausted, "program archive scan exceeds capacity");
        const auto name = entry.path().filename().string();
        if (entry.is_symlink())
            throw std::invalid_argument("program archive rejects symbolic links");
        if ((name == "staging" && entry.is_directory()) || name == "owner.pb" || temporary(entry))
            continue;
        digest(name);
        if (!entry.is_regular_file())
            throw std::invalid_argument("unknown program archive entry");
        if (name > after)
            ids.push_back(name);
    }
    std::ranges::sort(ids);
    std::vector<Entry> result;
    for (const auto& id : ids) {
        if (result.size() == limit)
            break;
        result.push_back(metadata(id));
    }
    return result;
}

} // namespace asterion::programs
