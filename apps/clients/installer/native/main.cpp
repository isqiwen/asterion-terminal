#include "installation.hpp"
#include "startup.hpp"
#include "service_programs.hpp"
#include "node_paths.hpp"
#include "node_service.hpp"
#include <asterion/manager.pb.h>
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/environment.hpp>
#include <asterion/kernel/logger.hpp>
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/process/file_lock.hpp>
#include <asterion/kernel/process/owner.hpp>
#include <asterion/foundation/error.hpp>
#include <CLI/CLI.hpp>
#include <fstream>
#include <thread>
#include <future>
#include <google/protobuf/util/json_util.h>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

using namespace asterion;
using namespace asterion::terminal;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

std::string endpoint(const fs::path& directory, bool manager)
{
    std::ifstream input(directory / "ipc-id");
    std::string identity;
    input >> identity;
    if (identity.size() != 32
        || identity.find_first_not_of("0123456789abcdef") != std::string::npos)
        throw Error(
            ErrorCode::invalid_request,
            "invalid installed node identity: " + directory.string());
    const auto root = fs::path("/tmp") / ((manager ? "ast-manager-" : "ast-node-") + identity);
    create_directories_durably(root);
    struct stat status{};
    if (::lstat(root.c_str(), &status) || !S_ISDIR(status.st_mode) || status.st_uid != ::getuid())
        throw Error(ErrorCode::permission_denied, "installed socket directory is not private");
    fs::permissions(root, fs::perms::owner_all, fs::perm_options::replace);
    return (root / (manager ? "manager.sock" : "node.sock")).string();
}

void launcher(const installation::Layout& layout, bool remove)
{
#ifndef __APPLE__
    const auto home = environment_path("HOME");
    if (!home)
        throw Error(ErrorCode::not_found, "local user data directory unavailable");
    const auto file = *home / ".local/share/applications/asterion-terminal.desktop";
    if (remove) {
        if (fs::exists(file)) {
            fs::remove(file);
            sync_directory(file.parent_path());
        }
        return;
    }
    std::string executable;
    for (const char c : (layout.destination / "asterion_terminal").string()) {
        if (c == '\n' || c == '\r')
            throw Error(ErrorCode::invalid_request, "invalid installation destination");
        if (c == '\\' || c == '"' || c == '$' || c == '`')
            executable += "\\";
        if (c == '%')
            executable += '%';
        executable += c;
    }
    create_directories_durably(file.parent_path());
    replace_file_durably(
        file,
        "[Desktop Entry]\nType=Application\nName=Asterion Terminal\nExec=\"" + executable
            + "\"\nTerminal=false\nCategories=Office;Finance;\n");
#endif
}

manager::proto::Snapshot snapshot(const fs::path& root)
{
    auto channel = ipc::Channel::connect(endpoint(root / "manager", true), 2s);
    manager::proto::Request request;
    request.set_version(1);
    request.set_correlation_id("installer-stop");
    request.mutable_snapshot();
    channel.send(request.SerializeAsString(), 2s);
    manager::proto::Response response;
    if (!response.ParseFromString(channel.receive(2s)) || !response.has_snapshot()
        || response.version() != 1 || response.correlation_id() != request.correlation_id())
        throw Error(
            ErrorCode::unavailable,
            "cannot inspect running Manager; stop services before maintenance");
    return response.snapshot();
}

