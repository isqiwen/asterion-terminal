#pragma once
#include <asterion/system/environment.hpp>
#include <asterion/system/process/child.hpp>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace asterion::terminal {

// Selected by the application entry point, never by a page or connection form.
inline bool development_environment()
{
    const auto value = environment_variable("ASTERION_ENVIRONMENT");
    if (!value || *value == "production")
        return false;
    if (*value == "development")
        return true;
    throw std::invalid_argument("invalid Terminal environment");
}

// The application's native program directory: the bundled programs, plugins
// and configuration defaults the local services run from. Electron and the
// development entry name it (ASTERION_APPLICATION_DIRECTORY); otherwise it is
// the directory of the running executable.
inline std::filesystem::path application_directory()
{
    return environment_path("ASTERION_APPLICATION_DIRECTORY")
        .value_or(current_executable().parent_path());
}

} // namespace asterion::terminal
