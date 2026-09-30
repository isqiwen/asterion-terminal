#include "plugin_catalog.hpp"
#include "plugin_artifacts.hpp"
#include "service_configuration.hpp"
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/child.hpp>
#include <gtest/gtest.h>
#include <fstream>
using namespace asterion;
namespace fs = std::filesystem;
namespace {
class PluginManagement : public ::testing::Test {
protected:
  fs::path root = fs::temp_directory_path() / ("asterion-plugin-management-" + unique_process_id());
  void SetUp() override { fs::create_directories(root); }
  void TearDown() override { fs::remove_all(root); }
  fs::path copy_plugin(const char* source, const std::string& name) {
    const auto path = root / (name + fs::path(source).extension().string());
    fs::copy_file(source, path);
    return path;
  }
};
TEST_F(PluginManagement, BrokenModuleDoesNotHideHealthySelection) {
  const auto good = copy_plugin(PLUGIN_GOOD, "good");
  copy_plugin(PLUGIN_BAD_ABI, "broken");
  const auto catalog = terminal::PluginCatalog::inspect(root);
  ASSERT_EQ(catalog.entries.size(), 2);
  const std::vector<std::string> hashes{sha256_file(good)};
  const auto selected = catalog.select_research(hashes);
  ASSERT_EQ(selected.uploads.size(), 1);
  EXPECT_EQ(selected.uploads.front().path, good);
  const auto view = terminal::plugin_catalog_json(catalog);
  EXPECT_EQ(view.at("items").at(0).at("state"), "invalid");
  EXPECT_EQ(view.at("items").at(1).at("state"), "available");
}
TEST_F(PluginManagement, RetainedArtifactDoesNotRequireSourceButNewSelectionDoes) {
  const auto good = copy_plugin(PLUGIN_GOOD, "good");
  const std::vector<std::string> hashes{sha256_file(good)};
  fs::remove(good);
  const auto catalog = terminal::PluginCatalog::inspect(root);
  const auto retained = catalog.select_research(hashes, hashes);
  EXPECT_EQ(retained.hashes, hashes);
  EXPECT_TRUE(retained.uploads.empty());
  EXPECT_THROW(catalog.select_research(hashes), std::invalid_argument);
  const std::vector<std::string> duplicate{hashes.front(), hashes.front()};
  EXPECT_THROW(catalog.select_research(duplicate, hashes), std::invalid_argument);
  EXPECT_TRUE(catalog.select_research({}).hashes.empty());
}
TEST_F(PluginManagement, DuplicateIdentityCannotBeSelectedByFilenameOrder) {
  const auto good = copy_plugin(PLUGIN_GOOD, "first");
  copy_plugin(PLUGIN_GOOD, "second");
  const auto catalog = terminal::PluginCatalog::inspect(root);
  for (const auto& entry : catalog.entries)
    EXPECT_EQ(entry.availability, terminal::PluginAvailability::invalid);
  const std::vector<std::string> hashes{sha256_file(good)};
  EXPECT_THROW(catalog.select_research(hashes), std::invalid_argument);
}
TEST_F(PluginManagement, ArtifactInstallationRejectsUnlistedFilesAndTampering) {
  fs::create_directory(root / "artifacts");
  fs::create_directories(root / "services/research");
  const auto hash = sha256_file(PLUGIN_GOOD);
  const auto suffix = current_platform().os == "windows" ? ".exe" : ".bin";
  fs::copy_file(PLUGIN_GOOD, root / "artifacts" / (hash + suffix));
  const agent::PluginArtifacts artifacts(root);
  const auto installed = artifacts.materialize("research", {hash});
  const auto library = installed / (hash + (current_platform().os == "macos" ? ".dylib" : ".so"));
  EXPECT_EQ(sha256_file(library), hash);
  write_file_durably(installed / "unlisted", "unexpected");
  EXPECT_THROW(artifacts.materialize("research", {hash}), std::invalid_argument);
  fs::remove(installed / "unlisted");
  write_file_durably(library, "corrupted");
  EXPECT_THROW(artifacts.materialize("research", {hash}), std::invalid_argument);
}
TEST_F(PluginManagement, ConfigurationKeepsVersionRevisionAndStoppedState) {
  fs::create_directory(root / "ledger");
  const Json document{{"version", 3},
                      {"kind", static_cast<int>(node::v1::TASK_SERVICE)},
                      {"artifact", std::string(64, 'a')},
                      {"provider_artifact", ""},
                      {"worker_artifact", std::string(64, 'b')},
                      {"factor_artifact", std::string(64, 'c')},
                      {"data_artifact", std::string(64, 'd')},
                      {"plugin_artifacts", Json::array({std::string(64, 'e')})},
                      {"port", 0},
                      {"desired", false},
                      {"directory", (root / "ledger").string()}};
  write_file_durably(root / "service.json", document.dump());
  const auto configuration = agent::load_service_configuration(root, true, 0);
  EXPECT_FALSE(configuration.desired);
  EXPECT_EQ(agent::service_revision(configuration), sha256_bytes(document.dump()));
  agent::save_service_configuration(root, configuration);
  EXPECT_EQ(sha256_file(root / "service.json"), sha256_bytes(document.dump()));
  write_file_durably(root / "service.pending", "interrupted");
  EXPECT_THROW(agent::load_service_configuration(root, true, 0), std::runtime_error);
  EXPECT_THROW(agent::save_service_configuration(root, configuration), std::runtime_error);
  EXPECT_EQ(sha256_file(root / "service.json"), sha256_bytes(document.dump()));
}
} // namespace