Json remote(const installation::Layout& layout)
{
    const auto payload = Json::parse(std::cin);
    manager::proto::Request request;
    const auto parsed = google::protobuf::util::JsonStringToMessage(payload.dump(), &request);
    if (!parsed.ok() || !request.has_maintenance())
        throw Error(ErrorCode::invalid_request, "invalid installer remote request");
    request.set_version(1);
    request.set_correlation_id("installer-remote");
    auto* operation = request.mutable_maintenance();
    const auto application = layout.source.empty() ? layout.destination : layout.source;
    std::ifstream product(installation::native_directory(application) / "product.json");
    const auto descriptor = Json::parse(product);
    operation->set_target_version(descriptor.at("version").get<std::string>());
    operation->set_resources(
        (installation::native_directory(application).parent_path() / "remote-linux").string());
    std::unique_ptr<FileLock> terminal_stopped;
    if (request.maintenance().action() == manager::proto::Maintenance::PREPARE)
        terminal_stopped = std::make_unique<FileLock>(layout.node, "terminal.lock");
    const auto socket = endpoint(layout.node / "manager", true);
    start_node_service("me.asterion.node-manager");
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    std::optional<ipc::Channel> channel;
    while (!channel) {
        try {
            channel.emplace(ipc::Channel::connect(socket, 2s));
        } catch (const std::exception&) {
            if (std::chrono::steady_clock::now() >= deadline)
                throw;
            std::this_thread::sleep_for(100ms);
        }
    }
    if (terminal_stopped) {
        const auto state = snapshot(layout.node);
        bool observed_local = false;
        for (const auto& node : state.nodes()) {
            if (node.id() != "local")
                continue;
            observed_local = true;
            if (!node.online()) {
                verify_node_service_stopped(
                    installation::native_directory(layout.destination) / "asterion_node_agent",
                    layout.node,
                    endpoint(layout.node, false));
                continue;
            }
            if (node.health().phase() != node::proto::Status::READY)
                throw Error(
                    ErrorCode::unavailable,
                    "local Agent readiness is unknown; inspect node logs");
            for (const auto& service : node.health().services())
                if (service.pid() || service.desired_running())
                    throw Error(
                        ErrorCode::conflict,
                        "stop local business services before remote maintenance");
        }
        if (!observed_local) {
            FileLock stopped(layout.node, "agent.lock");
        }
    }
    channel->send(request.SerializeAsString(), 5s);
    auto reply = std::async(std::launch::async, [&] {
        return channel->receive(30min);
    });
    while (reply.wait_for(2s) != std::future_status::ready) {
        // Read progress on a separate stream; never restart Manager or replay a mutation.
        try {
            auto monitor = ipc::Channel::connect(socket, 1s);
            manager::proto::Request poll;
            poll.set_version(1);
            poll.set_correlation_id("installer-progress");
            poll.mutable_maintenance()->set_action(manager::proto::Maintenance::INSPECT);
            monitor.send(poll.SerializeAsString(), 1s);
            manager::proto::Response state;
            if (state.ParseFromString(monitor.receive(2s)) && state.has_presentation())
                std::cout << Json{{"remote_progress", Json::parse(state.presentation())}}.dump()
                          << std::endl;
        } catch (const std::exception&) {
            // A lost progress sample does not determine the running mutation's result.
        }
    }
    manager::proto::Response response;
    if (!response.ParseFromString(reply.get()) || response.version() != 1
        || response.correlation_id() != request.correlation_id())
        throw Error(ErrorCode::unavailable, "invalid remote maintenance response");
    if (response.has_error())
        throw Error(
            parse_error_code(response.error().code()).value_or(ErrorCode::operation_failed),
            response.error().message());
    if (response.has_presentation())
        return Json::parse(response.presentation());
    return Json{{"accepted", true}};
}

} // namespace

