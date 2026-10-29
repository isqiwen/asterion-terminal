#include "node_paths.hpp"
#include "application_environment.hpp"
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <fstream>

namespace asterion::terminal {
namespace fs = std::filesystem;

namespace {

fs::path configured_root, configured_enrollment;

fs::path environment(const char* name)
{
    return environment_path(name).value_or(fs::path{});
}

} // namespace

fs::path node_configuration_templates()
{
    const auto program = environment_path("ASTERION_NODE_MANAGER_EXECUTABLE");
    return (program ? program->parent_path() : application_directory()) / "config";
}

void install_node_configuration(const fs::path& root, const fs::path& source)
{
    // The Agent seeds each service's logging.json from its own and each
    // research instance's research.json from its template.
    const std::pair<const char*, const char*> files[] = {
        {"agent", "logging.json"},
        {"agent", "research.json"},
        {"manager", "logging.json"},
        {"terminal", "logging.json"},
    };
    for (const auto& [program, name] : files) {
        const auto destination = root / program / "config";
        create_directories_durably(destination);
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

void configure_node_paths(fs::path root, fs::path enrollment)
{
    configured_root = std::move(root);
    configured_enrollment = std::move(enrollment);
}

fs::path asterion_root()
{
    if (!configured_root.empty())
        return configured_root;
    const bool development = development_environment();
    auto root = environment("ASTERION_ROOT");
    if (root.empty()) {
#if defined(__APPLE__)
        root = environment("HOME") / "Library" / "Application Support"
            / (development ? "Asterion Development" : "Asterion");
#else
        root = environment("XDG_DATA_HOME");
        if (root.empty())
            root = environment("HOME") / ".local" / "share";
        root /= development ? "asterion-development" : "asterion";
#endif
    }
    return root;
}

fs::path program_directory(std::string_view program)
{
    return asterion_root() / program;
}

std::filesystem::path node_enrollment_directory()
{
    if (!configured_enrollment.empty())
        return configured_enrollment;
    if (development_environment() || environment_path("ASTERION_ROOT"))
        return program_directory("manager") / "data" / "enrollments";
    const auto home = environment_path("HOME");
    if (!home)
        throw std::runtime_error("local user data directory unavailable");
    return *home / ".asterion" / "nodes";
}

void link_node_program(const fs::path& source, const fs::path& installed)
{
    if (!source.is_absolute() || !installed.is_absolute() || source == installed)
        throw std::invalid_argument("invalid program link");
    create_directories_durably(installed.parent_path());
    if (fs::is_symlink(installed)) {
        if (fs::read_symlink(installed) == source)
            return;
    } else if (fs::exists(installed))
        throw std::runtime_error("an installed program is in the way: " + installed.string());
    const auto temporary = installed.parent_path() / ("link-" + unique_process_id());
    fs::create_symlink(source, temporary);
    fs::rename(temporary, installed);
    sync_directory(installed.parent_path());
}

void copy_node_program(const fs::path& source, const fs::path& installed)
{
    if (!source.is_absolute() || !installed.is_absolute() || source == installed
        || fs::is_symlink(installed) || !fs::is_regular_file(source))
        throw std::invalid_argument("invalid program copy");
    create_directories_durably(installed.parent_path());
    if (fs::exists(installed) && sha256_file(installed) == sha256_file(source))
        return;
    const auto temporary = installed.parent_path() / ("copy-" + unique_process_id());
    fs::copy_file(source, temporary);
    fs::permissions(temporary, fs::status(source).permissions());
    publish_file_durably(temporary, installed);
}

} // namespace asterion::terminal
