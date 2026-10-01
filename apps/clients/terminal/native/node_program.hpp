#pragma once
#include <asterion/foundation/serialization.hpp>
#include <filesystem>
#include <string>
namespace asterion::terminal {
// Read-only: never creates files, starts a service or hashes on a UI timer.
Json inspect_node_program(const std::filesystem::path& source,
                          const std::filesystem::path& installed,
                          const std::filesystem::path& root);
// Caller holds bootstrap.lock across OS stop, replacement and restart.
// Agent must be stopped. A retained transaction is resumed only by an explicit
// call with the same source and expected installed digest.
void replace_node_program(const std::filesystem::path& source,
                          const std::filesystem::path& installed, const std::filesystem::path& root,
                          const std::string& expected_digest);
// Explicit local OS-managed upgrade. Requires all business services stopped.
// The optional registration name is for isolated native acceptance fixtures.
void upgrade_node_service(const std::filesystem::path& source,
                          const std::filesystem::path& installed, const std::filesystem::path& root,
                          const std::string& endpoint, const std::string& expected_digest,
                          const std::string& name = {});
} // namespace asterion::terminal
