#include "remote_bundle.hpp"
#include <asterion/kernel/environment.hpp>
#include <vector>
#include <asterion/kernel/process/artifact.hpp>
#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace asterion::terminal {
namespace fs = std::filesystem;

namespace {

fs::path checked_resource(const std::string& arch, const std::string& name)
{
    if (arch != "x86_64")
        throw std::invalid_argument("unsupported remote Linux architecture");
    const auto configured = environment_path("ASTERION_REMOTE_RESOURCES");
    if (!configured)
        throw std::runtime_error("desktop package lacks bundled Linux service resources");
    const fs::path root(*configured);
    const auto folder = root / arch;
    if (!root.is_absolute() || fs::is_symlink(root) || fs::is_symlink(folder))
        throw std::invalid_argument("invalid bundled resource directory");
    const auto manifest = folder / "manifest.json";
    if (fs::is_symlink(manifest) || !fs::is_regular_file(manifest)
        || fs::file_size(manifest) > 65536)
        throw std::runtime_error(
            "desktop package lacks resources for the target Linux architecture; " "reinstall the complete package");
    std::ifstream input(manifest);
    const auto info = nlohmann::json::parse(input);
    if (info.size() != 5 || info.at("product_version") != ASTERION_PRODUCT_VERSION
        || info.at("source_sha256") != ASTERION_SERVICE_SOURCE_SHA256 || info.at("os") != "linux"
        || info.at("arch") != arch)
        throw std::invalid_argument("bundled Linux service version mismatch");
    std::vector<std::string> names{
        "asterion_node_agent",
        "asterion_trading",
        "asterion_market_data",
        "asterion_data_service",
        "asterion_task_service",
        "initialize-linux.py",
        "plugins/asterion_tushare.so",
        "plugins/asterion_shfe.so",
        "plugins/asterion_order_limits.so"
    };
    if (arch == "x86_64") {
        names.push_back("ctp-md.so");
        names.push_back("ctp-trader.so");
    }
    if (info.at("files").size() != names.size())
        throw std::invalid_argument("bundled Linux service version mismatch");
    for (const auto& expected : names) {
        if (!info.at("files").contains(expected))
            throw std::invalid_argument("bundled Linux service version mismatch");
    }
    // Package verification checks the whole bundle. Runtime consumers validate
    // their selected resource, without rescanning unrelated service programs.
    const auto file = folder / name;
    if (fs::is_symlink(file.parent_path()) || fs::is_symlink(file) || !fs::is_regular_file(file)
        || fs::file_size(file) > 256 * 1024 * 1024
        || sha256_file(file) != info.at("files").at(name).get<std::string>())
        throw std::invalid_argument("bundled Linux resource verification failed");
    if (name != "initialize-linux.py") {
        const auto platform = artifact_platform(file);
        if (platform.os != "linux" || platform.arch != arch)
            throw std::invalid_argument("bundled Linux program architecture mismatch");
    }
    return file;
}

} // namespace

fs::path bundled_linux_program(const std::string& arch, const std::string& program)
{
    if (program != "asterion_node_agent" && program != "asterion_trading"
        && program != "asterion_market_data" && program != "asterion_data_service"
        && program != "asterion_task_service"

        && !((program == "ctp-md.so" || program == "ctp-trader.so") && arch == "x86_64"))
        throw std::invalid_argument("invalid bundled program");
    return checked_resource(arch, program);
}

std::string bundled_linux_initializer()
{
    const auto path = checked_resource("x86_64", "initialize-linux.py");
    if (fs::file_size(path) > 65536)
        throw std::invalid_argument("initializer is too large");
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input), {}};
}

void export_bundled_initializer(const fs::path& destination)
{
    if (!destination.is_absolute() || fs::exists(destination) || fs::is_symlink(destination))
        throw std::invalid_argument(
            "choose a save path for the initializer that does not exist yet");
    fs::copy_file(checked_resource("x86_64", "initialize-linux.py"), destination);
}

} // namespace asterion::terminal
