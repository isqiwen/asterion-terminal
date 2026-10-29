#pragma once
#include <asterion/kernel/environment.hpp>
#include <asterion/kernel/process/child.hpp>
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
// and configuration defaults. A program started from elsewhere is given it
// (the Manager runs from <root>/manager/bin); otherwise it is the directory of
// the running executable.
inline std::filesystem::path& configured_application_directory()
{
    static std::filesystem::path directory;
    return directory;
}

inline void set_application_directory(std::filesystem::path directory)
{
    if (!directory.is_absolute())
        throw std::invalid_argument("invalid application directory: " + directory.string());
    configured_application_directory() = std::move(directory);
}

inline std::filesystem::path application_directory()
{
    const auto& configured = configured_application_directory();
    return configured.empty() ? current_executable().parent_path() : configured;
}

inline std::string local_node_service_name()
{
    return development_environment() ? "me.asterion.node-agent.dev" : "";
}

} // namespace asterion::terminal
