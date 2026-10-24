#include "task_payload.hpp"
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/protocol/task.hpp>
#include <algorithm>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace asterion::tasks::payload {
namespace fs = std::filesystem;

namespace {

std::uint64_t size(const fs::path& path, std::uint64_t maximum)
{
    if (fs::is_symlink(path) || fs::is_symlink(path.parent_path()) || !fs::is_regular_file(path))
        throw std::invalid_argument("task payload must be a regular owned file");
    const auto count = fs::file_size(path);
    if (!count || count > maximum)
        throw std::invalid_argument("task payload exceeds size limit");
    return count;
}

} // namespace

void validate_digest(std::string_view digest)
{
    if (digest.size() != 64 || !std::ranges::all_of(digest, [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        }))
        throw std::invalid_argument("invalid task payload digest");
}

Identity publish(
    const fs::path& path,
    const google::protobuf::Message& value,
    std::uint64_t maximum,
    const std::function<void(const Identity&)>& before_publish,
    const Reference* reference)
{
    if (fs::is_symlink(path) || fs::is_symlink(path.parent_path()) || fs::exists(path))
        throw std::invalid_argument("task payload already exists or is not owned");
    const auto count = value.ByteSizeLong();
    if (!count || count > maximum)
        throw std::invalid_argument("task payload exceeds size limit");
    std::string raw;
    if (!value.SerializeToString(&raw))
        throw std::invalid_argument("cannot serialize task payload");
    Identity identity{sha256_bytes(raw), raw.size()};
    if (before_publish)
        before_publish(identity);
    // The source is chosen by the same semantic owner, never a client path.
    // Different content is a new value; a failed reference publication is not
    // silently converted into a copy or an alternate identity.
    if (reference && identity.digest == reference->identity.digest
        && identity.size == reference->identity.size) {
        check(reference->path, identity.digest, maximum, identity.size);
        fs::create_hard_link(reference->path, path);
        sync_directory(path.parent_path());
    } else {
        // One task/attempt owns this destination. Publish only complete bytes.
        replace_file_durably(path, raw);
    }
    return identity;
}

void sync_immutable(const fs::path& path)
{
    (void)size(path, max_input_bytes);
    if (fs::hard_link_count(path) == 1)
        sync_file_durably(path);
    else
        sync_directory(path.parent_path());
}

void check(
    const fs::path& path,
    std::string_view digest,
    std::uint64_t maximum,
    std::uint64_t expected_size)
{
    validate_digest(digest);
    const auto count = size(path, maximum);
    if (expected_size && count != expected_size)
        throw std::invalid_argument("task payload size mismatch");
    if (sha256_file(path) != digest)
        throw std::invalid_argument("task payload digest mismatch");
}

std::string read(
    const fs::path& path,
    std::string_view digest,
    std::uint64_t maximum,
    std::uint64_t expected_size)
{
    validate_digest(digest);
    const auto count = size(path, maximum);
    if (expected_size && count != expected_size)
        throw std::invalid_argument("task payload size mismatch");
    std::ifstream file(path, std::ios::binary);
    std::string raw(static_cast<std::size_t>(count), '\0');
    if (!file.read(raw.data(), static_cast<std::streamsize>(count))
        || file.peek() != std::char_traits<char>::eof() || file.bad())
        throw std::invalid_argument("task payload read failed");
    if (sha256_bytes(raw) != digest)
        throw std::invalid_argument("task payload digest mismatch");
    return raw;
}

void parse(std::string_view raw, google::protobuf::Message& value)
{
    if (raw.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())
        || !value.ParseFromArray(raw.data(), static_cast<int>(raw.size())))
        throw std::invalid_argument("invalid persisted Protobuf");
    protocol::validate_message(value);
}

} // namespace asterion::tasks::payload
