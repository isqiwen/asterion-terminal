#pragma once
#include <google/protobuf/message.h>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <functional>

namespace asterion::tasks::payload {

// Format is selected by the task manifest. No automatic conversion or fallback.
inline constexpr std::uint64_t max_input_bytes = 128ULL * 1024 * 1024;
inline constexpr std::uint64_t max_result_bytes = 64ULL * 1024 * 1024;

struct Identity
{
    std::string digest;
    std::uint64_t size;
};

struct Reference
{
    std::filesystem::path path;
    Identity identity;
};

void validate_digest(std::string_view);
Identity publish(
    const std::filesystem::path&,
    const google::protobuf::Message&,
    std::uint64_t maximum,
    const std::function<void(const Identity&)>& before_publish = {},
    const Reference* reference = nullptr);
// Linked input bytes were already durably published by the same owner. Recovery
// verifies the content first, then only needs to finish its namespace publication.
void sync_immutable(const std::filesystem::path&);
// Digest-only startup validation streams the file. Reads hash the owned bytes
// that are passed to the parser, so replacing a file between opens cannot pass.
void check(
    const std::filesystem::path&,
    std::string_view digest,
    std::uint64_t maximum,
    std::uint64_t expected_size = 0);
// Reads bytes this owner published. Verifying the digest belongs to trust
// boundaries such as cold backup inspection, not to ordinary reads.
std::string
load(const std::filesystem::path&, std::uint64_t maximum, std::uint64_t expected_size = 0);
std::string read(
    const std::filesystem::path&,
    std::string_view digest,
    std::uint64_t maximum,
    std::uint64_t expected_size = 0);
void parse(std::string_view, google::protobuf::Message&);

} // namespace asterion::tasks::payload
