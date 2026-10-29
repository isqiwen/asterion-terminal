#pragma once
#include <asterion/foundation/serialization.hpp>
#include <filesystem>
#include <string>

namespace asterion::terminal {

// `root` is the program's installation: the executable is root/bin/<program>
// and the Agent's lock and update record are in root/data. Caller holds
// bootstrap.lock. Publish the first verified executable durably, or validate
// an existing identical installation without replacing it.
void install_node_program(
    const std::filesystem::path& source,
    const std::filesystem::path& installed,
    const std::filesystem::path& root);
// Development bootstrap owns the stopped Agent while replacing its executable.
// Agent must be stopped. A retained transaction is resumed only by an explicit
// call with the same source and expected installed digest.
void replace_node_program(
    const std::filesystem::path& source,
    const std::filesystem::path& installed,
    const std::filesystem::path& root,
    const std::string& expected_digest);

} // namespace asterion::terminal
