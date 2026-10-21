#include "installation.hpp"
#include "node_paths.hpp"
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/file_lock.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/foundation/error.hpp>
#include <fstream>
#include <memory>
#include <cerrno>
#include <array>
#include <charconv>
#ifdef __linux__
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>
#else
#include <stdio.h>
#endif

namespace asterion::installation {
namespace fs = std::filesystem;

namespace {

Json read(const fs::path& path)
{
    std::ifstream input(path);
    if (!input)
        throw Error(ErrorCode::not_found, "installation file is missing: " + path.string());
    const auto value = Json::parse(input, nullptr, false);
    if (!value.is_object())
        throw Error(ErrorCode::invalid_request, "invalid installation file: " + path.string());
    return value;
}

// The product version is CMake's numeric major.minor.patch release version.
std::array<unsigned, 3> release_version(const Json& value)
{
    if (!value.is_string())
        throw Error(ErrorCode::invalid_request, "invalid installation release version");
    const auto& text = value.get_ref<const std::string&>();
    std::array<unsigned, 3> parts{};
    const auto* cursor = text.data();
    const auto* end = cursor + text.size();
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const auto [next, error] = std::from_chars(cursor, end, parts[i]);
        if (error != std::errc{} || (i == 2 ? next != end : next == end || *next != '.'))
            throw Error(ErrorCode::invalid_request, "invalid installation release version");
        cursor = i == 2 ? next : next + 1;
    }
    return parts;
}

Json product(const fs::path& application)
{
    const auto value = read(native_directory(application) / "product.json");
    if (value.value("product", "") != "asterion-terminal" || !value.contains("version")
        || !value.at("version").is_string())
        throw Error(ErrorCode::invalid_request, "invalid Terminal installation product");
    return value;
}

bool busy(const fs::path& path, const char* lock)
{
    if (!fs::exists(path / lock))
        return false;
    try {
        FileLock owner(path, lock, FileLock::Access::exclusive_existing);
        return false;
    } catch (const std::runtime_error&) {
        return true;
    }
}

struct Ownership
{
    FileLock deployment, terminal;

    explicit Ownership(const Layout& layout)
        : deployment(layout.node, "installation.lock"),
          terminal(layout.node, "terminal.lock")
    {}
};

void require_target(const Layout& layout)
{
    if (!layout.destination.is_absolute() || !layout.node.is_absolute()
        || layout.destination == layout.node || fs::is_symlink(layout.destination))
        throw Error(ErrorCode::invalid_request, "invalid installation destination");
    for (auto path = layout.node; !path.empty() && path != path.root_path();
        path = path.parent_path())
        if (path == layout.destination)
            throw Error(ErrorCode::invalid_request, "application directory contains user data");
}

Json registration(const Layout& layout)
{
    const auto record = read(layout.node / "installation.json");
    if (record.value("product", "") != "asterion-terminal"
        || record.value("directory", "") != layout.destination.string())
        throw Error(ErrorCode::conflict, "installation destination differs from its registration");
    (void)release_version(record.at("version"));
    const auto pending = record.at("pending").get<std::string>();
    if (pending != "" && pending != "install" && pending != "upgrade" && pending != "repair"
        && pending != "uninstall")
        throw Error(ErrorCode::invalid_request, "invalid unfinished installation operation");
    return record;
}

std::string deployment_action(const Json& candidate, const Json& record)
{
    const auto target = release_version(candidate.at("version"));
    if (record.is_null())
        return "install";
    const auto current = release_version(record.at("version"));
    const auto pending = record.at("pending").get<std::string>();
    if (!pending.empty()) {
        if (pending == "uninstall")
            return "";
        if (target != current)
            throw Error(
                ErrorCode::conflict,
                "resume requires the same installation package version");
        return pending;
    }
    if (target < current)
        return "";
    return target == current ? "repair" : "upgrade";
}

void identity(const fs::path& root)
{
    const auto path = root / "ipc-id";
    if (!fs::exists(path))
        replace_file_durably(path, unique_process_id());
}

void exchange(const fs::path& a, const fs::path& b)
{
#ifdef __linux__
    const auto result =
        ::syscall(SYS_renameat2, AT_FDCWD, a.c_str(), AT_FDCWD, b.c_str(), RENAME_EXCHANGE);
#else
    const auto result = ::renamex_np(a.c_str(), b.c_str(), RENAME_SWAP);
#endif
    if (result != 0)
        throw std::system_error(
            errno,
            std::generic_category(),
            "cannot publish application directory");
    sync_directory(b.parent_path());
}

} // namespace

fs::path native_directory(const fs::path& application)
{
#ifdef __APPLE__
    return application / "Contents/Resources/native";
#else
    return application / "resources/native";
#endif
}

