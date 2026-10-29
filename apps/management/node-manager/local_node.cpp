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

fs::path bundled_agent()
{
    auto executable = environment("ASTERION_NODE_AGENT_EXECUTABLE");
    if (executable.empty())
        executable = application_directory() / "asterion_node_agent";
    return executable;
}

std::optional<NodeEndpoint> local_endpoint(const fs::path& root, bool create)
{
    // Persist a random socket namespace; never trust a shared predictable socket.
    const auto identity_file = root / "data" / "ipc-id";
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
    return local_endpoint(program_directory("agent"), false);
}

void shutdown_development_node(ServiceIo& io)
{
    if (!development_environment() || environment_variable("ASTERION_ROOT"))
        throw Error(
            ErrorCode::permission_denied,
            "automatic shutdown requires the managed development environment");
    const auto root = program_directory("agent");
    const auto data = root / "data";
    if (!fs::exists(data))
        return;
    if (!root.is_absolute() || fs::is_symlink(root) || fs::is_symlink(data))
        throw std::invalid_argument("invalid local Agent directory");
    FileLock bootstrap(data, "bootstrap.lock");
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
            FileLock stopped(data, "agent.lock");
        }
        if (!fs::exists(node_service_definition(local_node_service_name())))
            return;
        verify_node_service_stopped(installed, endpoint, local_node_service_name());
    }
    if (control) {
        const auto status = control->inspect_status().get();
        if (!status.online || !status.health)
            throw Error(
                ErrorCode::unavailable,
                "development Agent status is unavailable during shutdown");
        // Agent owns service shutdown, including during initialization or recovery.
        // Ending this environment must not rewrite persisted service intentions.
        stop_node_service(installed, endpoint, status.health->pid(), local_node_service_name());
    }
    // A stopped development environment must not be launched again at login.
    // stop/verify above checks the exact owned definition before removal.
    remove_node_service(local_node_service_name());
}

NodeEndpoint local_node(ServiceIo& io)
{
    static std::mutex bootstrap;
    std::lock_guard lock(bootstrap);
    // <root>/agent: bin/asterion_node_agent, and its state in data/.
    const auto root = program_directory("agent");
    const auto data = root / "data";
    if (!root.is_absolute() || fs::is_symlink(root) || fs::is_symlink(data))
        throw std::invalid_argument("invalid local Agent directory");
    create_directories_durably(data);
    fs::permissions(root, fs::perms::owner_all);
    fs::permissions(data, fs::perms::owner_all);
    const bool managed_development =
        development_environment() && !environment_variable("ASTERION_ROOT");
    static bool development_prepared = false;
    const bool prepare = managed_development && !development_prepared;
    FileLock ownership(data, "bootstrap.lock");
    for (const auto& file : {"agent-upgrade.json", "agent-upgrade.pending"}) {
        const auto pending = data / file;
        if (fs::exists(pending) || fs::is_symlink(pending))
            throw std::runtime_error("unfinished Agent update requires explicit recovery");
    }
    const auto executable = bundled_agent();
    const auto installed = root / "bin" / "asterion_node_agent";
    const bool bootstrap_allowed =
        development_environment() || environment_variable("ASTERION_ROOT");
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
    if (environment_variable("ASTERION_ROOT")) {
        // Explicit development/test isolation never registers a login service.
        // It runs the build's Agent through a link in its installation.
        link_node_program(executable, installed);
        ChildProcess process(installed, {"--endpoint", endpoint}, true);
        process.release();
    } else if (!development_environment()) {
        start_node_service("me.asterion.node-agent");
    } else {
        if (fs::is_symlink(root / "bin"))
            throw std::invalid_argument("invalid Agent binary directory");
        if (fs::exists(installed))
            replace_node_program(executable, installed, root, sha256_file(installed));
        else
            install_node_program(executable, installed, root);
        if (prepare) {
            prepare_local_programs(installed);
            development_prepared = true;
        }
        install_node_service(installed, endpoint, local_node_service_name());
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
