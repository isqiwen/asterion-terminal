#pragma once
#include <filesystem>
#include <string>

namespace asterion::agent {

// Caller holds the environment's exclusive Agent lock. This is persistent
// environment identity, independent of Agent process and installation versions.
std::string node_storage_identity(const std::filesystem::path& root);

} // namespace asterion::agent
