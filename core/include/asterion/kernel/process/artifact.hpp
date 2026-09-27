#pragma once
#include <filesystem>
#include <string>
#include <string_view>
namespace asterion {
struct HostPlatform {
  std::string os, arch;
};
HostPlatform current_platform();
HostPlatform artifact_platform(const std::filesystem::path& path);
std::string sha256_bytes(std::string_view bytes);
std::string sha256_file(const std::filesystem::path& path);
} // namespace asterion
