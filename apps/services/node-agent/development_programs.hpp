#pragma once
#include <filesystem>
namespace asterion::agent {
// Offline only. Holds agent.lock and preserves service intentions and all ledgers.
void prepare_development_programs(const std::filesystem::path& root,
                                  const std::filesystem::path& manifest);
} // namespace asterion::agent
