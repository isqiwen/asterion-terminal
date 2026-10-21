#include "local_manager.hpp"
#include <asterion/foundation/error.hpp>
#include "node_paths.hpp"
#include "node_service.hpp"
#include "application_environment.hpp"
#include <asterion/manager.pb.h>
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/file_lock.hpp>
#include <asterion/kernel/trace.hpp>
#include <fstream>
#include <sstream>
#include <asterion/foundation/serialization.hpp>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

namespace asterion::terminal {
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

fs::path executable()
{
    if (auto value = environment_path("ASTERION_NODE_MANAGER_EXECUTABLE"))
        return *value;
    return current_executable().parent_path() / "asterion_node_manager";
}

std::string service_name()
{
    return development_environment() ? "me.asterion.node-manager.dev" : "me.asterion.node-manager";
}

std::vector<std::string> launch_arguments()
{
    std::vector<std::string> result{"--enrollment-directory", node_enrollment_directory().string()};
    if (development_environment())
        result.push_back("--development");
    if (environment_variable("ASTERION_NODE_DIRECTORY"))
        result.push_back("--isolated");
    return result;
}

void probe(const std::string& endpoint)
{
    auto channel = ipc::Channel::connect(endpoint, 500ms);
    manager::proto::Request request;
    request.set_version(1);
    request.set_correlation_id(next_correlation_id());
    request.mutable_snapshot();
    channel.send(request.SerializeAsString(), 500ms);
    manager::proto::Response response;
    if (!response.ParseFromString(channel.receive(500ms))
        || (!response.has_snapshot() && !response.has_error()) || response.version() != 1
        || response.correlation_id() != request.correlation_id())
        throw Error(ErrorCode::unavailable, "invalid node manager response");
}

[[noreturn]] void
startup_failure(const fs::path& logs, fs::file_time_type started, const std::exception& failure)
{
    fs::path file;
    if (fs::exists(logs))
        for (const auto& entry : fs::directory_iterator(logs)) {
            if (entry.is_regular_file()
                && entry.path().filename().string().starts_with("node-manager-")
                && entry.path().extension() == ".log" && entry.last_write_time() >= started) {
                started = entry.last_write_time();
                file = entry.path();
            }
        }
    std::string reason = failure.what();
    if (!file.empty()) {
        std::ifstream input(file, std::ios::binary | std::ios::ate);
        if (input) {
            const auto size = static_cast<std::streamoff>(input.tellg());
            input.seekg(std::max<std::streamoff>(0, size - 4096));
            std::string tail(4096, '\0');
            input.read(tail.data(), tail.size());
            tail.resize(input.gcount());
            std::istringstream lines(tail);
            for (std::string line; std::getline(lines, line);) {
                const auto record = Json::parse(line, nullptr, false);
                if (record.is_object() && record.value("event", "") == "manager.failed")
                    reason = record.at("fields").at("message").get<std::string>();
            }
        }
    }
    throw Error(
        ErrorCode::operation_failed,
        "node manager startup failed: " + reason
            + (file.empty() ? "\nLog directory: " + logs.string()
                            : "\nLog file: " + file.string()));
}

} // namespace

std::string local_manager()
{
    const auto node = local_node_directory();
    if (!node.is_absolute() || fs::is_symlink(node))
        throw std::invalid_argument("invalid local Agent directory");
    const bool bootstrap_allowed =
        development_environment() || environment_variable("ASTERION_NODE_DIRECTORY");
    if (!bootstrap_allowed) {
        std::ifstream input(node / "installation.json");
        const auto record = Json::parse(input, nullptr, false);
        if (!record.is_object() || record.value("pending", "incomplete") != "")
            throw std::runtime_error("Terminal installation is incomplete; open the installer");
    }
    create_directories_durably(node);
    const auto root = node / "manager";
    if (fs::is_symlink(root))
        throw std::invalid_argument("invalid node manager directory");
    create_directories_durably(root);
    fs::permissions(root, fs::perms::owner_all, fs::perm_options::replace);
    FileLock bootstrap(root, "bootstrap.lock");
    const auto identity_file = root / "ipc-id";
    std::string identity;
    if (fs::exists(identity_file)) {
        std::ifstream input(identity_file);
        input >> identity;
    } else {
        if (!bootstrap_allowed)
            throw std::runtime_error("Terminal installation is incomplete; open the installer");
        install_node_configuration(node, node_configuration_templates());
        identity = unique_process_id();
        replace_file_durably(identity_file, identity);
    }
    if (identity.size() != 32
        || identity.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::invalid_argument("invalid node manager identity");
    const auto sockets = fs::path("/tmp") / ("ast-manager-" + identity);
    if (::mkdir(sockets.c_str(), 0700) && errno != EEXIST)
        throw std::runtime_error("cannot create node manager socket directory");
    struct stat st{};
    if (::lstat(sockets.c_str(), &st) || !S_ISDIR(st.st_mode) || st.st_uid != ::getuid()
        || (st.st_mode & 077))
        throw std::runtime_error("node manager socket directory is not private");
    const auto endpoint = (sockets / "manager.sock").string();
    try {
        probe(endpoint);
        return endpoint;
    } catch (const std::runtime_error&) {
    }
    // Service registration carries launch context; Manager owns no generated policy file.
    {
        FileLock stopped(root, "manager.lock");
    }
    const auto arguments = launch_arguments();
    const auto started = fs::file_time_type::clock::now();
    const auto program = executable();
    if (environment_variable("ASTERION_NODE_DIRECTORY")) {
        auto command = arguments;
        command.insert(command.begin(), {"--directory", root.string(), "--endpoint", endpoint});
        ChildProcess child(program, command, true);
        child.release();
    } else {
        if (development_environment())
            install_node_service(program, root, endpoint, service_name(), arguments);
        else
            start_node_service(service_name());
    }
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    for (;;) {
        try {
            probe(endpoint);
            return endpoint;
        } catch (const std::runtime_error& error) {
            if (std::chrono::steady_clock::now() >= deadline)
                startup_failure(node / "logs", started, error);
            std::this_thread::sleep_for(50ms);
        }
    }
}

void stop_development_manager(const std::string& endpoint, std::uint64_t pid)
{
    if (!development_environment() || environment_variable("ASTERION_NODE_DIRECTORY"))
        throw Error(
            ErrorCode::permission_denied,
            "automatic shutdown requires the managed development environment");
    stop_node_service(
        executable(),
        local_node_directory() / "manager",
        endpoint,
        pid,
        service_name(),
        launch_arguments());
    remove_node_service(service_name());
}

} // namespace asterion::terminal
