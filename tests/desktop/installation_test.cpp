#include "installation.hpp"
#include <asterion/system/durable_file.hpp>
#include <asterion/system/process/child.hpp>
#include <asterion/system/process/file_lock.hpp>
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
    Layout layout{root / "payload", root / "applications/terminal", root / "node"};
    int added = 0, removed = 0, forgotten = 0;
    Desktop desktop{
        [&](bool add) {
            ++(add ? added : removed);
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
    const auto settings = layout.root / "research/config/research/research.json";
    EXPECT_FALSE(inspect(layout).at("installed"));
    EXPECT_EQ(inspect(layout).at("deployment_action"), "install");
    deploy(layout, Deployment::install, desktop, progress);
    EXPECT_TRUE(inspect(layout).at("installed"));
    EXPECT_EQ(inspect(layout).at("deployment_action"), "repair");
    EXPECT_EQ(added, 1);
    EXPECT_TRUE(fs::exists(layout.terminal() / "config/logging.json"));
    create_directories_durably(settings.parent_path());
    replace_file_durably(settings, R"({"task_concurrency":7})");
    replace_file_durably(layout.terminal() / "config/logging.json", "operator logging");
    create_directories_durably(layout.root / "market/data");
    replace_file_durably(layout.root / "market/data/ledger", "operator data");
    replace_file_durably(
        native_directory(layout.source) / "product.json",
        R"({"product":"asterion-terminal","version":"0.2.0"})");
    EXPECT_EQ(inspect(layout).at("deployment_action"), "upgrade");
    deploy(layout, Deployment::upgrade, desktop, progress);
    EXPECT_EQ(added, 2);
    EXPECT_EQ(inspect(layout).at("version"), "0.2.0");
    EXPECT_EQ(contents(settings), R"({"task_concurrency":7})");
    EXPECT_EQ(contents(layout.terminal() / "config/logging.json"), "operator logging");
    uninstall(layout, true, desktop, progress);
    EXPECT_FALSE(fs::exists(layout.destination));
    EXPECT_FALSE(inspect(layout).at("installed"));
    EXPECT_EQ(removed, 1);
    EXPECT_EQ(forgotten, 0) << "kept data keeps its saved credentials";
    EXPECT_EQ(contents(layout.root / "market/data/ledger"), "operator data");
    EXPECT_EQ(contents(settings), R"({"task_concurrency":7})");
}

TEST_F(Installation, ExplicitDataRemovalResumesTheSameChoiceAndRetainsDiagnosticsAndLocks)
{
    deploy(layout, Deployment::install, desktop, progress);
    create_directories_durably(layout.root / "market/data");
    replace_file_durably(layout.root / "market/data/ledger", "operator data");
    create_directories_durably(layout.terminal() / "logs");
    replace_file_durably(layout.terminal() / "logs/installer.log", "diagnostic evidence");
    const auto outside = root / "external-export";
    replace_file_durably(outside, "preserve external files");
    fs::create_symlink(outside, layout.root / "export-link");
    auto failing = desktop;
    failing.launcher = [](bool) {
        throw std::runtime_error("launcher removal failed");
    };
    EXPECT_THROW(uninstall(layout, false, failing, progress), std::runtime_error);
    EXPECT_EQ(inspect(layout).at("pending"), "uninstall");
    EXPECT_FALSE(inspect(layout).at("preserve_data"));
    EXPECT_THROW(uninstall(layout, true, desktop, progress), std::runtime_error);
    EXPECT_TRUE(fs::exists(layout.root / "market/data/ledger"));
    uninstall(layout, false, desktop, [&](const char* step) {
        if (std::string_view(step) == "complete") {
            EXPECT_THROW(
                (FileLock(layout.terminal() / "data", "installation.lock")),
                std::runtime_error);
            EXPECT_THROW(
                (FileLock(layout.terminal() / "data", "terminal.lock")),
                std::runtime_error);
        }
    });
    EXPECT_FALSE(fs::exists(layout.destination));
    EXPECT_EQ(forgotten, 1);
    EXPECT_FALSE(fs::exists(layout.terminal() / "config"));
    EXPECT_FALSE(fs::exists(layout.root / "market/data/ledger"));
    EXPECT_FALSE(fs::exists(layout.terminal() / "data/installation.json"));
    EXPECT_EQ(contents(outside), "preserve external files");
    EXPECT_EQ(contents(layout.terminal() / "logs/installer.log"), "diagnostic evidence");
    EXPECT_TRUE(inspect(layout).at("preserve_data"));
}

TEST_F(Installation, RepairRestoresMissingProgramsAndPreservesUserState)
{
    deploy(layout, Deployment::install, desktop, progress);
    create_directories_durably(layout.root / "market/data");
    replace_file_durably(layout.root / "market/data/ledger", "operator data");
    fs::remove_all(layout.destination);
    EXPECT_TRUE(inspect(layout).at("installed"));
    EXPECT_EQ(inspect(layout).at("deployment_action"), "repair");
    deploy(layout, Deployment::repair, desktop, progress);
    EXPECT_TRUE(fs::exists(native_directory(layout.destination) / "product.json"));
    EXPECT_EQ(added, 2);
    EXPECT_EQ(removed, 0);
    EXPECT_EQ(contents(layout.root / "market/data/ledger"), "operator data");
}

TEST_F(Installation, ReleaseOrderingRejectsWrongOperationsBeforeChangingTheInstallation)
{
    deploy(layout, Deployment::install, desktop, progress);
    const auto record = contents(layout.terminal() / "data/installation.json");
    EXPECT_THROW(deploy(layout, Deployment::upgrade, desktop, progress), std::runtime_error);
    replace_file_durably(
        native_directory(layout.source) / "product.json",
        R"({"product":"asterion-terminal","version":"0.0.9"})");
    EXPECT_EQ(inspect(layout).at("deployment_action"), "");
    EXPECT_THROW(deploy(layout, Deployment::upgrade, desktop, progress), std::runtime_error);
    EXPECT_THROW(deploy(layout, Deployment::repair, desktop, progress), std::runtime_error);
    EXPECT_EQ(contents(layout.terminal() / "data/installation.json"), record);
    EXPECT_EQ(added, 1);
    replace_file_durably(
        native_directory(layout.source) / "product.json",
        R"({"product":"asterion-terminal","version":"0.10.0"})");
    deploy(layout, Deployment::upgrade, desktop, progress);
    replace_file_durably(
        native_directory(layout.source) / "product.json",
        R"({"product":"asterion-terminal","version":"0.2.0"})");
    EXPECT_EQ(inspect(layout).at("deployment_action"), "");
    uninstall(layout, true, desktop, progress); // An older package may still uninstall.
    EXPECT_FALSE(inspect(layout).at("installed"));
}

TEST_F(Installation, FailedUpgradeResumesWithItsTargetVersionAfterPublication)
{
    deploy(layout, Deployment::install, desktop, progress);
    replace_file_durably(
        native_directory(layout.source) / "product.json",
        R"({"product":"asterion-terminal","version":"0.2.0"})");
    auto failing = desktop;
    failing.launcher = [](bool) {
        throw std::runtime_error("launcher registration failed");
    };
    EXPECT_THROW(deploy(layout, Deployment::upgrade, failing, progress), std::runtime_error);
    EXPECT_EQ(inspect(layout).at("pending"), "upgrade");
    EXPECT_EQ(inspect(layout).at("deployment_action"), "upgrade");
    EXPECT_THROW(deploy(layout, Deployment::repair, desktop, progress), std::runtime_error);
    replace_file_durably(
        native_directory(layout.source) / "product.json",
        R"({"product":"asterion-terminal","version":"0.3.0"})");
    EXPECT_NE(inspect(layout).at("conflict"), "");
    EXPECT_THROW(deploy(layout, Deployment::upgrade, desktop, progress), std::runtime_error);
    replace_file_durably(
        native_directory(layout.source) / "product.json",
        R"({"product":"asterion-terminal","version":"0.2.0"})");
    deploy(layout, Deployment::upgrade, desktop, progress);
    EXPECT_EQ(inspect(layout).at("pending"), "");
    EXPECT_EQ(inspect(layout).at("deployment_action"), "repair");
}

TEST_F(Installation, FailedInstallRequiresExplicitResumeAndPreservesConfiguration)
{
    auto failing = desktop;
    failing.launcher = [](bool) {
        throw std::runtime_error("launcher registration failed");
    };
    EXPECT_THROW(deploy(layout, Deployment::install, failing, progress), std::runtime_error);
    EXPECT_EQ(inspect(layout).at("pending"), "install");
    EXPECT_THROW(deploy(layout, Deployment::upgrade, desktop, progress), std::runtime_error);
    replace_file_durably(layout.terminal() / "config/logging.json", "operator logging");
    deploy(layout, Deployment::install, desktop, progress);
    EXPECT_EQ(inspect(layout).at("pending"), "");
    EXPECT_EQ(contents(layout.terminal() / "config/logging.json"), "operator logging");
}

TEST_F(Installation, RunningTerminalAndForeignDestinationAreNotModified)
{
    create_directories_durably(layout.terminal() / "data");
    {
        FileLock terminal(layout.terminal() / "data", "terminal.lock", FileLock::Access::shared);
        EXPECT_THROW(deploy(layout, Deployment::install, desktop, progress), std::runtime_error);
        EXPECT_FALSE(fs::exists(layout.destination));
        EXPECT_EQ(inspect(layout).at("busy"), Json::array({"terminal"}));
    }
    create_directories_durably(layout.destination);
    replace_file_durably(layout.destination / "foreign", "preserve");
    EXPECT_THROW(deploy(layout, Deployment::install, desktop, progress), std::runtime_error);
    EXPECT_EQ(contents(layout.destination / "foreign"), "preserve");
    EXPECT_FALSE(fs::exists(layout.terminal() / "data/installation.json"));
}

} // namespace
} // namespace asterion::installation
