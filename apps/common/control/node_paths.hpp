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
// market/, data/, research/ and lab/ are the local services the Terminal
// runs; terminal/ has the Terminal's configuration, logs and data, its program
// staying inside the application. The root is
// ASTERION_ROOT when set (isolated tests), else the user's data directory:
// asterion/ (Linux) or Asterion/ (macOS), asterion-development/ or
// "Asterion Development/" for development.
std::filesystem::path asterion_root();
std::filesystem::path program_directory(std::string_view program);
// Copies the Terminal's logging defaults into <root>/terminal/config once;
// an existing file is never replaced.
void install_node_configuration(
    const std::filesystem::path& root,
    const std::filesystem::path& source);
// A service runs the program inside the application through a symbolic link at
// `installed` (<installation>/bin/<program>), so it finds that installation.
void link_node_program(const std::filesystem::path& source, const std::filesystem::path& installed);

} // namespace asterion::terminal
