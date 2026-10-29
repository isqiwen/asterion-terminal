#include "installation.hpp"
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/file_lock.hpp>
#include <gtest/gtest.h>
#include <fstream>

namespace asterion::installation {
namespace fs = std::filesystem;

namespace {

std::string contents(const fs::path& path)
{
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input), {}};
}

class Installation : public testing::Test
{
protected:
    fs::path root = fs::temp_directory_path() / ("asterion-installer-test-" + unique_process_id());
    Layout layout{
        root / "payload",
        root / "applications/terminal",
        root / "node",
        root / "enrollments"
    };
    int registered = 0, removed = 0, prepared = 0, started = 0, forgotten = 0;
    Services services{
        [&] {
            ++removed;
        },
        [&] {
            ++registered;
        },
        [&] {
            ++prepared;
        },
        [&] {
            FileLock agent(layout.node, "agent.lock"),
                manager(layout.node / "manager", "manager.lock");
            EXPECT_TRUE(inspect(layout).at("installed"));
            EXPECT_EQ(inspect(layout).at("pending"), "");
            EXPECT_GT(registered, 0);
            EXPECT_GT(prepared, 0);
            ++started;
        },
        [&] {
            // The keychain helper is one of the programs.
            EXPECT_TRUE(fs::exists(layout.destination));
            ++forgotten;
        }
    };
    std::function<void(const char*)> progress = [](const char*) {};

    void SetUp() override
    {
        const auto native = native_directory(layout.source);
        create_directories_durably(native / "config");
        replace_file_durably(
            native / "product.json",
            R"({"product":"asterion-terminal","version":"0.1.0"})");
        replace_file_durably(native / "config/research.json", R"({"task_concurrency":2})");
        replace_file_durably(
            native / "config/logging.json",
            R"({"format":"json","retention_days":30,"console":{"enabled":false,"level":"info"},"file":{"enabled":true,"level":"info"}})");
    }

    void TearDown() override
    {
        fs::remove_all(root);
    }
};

TEST_F(Installation, InstallUpgradeUninstallPreserveUserData)
{
    EXPECT_FALSE(inspect(layout).at("installed"));
    EXPECT_EQ(inspect(layout).at("deployment_action"), "install");
    deploy(layout, Deployment::install, services, progress);
    EXPECT_TRUE(inspect(layout).at("installed"));
    EXPECT_EQ(inspect(layout).at("deployment_action"), "repair");
    EXPECT_EQ(registered, 1);
    EXPECT_EQ(started, 1);
    const auto identity = contents(layout.node / "ipc-id");
    replace_file_durably(layout.node / "config/research.json", R"({"task_concurrency":7})");
    replace_file_durably(layout.node / "ledger", "operator data");
    create_directories_durably(layout.enrollment);
    replace_file_durably(layout.enrollment / "connection", "local connection settings");
    replace_file_durably(
        native_directory(layout.source) / "product.json",
        R"({"product":"asterion-terminal","version":"0.2.0"})");
    EXPECT_EQ(inspect(layout).at("deployment_action"), "upgrade");
    deploy(layout, Deployment::upgrade, services, progress);
    EXPECT_EQ(started, 2);
    EXPECT_EQ(inspect(layout).at("version"), "0.2.0");
    EXPECT_EQ(contents(layout.node / "config/research.json"), R"({"task_concurrency":7})");
    EXPECT_EQ(contents(layout.node / "ipc-id"), identity);
    EXPECT_EQ(prepared, 2);
    uninstall(layout, true, services, progress);
    EXPECT_FALSE(fs::exists(layout.destination));
    EXPECT_FALSE(inspect(layout).at("installed"));
    EXPECT_EQ(forgotten, 0) << "kept data keeps its saved credentials";
    EXPECT_EQ(contents(layout.node / "ledger"), "operator data");
    EXPECT_EQ(contents(layout.node / "ipc-id"), identity);
    EXPECT_EQ(removed, 2);
    EXPECT_EQ(contents(layout.enrollment / "connection"), "local connection settings");
}

TEST_F(Installation, ServiceStartFailureDoesNotUndoPublishedInstallation)
{
    services.start = [] {
        throw std::runtime_error("system service start failed");
    };
    bool complete = false;
    EXPECT_THROW(
        deploy(
            layout,
            Deployment::install,
            services,
            [&](const char* step) {
                complete = std::string(step) == "complete";
            }),
        std::runtime_error);
    EXPECT_FALSE(complete);
    EXPECT_TRUE(inspect(layout).at("installed"));
    EXPECT_EQ(inspect(layout).at("pending"), "");
    EXPECT_EQ(inspect(layout).at("deployment_action"), "repair");
    // Repair remains a stopped maintenance operation, even after a start failure.
    EXPECT_NO_THROW(deploy(layout, Deployment::repair, services, progress));
}

