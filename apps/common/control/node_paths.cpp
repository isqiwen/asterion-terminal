#include "node_paths.hpp"
#include "application_environment.hpp"
#include <asterion/system/durable_file.hpp>
#include <asterion/system/process/child.hpp>
#include <asterion/system/process/artifact.hpp>
#include <fstream>

namespace asterion::terminal {
namespace fs = std::filesystem;

namespace {

fs::path environment(const char* name)
{
    return environment_path(name).value_or(fs::path{});
}

} // namespace

fs::path node_configuration_templates()
{
    return application_directory() / "config";
}

void install_node_configuration(const fs::path& root, const fs::path& source)
{
    // The local services seed their own configuration when first started.
    const std::pair<const char*, const char*> files[] = {
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

fs::path asterion_root()
{
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

} // namespace asterion::terminal
