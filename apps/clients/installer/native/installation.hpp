#pragma once
#include <asterion/base/serialization.hpp>
#include <filesystem>
#include <functional>

namespace asterion::installation {

// `root` holds every program's installation (see node_paths.hpp); the
// installer keeps its record and logs in root/terminal. The Terminal starts the
// local services itself, so nothing is registered with the OS but a launcher.
struct Layout
{
    std::filesystem::path source, destination, root;

    std::filesystem::path terminal() const
    {
        return root / "terminal";
    }
};

// Application files are separate from user state. The caller supplies the
// desktop operations; tests exercise publication without touching the desktop.
struct Desktop
{
    // Adds (true) or removes (false) the application launcher.
    std::function<void(bool)> launcher;
    // Erases the secrets this node remembered in the login keychain. Runs
    // before the programs and user data are removed: the helper is a program.
    std::function<void()> forget_credentials;
};

enum class Deployment
{
    install,
    upgrade,
    repair
};

std::filesystem::path native_directory(const std::filesystem::path& application);
Json inspect(const Layout& layout);
void deploy(
    const Layout& layout,
    Deployment deployment,
    const Desktop& desktop,
    const std::function<void(const char*)>& progress);
void uninstall(
    const Layout& layout,
    bool preserve_data,
    const Desktop& desktop,
    const std::function<void(const char*)>& progress);

} // namespace asterion::installation