TEST_F(Installation, ExplicitDataRemovalResumesTheSameChoiceAndRetainsDiagnosticsAndLocks)
{
    deploy(layout, Deployment::install, services, progress);
    replace_file_durably(layout.node / "ledger", "operator data");
    create_directories_durably(layout.node / "logs");
    replace_file_durably(layout.node / "logs/installer.log", "diagnostic evidence");
    create_directories_durably(layout.enrollment);
    replace_file_durably(layout.enrollment / "connection", "local connection settings");
    const auto outside = root / "external-export";
    replace_file_durably(outside, "preserve external files");
    fs::create_symlink(outside, layout.node / "export-link");
    auto failing = services;
    failing.unregister = [] {
        throw std::runtime_error("system service removal failed");
    };
    EXPECT_THROW(uninstall(layout, false, failing, progress), std::runtime_error);
    EXPECT_EQ(inspect(layout).at("pending"), "uninstall");
    EXPECT_FALSE(inspect(layout).at("preserve_data"));
    EXPECT_THROW(uninstall(layout, true, services, progress), std::runtime_error);
    EXPECT_TRUE(fs::exists(layout.node / "ledger"));
    uninstall(layout, false, services, [&](const char* step) {
        if (std::string_view(step) == "complete") {
            EXPECT_THROW((FileLock(layout.node, "installation.lock")), std::runtime_error);
            EXPECT_THROW((FileLock(layout.node, "terminal.lock")), std::runtime_error);
            EXPECT_THROW((FileLock(layout.node, "agent.lock")), std::runtime_error);
            EXPECT_THROW((FileLock(layout.node / "manager", "manager.lock")), std::runtime_error);
        }
    });
    EXPECT_FALSE(fs::exists(layout.destination));
    EXPECT_EQ(forgotten, 1);
    EXPECT_FALSE(fs::exists(layout.node / "config"));
    EXPECT_FALSE(fs::exists(layout.node / "ledger"));
    EXPECT_FALSE(fs::exists(layout.node / "ipc-id"));
    EXPECT_FALSE(fs::exists(layout.node / "manager/ipc-id"));
    EXPECT_FALSE(fs::exists(layout.enrollment));
    EXPECT_FALSE(fs::exists(layout.node / "installation.json"));
    EXPECT_EQ(contents(outside), "preserve external files");
    EXPECT_EQ(contents(layout.node / "logs/installer.log"), "diagnostic evidence");
    EXPECT_TRUE(inspect(layout).at("preserve_data"));
}

TEST_F(Installation, RepairRestoresMissingProgramsAndPreservesUserState)
{
    deploy(layout, Deployment::install, services, progress);
    const auto identity = contents(layout.node / "ipc-id");
    replace_file_durably(layout.node / "config/research.json", R"({"task_concurrency":7})");
    replace_file_durably(layout.node / "ledger", "operator data");
    fs::remove_all(layout.destination);
    EXPECT_TRUE(inspect(layout).at("installed"));
    EXPECT_EQ(inspect(layout).at("deployment_action"), "repair");
    deploy(layout, Deployment::repair, services, progress);
    EXPECT_TRUE(fs::exists(native_directory(layout.destination) / "product.json"));
    EXPECT_EQ(registered, 2);
    EXPECT_EQ(started, 1);
    EXPECT_EQ(removed, 1);
    EXPECT_EQ(prepared, 2);
    EXPECT_EQ(contents(layout.node / "config/research.json"), R"({"task_concurrency":7})");
    EXPECT_EQ(contents(layout.node / "ipc-id"), identity);
    EXPECT_EQ(contents(layout.node / "ledger"), "operator data");
}

TEST_F(Installation, ReleaseOrderingRejectsWrongOperationsBeforeChangingTheInstallation)
{
    deploy(layout, Deployment::install, services, progress);
    const auto record = contents(layout.node / "installation.json");
    EXPECT_THROW(deploy(layout, Deployment::upgrade, services, progress), std::runtime_error);
    replace_file_durably(
        native_directory(layout.source) / "product.json",
        R"({"product":"asterion-terminal","version":"0.0.9"})");
    EXPECT_EQ(inspect(layout).at("deployment_action"), "");
    EXPECT_THROW(deploy(layout, Deployment::upgrade, services, progress), std::runtime_error);
    EXPECT_THROW(deploy(layout, Deployment::repair, services, progress), std::runtime_error);
    EXPECT_EQ(contents(layout.node / "installation.json"), record);
    EXPECT_EQ(removed, 0);
    replace_file_durably(
        native_directory(layout.source) / "product.json",
        R"({"product":"asterion-terminal","version":"0.10.0"})");
    deploy(layout, Deployment::upgrade, services, progress);
    replace_file_durably(
        native_directory(layout.source) / "product.json",
        R"({"product":"asterion-terminal","version":"0.2.0"})");
    EXPECT_EQ(inspect(layout).at("deployment_action"), "");
    uninstall(layout, true, services, progress); // An older package may still uninstall.
    EXPECT_FALSE(inspect(layout).at("installed"));
}

