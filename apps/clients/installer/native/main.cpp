#include "installation.hpp"
#include "ctp_connections.hpp"
#include "data_credentials.hpp"
#include "node_paths.hpp"
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/environment.hpp>
#include <asterion/kernel/log.hpp>
#include <asterion/kernel/process/owner.hpp>
#include <asterion/foundation/error.hpp>
#include <CLI/CLI.hpp>
#include <iostream>

using namespace asterion;
using namespace asterion::terminal;
namespace fs = std::filesystem;

namespace {

void launcher(const installation::Layout& layout, bool add)
{
#ifndef __APPLE__
    const auto home = environment_path("HOME");
    if (!home)
        throw Error(ErrorCode::not_found, "local user data directory unavailable");
    const auto file = *home / ".local/share/applications/asterion-terminal.desktop";
    if (!add) {
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
#else
    (void)layout;
    (void)add;
#endif
}

} // namespace

int main(int argc, char** argv)
{
    CLI::App app{"Internal Terminal installation executor"};
    std::string operation, source, destination;
    bool remove_user_data = false;
    app.add_option("--operation", operation)
        ->required()
        ->check(CLI::IsMember({"inspect", "install", "upgrade", "repair", "uninstall"}));
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
        const installation::Layout layout{source, destination, asterion_root()};
        if (operation == "inspect") {
            std::cout << Json{{"result", installation::inspect(layout)}}.dump() << '\n';
            return 0;
        }
        // The installer runs from the application; it logs with the Terminal.
        create_directories_durably(layout.terminal() / "data");
        log::init(layout.terminal(), "installer");
        log_file = log::file(layout.terminal(), "installer", current_process_id());
        const auto native = installation::native_directory(layout.destination);
        const installation::Desktop desktop{
            [&](bool add) {
                launcher(layout, add);
            },
            [&] {
                // Keychain entries are named after these directories, as the
                // Terminal opened them. No helper or keyring means none exist.
                const auto keychain = keychain_store(native / "asterion_keychain");
                if (!keychain)
                    return;
                DataCredentials(layout.terminal() / "data" / "data-providers", keychain)
                    .forget_remembered();
                CtpConnections(layout.terminal() / "data" / "ctp-connections", keychain)
                    .forget_remembered();
            }
        };
        auto progress = [&](const char* step) {
            log::info("installer step: {}", step);
            std::cout << Json{{"step", step}}.dump() << std::endl;
        };
        if (operation == "install" || operation == "upgrade" || operation == "repair")
            installation::deploy(
                layout,
                operation == "install"       ? installation::Deployment::install
                    : operation == "upgrade" ? installation::Deployment::upgrade
                                             : installation::Deployment::repair,
                desktop,
                progress);
        else
            installation::uninstall(layout, !remove_user_data, desktop, progress);
        std::cout << Json{{"result", installation::inspect(layout)}}.dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        const auto* typed = dynamic_cast<const Error*>(&error);
        const std::string code =
            typed ? std::string(error_name(typed->code())) : "operation_failed";
        log::error("installer failed: {}", error.what());
        std::cout << Json{
            {"error", {{"code", code}, {"message", error.what()}, {"log_file", log_file.string()}}}
        }.dump() << '\n';
        return 1;
    }
}