#include "data_connections.hpp"
namespace {
data::v1::HistoryConnectionSchema connection_schema(bool remember = true) {
  data::v1::HistoryConnectionSchema schema;
  schema.set_credential_required(true);
  schema.set_credential_max_length(256);
  schema.set_remember_allowed(remember);
  schema.set_requests_per_minute_max(60);
  return schema;
}
} // namespace
TEST_F(PluginManagement, ConnectionCredentialsStayPrivateAndSessionCredentialsExpire) {
  terminal::DataConnections store(root / "connections");
  terminal::DataConnection input{"alpha", "First", "fixture.minutes", "test.independent.c", "",
                                 30,      false,   "fixture-secret"};
  store.save(input, "", "replace", connection_schema());
  const auto saved = store.get("alpha");
  EXPECT_EQ(saved.credential, "fixture-secret");
  EXPECT_EQ(store.snapshot().dump().find("fixture-secret"), std::string::npos);
  std::ifstream file(root / "connections/alpha.json");
  const std::string contents{std::istreambuf_iterator<char>(file), {}};
  EXPECT_EQ(contents.find("fixture-secret"), std::string::npos);
  terminal::DataConnections reopened(root / "connections");
  EXPECT_TRUE(reopened.get("alpha").credential.empty());
  input.remember = true;
  input.credential.clear();
  store.save(input, saved.revision, "keep", connection_schema());
  EXPECT_EQ(reopened.get("alpha").credential, "fixture-secret");
  EXPECT_EQ(fs::status(root / "connections/alpha.json").permissions() & fs::perms::all,
            fs::perms::owner_read | fs::perms::owner_write);
  EXPECT_THROW(store.save(input, saved.revision, "keep", connection_schema()), Error);
  EXPECT_THROW(store.remove("alpha", saved.revision), Error);
  const auto current = store.get("alpha");
  store.remove("alpha", current.revision);
  EXPECT_TRUE(store.snapshot().empty());
  EXPECT_EQ(saved.credential, "fixture-secret");
}
TEST_F(PluginManagement, ConnectionsRejectForbiddenPersistenceAndUnsafePaths) {
  terminal::DataConnections store(root / "connections");
  terminal::DataConnection input{"alpha", "First", "fixture.minutes", "test.independent.c", "",
                                 30,      true,    "fixture-secret"};
  EXPECT_THROW(store.save(input, "", "replace", connection_schema(false)), std::invalid_argument);
  input.remember = false;
  input.requests_per_minute = 61;
  EXPECT_THROW(store.save(input, "", "replace", connection_schema()), std::invalid_argument);
  input.requests_per_minute = 30;
  input.id = "../outside";
  EXPECT_THROW(store.save(input, "", "replace", connection_schema()), std::invalid_argument);
  input.id = "alpha";
  fs::create_symlink(root / "outside", root / "connections/alpha.json");
  EXPECT_THROW(store.save(input, "", "replace", connection_schema()), std::invalid_argument);
}