TEST_F(Installation, FailedUpgradeResumesWithItsTargetVersionAfterPublication)
{
    deploy(layout, Deployment::install, services, progress);
    replace_file_durably(
        native_directory(layout.source) / "product.json",
        R"({"product":"asterion-terminal","version":"0.2.0"})");
    auto failing = services;
    failing.register_stopped = [] {
        throw std::runtime_error("system service registration failed");
    };
    EXPECT_THROW(deploy(layout, Deployment::upgrade, failing, progress), std::runtime_error);
    EXPECT_EQ(inspect(layout).at("pending"), "upgrade");
    EXPECT_EQ(inspect(layout).at("deployment_action"), "upgrade");
    EXPECT_THROW(deploy(layout, Deployment::repair, services, progress), std::runtime_error);
    replace_file_durably(
        native_directory(layout.source) / "product.json",
        R"({"product":"asterion-terminal","version":"0.3.0"})");
    EXPECT_NE(inspect(layout).at("conflict"), "");
    EXPECT_THROW(deploy(layout, Deployment::upgrade, services, progress), std::runtime_error);
    replace_file_durably(
        native_directory(layout.source) / "product.json",
        R"({"product":"asterion-terminal","version":"0.2.0"})");
    deploy(layout, Deployment::upgrade, services, progress);
    EXPECT_EQ(inspect(layout).at("pending"), "");
    EXPECT_EQ(inspect(layout).at("deployment_action"), "repair");
}

TEST_F(Installation, FailedRegistrationRequiresExplicitResumeAndPreservesConfiguration)
{
    auto failing = services;
    failing.register_stopped = [] {
        throw std::runtime_error("system service registration failed");
    };
    EXPECT_THROW(deploy(layout, Deployment::install, failing, progress), std::runtime_error);
    EXPECT_EQ(inspect(layout).at("pending"), "install");
    EXPECT_THROW(deploy(layout, Deployment::upgrade, services, progress), std::runtime_error);
    replace_file_durably(layout.node / "config/research.json", R"({"task_concurrency":9})");
    deploy(layout, Deployment::install, services, progress);
    EXPECT_EQ(inspect(layout).at("pending"), "");
    EXPECT_EQ(contents(layout.node / "config/research.json"), R"({"task_concurrency":9})");
}

TEST_F(Installation, RunningTerminalAndForeignDestinationAreNotModified)
{
    create_directories_durably(layout.node);
    {
        FileLock terminal(layout.node, "terminal.lock", FileLock::Access::shared);
        EXPECT_THROW(deploy(layout, Deployment::install, services, progress), std::runtime_error);
        EXPECT_FALSE(fs::exists(layout.destination));
    }
    create_directories_durably(layout.destination);
    replace_file_durably(layout.destination / "foreign", "preserve");
    EXPECT_THROW(deploy(layout, Deployment::install, services, progress), std::runtime_error);
    EXPECT_EQ(contents(layout.destination / "foreign"), "preserve");
    EXPECT_FALSE(fs::exists(layout.node / "installation.json"));
}

TEST_F(Installation, RemoteMaintenanceMustPrecedeLocalReplacementAndIdentityRemoval)
{
    deploy(layout, Deployment::install, services, progress);
    create_directories_durably(layout.enrollment / "remote");
    replace_file_durably(layout.enrollment / "remote/enrollment.json", R"({"installed":true})");
    EXPECT_NO_THROW(deploy(layout, Deployment::repair, services, progress));
    replace_file_durably(
        native_directory(layout.source) / "product.json",
        R"({"product":"asterion-terminal","version":"0.2.0"})");
    EXPECT_THROW(deploy(layout, Deployment::upgrade, services, progress), std::exception);
    EXPECT_THROW(uninstall(layout, false, services, progress), std::exception);
    EXPECT_TRUE(fs::exists(layout.destination));
    EXPECT_EQ(inspect(layout).at("pending"), "");
    replace_file_durably(
        layout.enrollment / ".maintenance.json",
        R"({"action":"prepare","version":"0.2.0","nodes":[{"id":"remote","phase":"prepared"}]})");
    EXPECT_NO_THROW(deploy(layout, Deployment::upgrade, services, progress));
    EXPECT_THROW(uninstall(layout, true, services, progress), std::exception);
    fs::remove(layout.enrollment / ".maintenance.json");
    // Removing only the local application preserves remote management identities.
    EXPECT_NO_THROW(uninstall(layout, true, services, progress));
    EXPECT_TRUE(fs::exists(layout.enrollment / "remote/enrollment.json"));
}

} // namespace
} // namespace asterion::installation