int main(int argc, char** argv)
{
    CLI::App app{"Internal Terminal installation executor"};
    std::string operation, source, destination;
    bool remove_user_data = false;
    app.add_option("--operation", operation)
        ->required()
        ->check(
            CLI::IsMember(
                {"inspect", "install", "upgrade", "repair", "uninstall", "stop", "remote"}));
    app.add_option("--source", source);
    app.add_option("--destination", destination)->required();
    app.add_flag("--remove-user-data", remove_user_data);
    CLI11_PARSE(app, argc, argv);
    fs::path log_file;
    try {
        if (remove_user_data && operation != "uninstall")
            throw Error(
                ErrorCode::invalid_request,
                "user data removal is only available during uninstall");
        ::setenv("ASTERION_ENVIRONMENT", "production", 1);
        const installation::Layout
            layout{source, destination, local_node_directory(), node_enrollment_directory()};
        if (operation == "inspect") {
            std::cout << Json{{"result", installation::inspect(layout)}}.dump() << '\n';
            return 0;
        }
        create_directories_durably(layout.node / "manager");
        ::setenv("ASTERION_LOG_DIRECTORY", (layout.node / "logs").c_str(), 1);
        initialize_process_logging("installer");
        log_file = process_log_file(layout.node / "logs", "installer", current_process_id());
        if (operation == "remote") {
            std::cout << Json{{"result", remote(layout)}}.dump() << '\n';
            return 0;
        }
        const auto native = installation::native_directory(layout.destination);
        const std::vector<std::string> arguments{
            "--enrollment-directory",
            layout.enrollment.string()
        };
        auto each_service = [&](auto action) {
            action(
                native / "asterion_node_agent",
                layout.node,
                "me.asterion.node-agent",
                std::vector<std::string>{});
            action(
                native / "asterion_node_manager",
                layout.node / "manager",
                "me.asterion.node-manager",
                arguments);
        };
        const installation::Services services{
            [&] {
                each_service(
                    [&](const auto& program, const auto& root, const auto& name, const auto& args) {
                        if (fs::exists(node_service_definition(name))) {
                            verify_node_service_stopped(
                                program,
                                root,
                                endpoint(root, name == std::string("me.asterion.node-manager")),
                                name,
                                args);
                            remove_node_service(name);
                        }
                    });
                launcher(layout, true);
            },
            [&] {
                each_service(
                    [&](const auto& program, const auto& root, const auto& name, const auto& args) {
                        install_node_service(
                            program,
                            root,
                            endpoint(root, name == std::string("me.asterion.node-manager")),
                            name,
                            args,
                            false);
                    });
                launcher(layout, false);
            },
            [&] {
                prepare_local_programs(layout.node, native / "asterion_node_agent");
            },
            [&] {
                start_node_service("me.asterion.node-agent");
                start_node_service("me.asterion.node-manager");
                installation::wait_for_local_readiness(
                    endpoint(layout.node / "manager", true),
                    layout.node / "logs");
            }
        };
        auto progress = [&](const char* step) {
            log_process_event("deployment", LogLevel::info, step);
            std::cout << Json{{"step", step}}.dump() << std::endl;
        };
        if (operation == "install" || operation == "upgrade" || operation == "repair")
            installation::deploy(
                layout,
                operation == "install"       ? installation::Deployment::install
                    : operation == "upgrade" ? installation::Deployment::upgrade
                                             : installation::Deployment::repair,
                services,
                progress);
        else if (operation == "uninstall")
            installation::uninstall(layout, !remove_user_data, services, progress);
        else {
            // The installed executor uses its own Manager protocol. Never send
            // commands directly to Agents or stop a service that still owns work.
            FileLock terminal_stopped(layout.node, "terminal.lock");
            const auto state = snapshot(layout.node);
            std::uint64_t agent_pid = 0;
            for (const auto& node : state.nodes()) {
                if (node.id() != "local")
                    continue;
                if (!node.online()) {
                    verify_node_service_stopped(
                        native / "asterion_node_agent",
                        layout.node,
                        endpoint(layout.node, false));
                    continue;
                }
                if (node.health().phase() != node::proto::Status::READY)
                    throw Error(
                        ErrorCode::unavailable,
                        "local Agent readiness is unknown; inspect node logs");
                for (const auto& service : node.health().services())
                    if (service.pid() || service.desired_running())
                        throw Error(
                            ErrorCode::conflict,
                            "stop business services in Terminal before maintenance: "
                                + service.id());
                agent_pid = node.health().pid();
            }
            if (agent_pid)
                stop_node_service(
                    native / "asterion_node_agent",
                    layout.node,
                    endpoint(layout.node, false),
                    agent_pid);
            else {
                FileLock agent_stopped(layout.node, "agent.lock");
            }
            stop_node_service(
                native / "asterion_node_manager",
                layout.node / "manager",
                endpoint(layout.node / "manager", true),
                state.pid(),
                "me.asterion.node-manager",
                arguments);
        }
        std::cout << Json{{"result", installation::inspect(layout)}}.dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        const auto* typed = dynamic_cast<const Error*>(&error);
        const std::string code =
            typed ? std::string(error_name(typed->code())) : "operation_failed";
        log_process_event(
            "deployment",
            LogLevel::error,
            "installer.failed",
            {{"message", error.what()}});
        std::cout << Json{
            {"error", {{"code", code}, {"message", error.what()}, {"log_file", log_file.string()}}}
        }.dump() << '\n';
        return 1;
    }
}
