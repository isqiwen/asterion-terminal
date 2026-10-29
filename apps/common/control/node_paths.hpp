#pragma once
#include <filesystem>
#include <string>

namespace asterion::terminal {

// Deployment defaults carried beside the bundled native programs.
std::filesystem::path node_configuration_templates();
// Every program is installed in its own directory under one root:
//
//   <root>/<program>/bin/ lib/ config/ logs/ data/
//
// agent/ and manager/ manage the node; market/, trading/, data/, research/
// and lab/ are its services; terminal/ has the Terminal's configuration,
// logs and data, its program staying inside the application. The root is
// ASTERION_ROOT when set (isolated tests), else the user's data directory:
// asterion/ (Linux) or Asterion/ (macOS), asterion-development/ or
// "Asterion Development/" for development.
std::filesystem::path asterion_root();
std::filesystem::path program_directory(std::string_view program);
// Copies each program's configuration defaults into <root>/<program>/config
// once, under the bootstrap owner's lock; existing files are never replaced.
void install_node_configuration(
    const std::filesystem::path& root,
    const std::filesystem::path& source);
void configure_node_paths(std::filesystem::path root, std::filesystem::path enrollment);
std::filesystem::path node_enrollment_directory();
// Isolated tests run the build's program through a symbolic link at
// `installed` (<installation>/bin/<program>), so it finds that installation.
void link_node_program(const std::filesystem::path& source, const std::filesystem::path& installed);
// Development copies the build's program to `installed` when it differs; the
// program must be stopped.
void copy_node_program(const std::filesystem::path& source, const std::filesystem::path& installed);

} // namespace asterion::terminal
