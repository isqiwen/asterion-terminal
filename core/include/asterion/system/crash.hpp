#pragma once
#include <filesystem>
#include <string_view>

namespace asterion {

// Linux service entry points install this before starting worker threads.
void install_crash_logging(const std::filesystem::path& directory, std::string_view program);

} // namespace asterion
