#pragma once
#include <filesystem>
#include <optional>
#include <string>
namespace asterion {
// Environment lookups that are lossless on every platform. Windows reads the
// UTF-16 environment (std::getenv returns the ANSI code page there, which
// mangles non-ASCII user paths); POSIX returns the bytes unchanged.
// Empty values count as unset.
std::optional<std::string> environment_variable(const char* name);
std::optional<std::filesystem::path> environment_path(const char* name);
} // namespace asterion
