#include "service_programs.hpp"
#include "remote_bundle.hpp"
#include "plugin_catalog.hpp"
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/logger.hpp>
#include <asterion/foundation/error.hpp>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <asterion/kernel/environment.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/child.hpp>

namespace asterion::terminal {
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

std::string utf8(const fs::path& path)
{
    const auto value = path.u8string();
    return {value.begin(), value.end()};
}

// The same role-to-program mapping is used for initial deployment and program updates.
template <typename Resolve>
ServicePrograms resolve_programs(node::proto::ServiceKind kind, Resolve resolve)
{
    ServicePrograms programs;
    switch (kind) {
        case node::proto::TRADING:
            programs.executable = resolve("ASTERION_TRADING_EXECUTABLE", "asterion_trading", false);
            programs.catalog = resolve("ASTERION_CTP_CATALOG_LIBRARY", "ctp-trader", true);
            break;
        case node::proto::MARKET:
            programs.executable = resolve("ASTERION_MARKET_EXECUTABLE", "asterion_market", false);
            programs.provider = resolve("ASTERION_CTP_LIBRARY", "ctp-md", true);
            programs.catalog = resolve("ASTERION_CTP_CATALOG_LIBRARY", "ctp-trader", true);
            break;
        case node::proto::DATA:
            programs.executable =
                resolve("ASTERION_DATA_SERVICE_EXECUTABLE", "asterion_data", false);
            break;
        case node::proto::RESEARCH:
            programs.executable =
                resolve("ASTERION_RESEARCH_EXECUTABLE", "asterion_research", false);
            break;
        default:
            throw std::invalid_argument("unknown service kind");
    }
    return programs;
}

} // namespace

ServicePrograms local_service_programs(node::proto::ServiceKind kind)
{
    const auto platform = current_platform();
    const auto root = current_executable().parent_path();
    return resolve_programs(kind, [&](const char* variable, const char* name, bool library) {
        if (const auto configured = environment_path(variable))
            return *configured;
        const auto extension = library ? (platform.os == "macos" ? ".dylib" : ".so") : "";
        return root / (std::string(name) + extension);
    });
}

ServicePrograms bundled_service_programs(const std::string& arch, node::proto::ServiceKind kind)
{
    return resolve_programs(kind, [&](const char*, const char* name, bool library) {
        return bundled_linux_program(arch, std::string(name) + (library ? ".so" : ""));
    });
}

void prepare_local_programs(const fs::path& root, const fs::path& agent)
{
    node::proto::ServicePrograms manifest;
    manifest.set_version(1);
    for (const auto kind :
        {node::proto::MARKET, node::proto::DATA, node::proto::RESEARCH, node::proto::TRADING}) {
        const auto programs = local_service_programs(kind);
        auto& source = *manifest.add_services();
        source.set_kind(kind);
        source.set_executable(utf8(programs.executable));
        source.set_provider(utf8(programs.provider));
        source.set_catalog(utf8(programs.catalog));
    }
    // Electron and the development entry name the bundled interpreter; installed
    // programs find it beside their own directory. Without one, a node has no
    // lab service.
    if (const auto python = environment_path("ASTERION_PYTHON")) {
        if (!fs::is_regular_file(*python))
            throw std::runtime_error("bundled Python is missing: " + utf8(*python));
        manifest.set_python(utf8(*python));
    } else if (const auto python =
                   current_executable().parent_path().parent_path() / "python" / "bin" / "python3";
        fs::is_regular_file(python))
        manifest.set_python(utf8(python));
    for (const auto& plugin : local_plugin_catalog().entries) {
        if (plugin.managed)
            continue;
        if (plugin.availability != PluginAvailability::available)
            throw std::runtime_error("bundled bundled plugin is unavailable");
        manifest.add_bundled_plugins(utf8(plugin.artifact.path));
    }
    const auto file = root / "service-programs.pb";
    replace_file_durably(file, manifest.SerializeAsString());
    create_directories_durably(root / "logs");
    fs::path log;
    bool completed;
    int exit_code = -1;
    {
        ChildProcess preparation(
            agent,
            {"--directory", utf8(root), "--prepare-programs", utf8(file)});
        log = process_log_file(root / "logs", "agent", preparation.id());
        completed = preparation.wait(60s);
        if (completed) {
            exit_code = preparation.exit_code();
            log = process_log_file(root / "logs", "agent", preparation.id());
        }
    }
    // Read diagnostics only after the child has exited or been reaped on timeout.
    if (!completed || exit_code != 0) {
        auto reason = completed ? "Process exited with code " + std::to_string(exit_code)
                                : "Process timed out after 60 seconds";
        if (completed) {
            std::ifstream input(log, std::ios::binary | std::ios::ate);
            if (input) {
                const auto size = static_cast<std::streamoff>(input.tellg());
                input.seekg(std::max<std::streamoff>(0, size - 4096));
                std::string tail(4096, '\0');
                input.read(tail.data(), tail.size());
                tail.resize(input.gcount());
                std::istringstream lines(tail);
                std::string line;
                while (std::getline(lines, line)) {
                    const auto record = Json::parse(line, nullptr, false);
                    if (record.is_object() && record.value("event", "") == "agent.failed")
                        reason = record.at("fields").at("message").get<std::string>();
                }
            }
        }
        throw Error(
            ErrorCode::operation_failed,
            "service program synchronization failed: " + reason + "\nLog file: " + utf8(log));
    }
    fs::remove(file);
    sync_directory(root);
}

} // namespace asterion::terminal
