#include "node_paths.hpp"
#include "application_environment.hpp"
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/child.hpp>
#include <fstream>

namespace asterion::terminal {
namespace fs = std::filesystem;

namespace {

fs::path configured_node, configured_enrollment;

fs::path environment(const char* name)
{
    return environment_path(name).value_or(fs::path{});
}

} // namespace

fs::path node_configuration_templates()
{
    const auto program =
        environment_path("ASTERION_NODE_MANAGER_EXECUTABLE").value_or(current_executable());
    return program.parent_path() / "config";
}

void install_node_configuration(const fs::path& node, const fs::path& source)
{
    const auto destination = node / "config";
    create_directories_durably(destination);
    for (const auto* name : {"research.json", "logging.json"}) {
        const auto file = destination / name;
        if (fs::exists(file) || fs::is_symlink(file))
            continue;
        std::ifstream input(source / name, std::ios::binary);
        if (!input)
            throw std::runtime_error(
                "cannot read node configuration template: " + (source / name).string());
        const std::string contents{std::istreambuf_iterator<char>(input), {}};
        replace_file_durably(file, contents);
    }
}

void configure_node_paths(fs::path node, fs::path enrollment)
{
    configured_node = std::move(node);
    configured_enrollment = std::move(enrollment);
}

fs::path local_node_directory()
{
    if (!configured_node.empty())
        return configured_node;
    const bool development = development_environment();
    auto root = environment("ASTERION_NODE_DIRECTORY");
    if (root.empty()) {
#if defined(__APPLE__)
        root = environment("HOME") / "Library" / "Application Support"
            / (development ? "Asterion Development" : "Asterion") / "node";
#else
        root = environment("XDG_DATA_HOME");
        if (root.empty())
            root = environment("HOME") / ".local" / "share";
        root /= development ? "asterion-development/node" : "asterion/node";
#endif
    }
    return root;
}

std::filesystem::path node_enrollment_directory()
{
    if (!configured_enrollment.empty())
        return configured_enrollment;
    if (development_environment() || environment_path("ASTERION_NODE_DIRECTORY"))
        return local_node_directory() / "enrollments";
    const auto home = environment_path("HOME");
    if (!home)
        throw std::runtime_error("local user data directory unavailable");
    return *home / ".asterion" / "nodes";
}

} // namespace asterion::terminal