Json inspect(const Layout& layout)
{
    require_target(layout);
    Json result{
        {"directory", layout.destination.string()},
        {"node", layout.node.string()},
        {"enrollment", layout.enrollment.string()},
        {"installed", false},
        {"version", ""},
        {"candidate_version", ""},
        {"deployment_action", ""},
        {"pending", ""},
        {"preserve_data", true},
        {"busy", Json::array()},
        {"conflict", ""}
    };
    try {
        Json record;
        if (fs::exists(layout.node / "installation.json")) {
            record = registration(layout);
            result["pending"] = record.at("pending");
            if (record.at("pending") == "uninstall")
                result["preserve_data"] = record.at("preserve_data").get<bool>();
            // Registration owns the installation even when files need repair.
            result["version"] = record.at("version");
            result["installed"] = true;
        } else if (fs::exists(layout.destination))
            result["conflict"] = "application destination is not owned by this installer";
        if (!layout.source.empty()) {
            const auto candidate = product(layout.source);
            result["candidate_version"] = candidate.at("version");
            result["deployment_action"] = deployment_action(candidate, record);
        }
    } catch (const std::exception& error) {
        result["conflict"] = error.what();
    }
    if (busy(layout.node, "terminal.lock"))
        result["busy"].push_back("terminal");
    if (busy(layout.node, "agent.lock"))
        result["busy"].push_back("agent");
    if (busy(layout.node / "manager", "manager.lock"))
        result["busy"].push_back("manager");
    return result;
}

void deploy(
    const Layout& layout,
    Deployment deployment,
    const Services& services,
    const std::function<void(const char*)>& progress)
{
    require_target(layout);
    const auto candidate = product(layout.source);
    for (auto path = layout.destination; !path.empty() && path != path.root_path();
        path = path.parent_path())
        if (path == layout.source)
            throw Error(ErrorCode::invalid_request, "installation source overlaps its destination");
    create_directories_durably(layout.node / "manager");
    Ownership ownership(layout);
    const std::string operation = deployment == Deployment::install ? "install"
        : deployment == Deployment::upgrade                         ? "upgrade"
                                                                    : "repair";
    const bool registered = fs::exists(layout.node / "installation.json");
    if (!registered && (deployment != Deployment::install || fs::exists(layout.destination)))
        throw Error(ErrorCode::conflict, "application destination is not owned by this installer");
    // Recheck under the installation lock: another installer may have completed
    // since the UI's inspection. No writes or service changes precede this decision.
    if (deployment_action(candidate, registered ? registration(layout) : Json{}) != operation)
        throw Error(
            ErrorCode::conflict,
            "installation operation is not available for this package");
    if (deployment != Deployment::install && fs::exists(layout.enrollment)
        && (deployment == Deployment::upgrade
            || fs::exists(layout.enrollment / ".maintenance.json"))) {
        bool installed_remote = false;
        for (const auto& entry : fs::directory_iterator(layout.enrollment)) {
            if (entry.path().filename().string().starts_with("."))
                continue;
            const auto file = entry.path() / "enrollment.json";
            if (fs::is_regular_file(file) && read(file).value("installed", true))
                installed_remote = true;
        }
        if (installed_remote) {
            const auto plan = read(layout.enrollment / ".maintenance.json");
            if (plan.at("action") != "prepare" || plan.at("version") != candidate.at("version"))
                throw Error(
                    ErrorCode::conflict,
                    "prepare remote nodes for this package before local deployment");
            for (const auto& node : plan.at("nodes"))
                if (node.at("phase") != "prepared")
                    throw Error(ErrorCode::conflict, "remote program preparation is incomplete");
        }
    }
    progress("checking");
    // Check process ownership before writing a pending operation or unregistering services.
    {
        FileLock agent(layout.node, "agent.lock"), manager(layout.node / "manager", "manager.lock");
    }
    Json record{
        {"product", "asterion-terminal"},
        {"version", candidate.at("version")},
        {"directory", layout.destination.string()},
        {"pending", operation}
    };
    replace_file_durably(layout.node / "installation.json", record.dump(2) + "\n");
    if (deployment != Deployment::install)
        services.unregister();
    auto agent_stopped = std::make_unique<FileLock>(layout.node, "agent.lock");
    auto manager_stopped = std::make_unique<FileLock>(layout.node / "manager", "manager.lock");
    create_directories_durably(layout.destination.parent_path());
    const auto staged =
        layout.destination.parent_path() / (".asterion-install-" + unique_process_id());

    struct Cleanup
    {
        fs::path path;

        ~Cleanup()
        {
            std::error_code ignored;
            fs::remove_all(path, ignored);
        }
    } cleanup{staged};

    progress("copying");
    fs::copy(layout.source, staged, fs::copy_options::recursive | fs::copy_options::copy_symlinks);
    progress("publishing");
    if (fs::exists(layout.destination))
        exchange(staged, layout.destination);
    else {
        fs::rename(staged, layout.destination);
        sync_directory(layout.destination.parent_path());
    }
    if (deployment == Deployment::install) {
        terminal::install_node_configuration(
            layout.node,
            native_directory(layout.destination) / "config");
        identity(layout.node);
        identity(layout.node / "manager");
    }
    progress("preparing");
    agent_stopped.reset(); // The offline Agent child acquires this lock for preparation.
    services.prepare_programs();
    agent_stopped = std::make_unique<FileLock>(layout.node, "agent.lock");
    progress("registering");
    services.register_stopped();
    record["pending"] = "";
    replace_file_durably(layout.node / "installation.json", record.dump(2) + "\n");
    if (deployment == Deployment::install || deployment == Deployment::upgrade) {
        // Publication is complete. Daemons must acquire their own process locks;
        // retain installer ownership until both service start requests finish.
        agent_stopped.reset();
        manager_stopped.reset();
        progress("starting");
        services.start();
    }
    progress("complete");
}

