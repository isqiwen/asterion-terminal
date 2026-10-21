#pragma once
#include <filesystem>
#include <string>

namespace asterion::terminal {

// Deployment defaults carried beside the bundled native programs.
std::filesystem::path node_configuration_templates();
// Called only during first installation, under the bootstrap owner's lock.
// Existing operator configuration is never replaced.
void install_node_configuration(
    const std::filesystem::path& node,
    const std::filesystem::path& source);
void configure_node_paths(std::filesystem::path node, std::filesystem::path enrollment);
std::filesystem::path local_node_directory();
std::filesystem::path node_enrollment_directory();

} // namespace asterion::terminal
