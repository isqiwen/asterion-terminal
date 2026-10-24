#pragma once
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>

namespace asterion::provider_programs {

// Exact loaded provider bytes, shared by historical and document acquisition.
// Caller serializes publication within Data; source lookup is unnecessary when
// an exact retained module already exists. Never loads or executes the copy.
void preserve(
    const std::string& digest,
    const std::filesystem::path& directory,
    const std::function<std::filesystem::path()>& source_path,
    std::stop_token = {});

} // namespace asterion::provider_programs
