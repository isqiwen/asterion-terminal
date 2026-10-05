#pragma once
#include <filesystem>
#include <optional>
#include <string>
namespace asterion {
// Environment lookups; values are returned as their bytes. Empty values count
// as unset.
std::optional<std::string> environment_variable(const char* name);
std::optional<std::filesystem::path> environment_path(const char* name);
} // namespace asterion
