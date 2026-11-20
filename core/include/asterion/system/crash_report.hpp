#pragma once
#include <filesystem>

namespace asterion {

void write_crash_report(const std::filesystem::path& frames);

} // namespace asterion
