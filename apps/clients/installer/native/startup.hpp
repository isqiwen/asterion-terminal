#pragma once
#include <chrono>
#include <filesystem>
#include <string>

namespace asterion::installation {

// Observe through Manager after the OS has accepted both service start requests.
// This never starts services or replays a management command.
void wait_for_local_readiness(
    const std::string& manager_endpoint,
    const std::filesystem::path& logs,
    std::chrono::milliseconds timeout = std::chrono::seconds(30));

} // namespace asterion::installation
