#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
namespace asterion {
// Largest program the Agent accepts. Sanitizer-instrumented test builds embed
// far more code, so only they allow more; release builds keep 128 MiB.
#ifdef ASTERION_SANITIZED
inline constexpr std::uintmax_t max_artifact_bytes = 512ULL * 1024 * 1024;
#else
inline constexpr std::uintmax_t max_artifact_bytes = 128ULL * 1024 * 1024;
#endif
struct HostPlatform {
  std::string os, arch;
};
HostPlatform current_platform();
HostPlatform artifact_platform(const std::filesystem::path& path);
std::string sha256_bytes(std::string_view bytes);
std::string sha256_file(const std::filesystem::path& path);
} // namespace asterion
