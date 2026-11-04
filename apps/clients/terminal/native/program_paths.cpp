#include "program_paths.hpp"
#include "application_environment.hpp"
#include <asterion/system/durable_file.hpp>
#include <asterion/system/process/child.hpp>
#include <fstream>
#include <set>

namespace asterion::terminal {
namespace fs = std::filesystem;

namespace {

fs::path environment(const char* name)
{
    return environment_path(name).value_or(fs::path{});
}

} // namespace

fs::path configuration_templates()
{
    return application_directory() / "config";
}

void install_configuration(const fs::path& root, const fs::path& source)
{
    // Lab seeds its own when it first starts.
    const auto destination = root / "terminal" / "config";
    create_directories_durably(destination);
    const auto file = destination / "logging.json";
    if (fs::exists(file) || fs::is_symlink(file))
        return;
    std::ifstream input(source / "logging.json", std::ios::binary);
    if (!input)
        throw std::runtime_error(
            "cannot read configuration default: " + (source / "logging.json").string());
    replace_file_durably(file, std::string{std::istreambuf_iterator<char>(input), {}});
}

fs::path asterion_root()
{
    auto root = environment("ASTERION_ROOT");
    if (root.empty()) {
#if defined(__APPLE__)
        root = environment("HOME") / "Library" / "Application Support" / "Asterion";
#else
        root = environment("XDG_DATA_HOME");
        if (root.empty())
            root = environment("HOME") / ".local" / "share";
        root /= "asterion";
#endif
    }
    return root;
}

fs::path program_directory(std::string_view program)
{
    return asterion_root() / program;
}

namespace {

// A module the user can change; a built-in copy is read only.
bool writable(const fs::path& path)
{
    return fs::exists(path)
        && (fs::status(path).permissions() & fs::perms::owner_write) != fs::perms::none;
}

} // namespace

void sync_builtin_research(const fs::path& builtin, const fs::path& research)
{
    for (const auto* folder : {"strategies", "factors"}) {
        create_directories_durably(research / folder);
        std::set<fs::path> shipped;
        if (fs::is_directory(builtin / folder))
            for (const auto& entry : fs::directory_iterator(builtin / folder)) {
                const auto target = research / folder / entry.path().filename();
                if (entry.path().extension() != ".py" || writable(target))
                    continue;
                shipped.insert(target);
                std::ifstream input(entry.path(), std::ios::binary);
                replace_file_durably(
                    target,
                    std::string{std::istreambuf_iterator<char>(input), {}});
                fs::permissions(
                    target,
                    fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read);
            }
        for (const auto& entry : fs::directory_iterator(research / folder))
            if (entry.path().extension() == ".py" && !shipped.contains(entry.path())
                && !writable(entry.path()))
                fs::remove(entry.path());
    }
}

} // namespace asterion::terminal
