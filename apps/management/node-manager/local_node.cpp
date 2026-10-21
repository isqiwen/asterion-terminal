#include "agent_client.hpp"
#include "node_paths.hpp"
#include "application_environment.hpp"
#include "node_program.hpp"
#include "node_service.hpp"
#include "service_programs.hpp"
#include <asterion/kernel/environment.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <asterion/foundation/error.hpp>
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/file_lock.hpp>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace asterion::terminal {
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

fs::path environment(const char* name)
{
    return environment_path(name).value_or(fs::path{});
}

std::string utf8(const fs::path& p)
{
    const auto s = p.u8string();
    return {s.begin(), s.end()};
}

fs::path bundled_agent()
{
    auto executable = environment("ASTERION_NODE_AGENT_EXECUTABLE");
    if (executable.empty())
        executable = current_executable().parent_path() / "asterion_node_agent";
    return executable;
}

std::optional<NodeEndpoint> local_endpoint(const fs::path& root, bool create)
{
    // Persist a random socket namespace; never trust a shared predictable socket.
    const auto identity_file = root / "ipc-id";
    if (fs::is_symlink(identity_file))
        throw std::invalid_argument("invalid Agent identity file");
    std::string identity;
    if (fs::exists(identity_file)) {
        if (!fs::is_regular_file(identity_file) || fs::file_size(identity_file) > 64)
            throw std::runtime_error("invalid Agent identity file");
        std::ifstream input(identity_file);
        input >> identity;
    } else {
        if (!create)
            return std::nullopt;
        identity = unique_process_id();
        replace_file_durably(identity_file, identity);
    }
    if (identity.size() != 32
        || identity.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw std::invalid_argument("invalid Agent identity");
    const auto sockets = fs::path("/tmp") / ("ast-node-" + identity);
    if (create && ::mkdir(sockets.c_str(), 0700) != 0 && errno != EEXIST)
        throw std::runtime_error("cannot create Agent socket directory");
    struct stat st{};
    if ((create || fs::exists(sockets))
        && (::lstat(sockets.c_str(), &st) || !S_ISDIR(st.st_mode) || st.st_uid != ::getuid()
            || (st.st_mode & 077) != 0))
        throw std::runtime_error("Agent socket directory is not private");
    const auto endpoint = (sockets / "node.sock").string();
    return NodeEndpoint{"local", "localhost", 0, {}, endpoint};
}

} // namespace

std::optional<NodeEndpoint> enrolled_local_node()
{
    return local_endpoint(local_node_directory(), false);
}

void shutdown_development_node(ServiceIo& io)
{
    if (!development_environment() || environment_variable("ASTERION_NODE_DIRECTORY"))
        throw Error(
            ErrorCode::permission_denied,
            "automatic shutdown requires the managed development environment");
    const auto root = local_node_directory();
    if (!fs::exists(root))
        return;
    if (!root.is_absolute() || fs::is_symlink(root))
        throw std::invalid_argument("invalid local Agent directory");
    FileLock bootstrap(root, "bootstrap.lock");
    const auto enrolled = enrolled_local_node();
    if (!enrolled)
        return;
    const auto endpoint = enrolled->endpoint;
    const auto installed = root / "bin" / "asterion_node_agent";
    std::shared_ptr<AgentClient> control;
    try {
        control = AgentClient::open(io, NodeEndpoint{"local", "localhost", 0, {}, endpoint}).get();
    } catch (const Error&) {
        // A disconnected socket does not prove the Agent stopped.
        {
            FileLock stopped(root, "agent.lock");
        }
        if (!fs::exists(node_service_definition(local_node_service_name())))
            return;
        verify_node_service_stopped(installed, root, endpoint, local_node_service_name());
    }
    if (control) {
        const auto status = control->inspect_status().get();
        if (!status.online || !status.health)
            throw Error(
                ErrorCode::unavailable,
                "development Agent status is unavailable during shutdown");
        // Agent owns service shutdown, including during initialization or recovery.
        // Ending this environment must not rewrite persisted service intentions.
        stop_node_service(
            installed,
            root,
            endpoint,
            status.health->pid(),
            local_node_service_name());
    }
    // A stopped development environment must not be launched again at login.
    // stop/verify above checks the exact owned definition before removal.
    remove_node_service(local_node_service_name());
}

NodeEndpoint local_node(ServiceIo& io)
{
    static std::mutex bootstrap;
    std::lock_guard lock(bootstrap);
    const auto root = local_node_directory();
    if (!root.is_absolute() || fs::is_symlink(root))
        throw std::invalid_argument("invalid local Agent directory");
    create_directories_durably(root);
    fs::permissions(root, fs::perms::owner_all);
    const bool managed_development =
        development_environment() && !environment_variable("ASTERION_NODE_DIRECTORY");
    static bool development_prepared = false;
    const bool prepare = managed_development && !development_prepared;
    FileLock ownership(root, "bootstrap.lock");
    for (const auto& file : {"agent-upgrade.json", "agent-upgrade.pending"}) {
        const auto pending = root / file;
        if (fs::exists(pending) || fs::is_symlink(pending))
            throw std::runtime_error("unfinished Agent update requires explicit recovery");
    }
    const auto executable = bundled_agent();
    const bool bootstrap_allowed =
        development_environment() || environment_variable("ASTERION_NODE_DIRECTORY");
    const auto enrolled = local_endpoint(root, bootstrap_allowed);
    if (!enrolled)
        throw std::runtime_error("Terminal installation is incomplete; open the installer");
    const auto config = *enrolled;
    const auto& endpoint = config.endpoint;
    try {
        auto probe = AgentClient::open(io, config).get();
        return config;
    } catch (const Error&) {
    }
    if (prepare) {
        prepare_local_programs(root, executable);
        const auto installed = root / "bin" / executable.filename();
        if (fs::exists(installed))
            replace_node_program(executable, installed, root, sha256_file(installed));
        development_prepared = true;
    }
    if (environment_variable("ASTERION_NODE_DIRECTORY")) {
        // Explicit development/test isolation never registers a login service.
        ChildProcess process(executable, {"--directory", utf8(root), "--endpoint", endpoint}, true);
        process.release();
    } else if (!development_environment()) {
        start_node_service("me.asterion.node-agent");
    } else {
        const auto bin = root / "bin";
        if (fs::is_symlink(bin))
            throw std::invalid_argument("invalid Agent binary directory");
        const auto installed = bin / executable.filename();
        install_node_program(executable, installed, root);
        install_node_service(installed, root, endpoint, local_node_service_name());
    }
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    for (;;) {
        try {
            auto probe = AgentClient::open(io, config).get();
            return config;
        } catch (const Error&) {
            if (std::chrono::steady_clock::now() >= deadline)
                throw;
            std::this_thread::sleep_for(50ms);
        }
    }
}

} // namespace asterion::terminal
