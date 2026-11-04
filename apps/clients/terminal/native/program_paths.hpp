#pragma once
#include <filesystem>
#include <string>

namespace asterion::terminal {

// Configuration defaults carried beside the bundled native programs.
std::filesystem::path configuration_templates();
// Everything the Terminal keeps on this machine is under one root:
//
//   <root>/terminal/  the Terminal's configuration, logs and data
//   <root>/lab/       Lab's configuration, logs and data
//   <root>/research/  the strategy directory
//
// The root is ASTERION_ROOT when set (isolated tests), else the user's data
// directory: asterion/ (Linux) or Asterion/ (macOS).
std::filesystem::path asterion_root();
std::filesystem::path program_directory(std::string_view program);
// Copies the Terminal's logging defaults into <root>/terminal/config once;
// an existing file is never replaced.
void install_configuration(const std::filesystem::path& root, const std::filesystem::path& source);
// Copies the built-in strategies and factors of `builtin` read only into the
// strategy directory: a new version replaces them and a module no longer
// shipped goes. A module of the user's with the same name stays.
void sync_builtin_research(
    const std::filesystem::path& builtin,
    const std::filesystem::path& research);

} // namespace asterion::terminal