void uninstall(
    const Layout& layout,
    bool preserve_data,
    const Services& services,
    const std::function<void(const char*)>& progress)
{
    require_target(layout);
    Ownership ownership(layout);
    auto record = registration(layout);
    if (record.at("pending") != "" && record.at("pending") != "uninstall")
        throw Error(ErrorCode::conflict, "complete the unfinished installation operation first");
    if (record.at("pending") == "uninstall" && record.at("preserve_data") != preserve_data)
        throw Error(
            ErrorCode::conflict,
            "continue uninstall with the recorded data retention choice");
    if (!preserve_data
        && (!layout.enrollment.is_absolute() || layout.enrollment == layout.enrollment.root_path()
            || fs::is_symlink(layout.node) || fs::is_symlink(layout.node / "manager")
            || fs::is_symlink(layout.enrollment)))
        throw Error(ErrorCode::invalid_request, "invalid uninstall data directory");
    if (!preserve_data)
        for (auto path = layout.node; !path.empty() && path != path.root_path();
            path = path.parent_path())
            if (path == layout.enrollment)
                throw Error(ErrorCode::invalid_request, "invalid uninstall data directory");
    {
        FileLock agent(layout.node, "agent.lock"), manager(layout.node / "manager", "manager.lock");
    }
    if (fs::exists(layout.enrollment / ".maintenance.json"))
        throw Error(
            ErrorCode::conflict,
            "finish remote maintenance before uninstalling this machine");
    if (!preserve_data && fs::exists(layout.enrollment)) {
        for (const auto& entry : fs::directory_iterator(layout.enrollment)) {
            if (entry.path().filename().string().starts_with("."))
                continue;
            const auto registration = entry.path() / "enrollment.json";
            if (fs::is_regular_file(registration) && read(registration).value("installed", true))
                throw Error(
                    ErrorCode::conflict,
                    "uninstall remote nodes before deleting their management identities");
        }
    }
    record["pending"] = "uninstall";
    record["preserve_data"] = preserve_data;
    replace_file_durably(layout.node / "installation.json", record.dump(2) + "\n");
    progress("unregistering");
    services.unregister();
    FileLock agent_stopped(layout.node, "agent.lock");
    FileLock manager_stopped(layout.node / "manager", "manager.lock");
    progress("removing");
    fs::remove_all(layout.destination);
    if (!preserve_data) {
        progress("removingData");
        if (fs::exists(layout.enrollment)) {
            fs::remove_all(layout.enrollment);
            sync_directory(layout.enrollment.parent_path());
        }
        // Keep the locked inodes and the pending record until completion, so
        // another installer cannot acquire a newly created lock during deletion.
        // Diagnostics remain available if deletion fails and must be resumed.
        for (const auto& entry : fs::directory_iterator(layout.node)) {
            const auto name = entry.path().filename();
            if (name == "installation.lock" || name == "terminal.lock" || name == "agent.lock"
                || name == "installation.json" || name == "logs")
                continue;
            if (name == "manager") {
                for (const auto& manager_entry : fs::directory_iterator(entry.path()))
                    if (manager_entry.path().filename() != "manager.lock")
                        fs::remove_all(manager_entry.path());
                sync_directory(entry.path());
            } else
                fs::remove_all(entry.path());
        }
    }
    fs::remove(layout.node / "installation.json");
    sync_directory(layout.destination.parent_path());
    sync_directory(layout.node);
    progress("complete");
}

} // namespace asterion::installation
