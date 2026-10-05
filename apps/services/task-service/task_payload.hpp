#pragma once
#include <google/protobuf/message.h>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
namespace asterion::tasks::payload {
// Format is selected by the task manifest. No automatic conversion or fallback.
inline constexpr std::uint64_t max_input_bytes = 128ULL * 1024 * 1024;
inline constexpr std::uint64_t max_result_bytes = 64ULL * 1024 * 1024;
struct Identity {
  std::string digest;
  std::uint64_t size;
};
void validate_digest(std::string_view);
Identity publish(const std::filesystem::path&, const google::protobuf::Message&,
                 std::uint64_t maximum);
// Digest-only startup validation streams the file. Reads hash the owned bytes
// that are passed to the parser, so replacing a file between opens cannot pass.
void check(const std::filesystem::path&, std::string_view digest, std::uint64_t maximum,
           std::uint64_t expected_size = 0);
std::string read(const std::filesystem::path&, std::string_view digest, std::uint64_t maximum,
                 std::uint64_t expected_size = 0);
void parse(std::string_view, google::protobuf::Message&);
} // namespace asterion::tasks::payload
