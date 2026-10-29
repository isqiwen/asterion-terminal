#pragma once
#include <asterion/foundation/serialization.hpp>
#include <filesystem>
#include <functional>

namespace asterion::installation {

// `root` holds every program's installation (see node_paths.hpp): the
// installer copies the Agent and the Manager into root/agent/bin and
// root/manager/bin and keeps its record and logs in root/terminal.
struct Layout
{
    std::filesystem::path source, destination, root, enrollment;

    std::filesystem::path agent() const
    {
        return root / "agent";
    }

    std::filesystem::path manager() const
    {
        return root / "manager";
    }

    std::filesystem::path terminal() const
    {
        return root / "terminal";
    }
};

// Application files are separate from user state. The caller supplies OS service
// operations; tests exercise publication without touching the login service manager.
struct Services
{
    std::function<void()> unregister;
    std::function<void()> register_stopped;
    std::function<void()> prepare_programs;
    std::function<void()> start;
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
    const Services& services,
    const std::function<void(const char*)>& progress);
void uninstall(
    const Layout& layout,
    bool preserve_data,
    const Services& services,
    const std::function<void(const char*)>& progress);

} // namespace asterion::installation
