#pragma once
#include <asterion/system/environment.hpp>
#include <asterion/system/process/child.hpp>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace asterion::terminal {

// The application's native program directory: the bundled programs, plugins
// and configuration defaults Lab runs from. Electron and the
// development entry name it (ASTERION_APPLICATION_DIRECTORY); otherwise it is
// the directory of the running executable.
inline std::filesystem::path application_directory()
{
    return environment_path("ASTERION_APPLICATION_DIRECTORY")
        .value_or(current_executable().parent_path());
}

} // namespace asterion::terminal