TEST_F(PluginManagement, InstallationKeepsVersionsSeparateAndUninstallPreservesBundledFiles) {
  const auto bundled = root / "bundled";
  const auto managed = root / "managed";
  fs::create_directory(bundled);
  fs::copy_file(PLUGIN_GOOD, bundled / "original.dylib");
  const auto before = sha256_file(bundled / "original.dylib");
  const auto newer = terminal::preview_plugin(PLUGIN_NEWER);
  terminal::install_plugin(bundled, managed, PLUGIN_NEWER, newer.artifact.sha256);
  const auto catalog = terminal::plugin_inventory(bundled, managed);
  ASSERT_EQ(catalog.entries.size(), 2);
  EXPECT_EQ(catalog.entries[0].availability, terminal::PluginAvailability::available);
  EXPECT_EQ(catalog.entries[1].availability, terminal::PluginAvailability::available);
  EXPECT_TRUE(catalog.entries[1].managed);
  const std::vector<std::string> both{before, newer.artifact.sha256};
  EXPECT_THROW(catalog.select_research(both), std::invalid_argument);
  EXPECT_THROW(terminal::install_plugin(bundled, managed, PLUGIN_NEWER, newer.artifact.sha256),
               std::invalid_argument);
  EXPECT_THROW(terminal::install_plugin(bundled, managed, PLUGIN_NEWER, std::string(64, '0')),
               std::invalid_argument);
  EXPECT_THROW(
      terminal::install_plugin(bundled, managed, PLUGIN_BAD_ABI, sha256_file(PLUGIN_BAD_ABI)),
      std::invalid_argument);
  EXPECT_THROW(terminal::uninstall_plugin(managed, before + ".dylib", before),
               std::invalid_argument);
  terminal::uninstall_plugin(managed, newer.artifact.sha256 + ".dylib", newer.artifact.sha256);
  EXPECT_EQ(sha256_file(bundled / "original.dylib"), before);
  EXPECT_EQ(terminal::plugin_inventory(bundled, managed).entries.size(), 1);
}

TEST_F(PluginManagement, UninstallCanRemoveAnObservedCorruptCopyWithoutTouchingOtherFiles) {
  const auto bundled = root / "bundled", managed = root / "managed";
  fs::create_directory(bundled);
  const auto candidate = terminal::preview_plugin(PLUGIN_NEWER);
  terminal::install_plugin(bundled, managed, PLUGIN_NEWER, candidate.artifact.sha256);
  const auto filename = candidate.artifact.sha256 + ".dylib";
  replace_file_durably(managed / filename, "corrupt fixture");
  const auto catalog = terminal::plugin_inventory(bundled, managed);
  ASSERT_EQ(catalog.entries.size(), 1);
  EXPECT_EQ(catalog.entries[0].availability, terminal::PluginAvailability::invalid);
  const auto observed = catalog.entries[0].artifact.sha256;
  EXPECT_EQ(observed, sha256_bytes("corrupt fixture"));
  EXPECT_THROW(terminal::uninstall_plugin(managed, filename, candidate.artifact.sha256),
               std::invalid_argument);
  EXPECT_THROW(terminal::uninstall_plugin(managed, "../outside.dylib", observed),
               std::invalid_argument);
  terminal::uninstall_plugin(managed, filename, observed);
  EXPECT_FALSE(fs::exists(managed / filename));
}
