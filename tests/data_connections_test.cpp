#include "credential_fixture.hpp"
#include "application_environment.hpp"
#include "node_client.hpp"
#include <asterion/kernel/process/child.hpp>
#include <algorithm>
#include <fstream>
#include <gtest/gtest.h>
using namespace asterion;
namespace fs = std::filesystem;
namespace {
struct Directory {
  fs::path path = fs::temp_directory_path() / ("asterion-connections-" + unique_process_id());
  ~Directory() {
    std::error_code ignored;
    fs::remove_all(path, ignored);
  }
};
data::v1::HistoryConnectionSchema schema() {
  data::v1::HistoryConnectionSchema result;
  result.set_credential_required(true);
  result.set_credential_max_length(64);
  result.set_remember_allowed(true);
  result.set_requests_per_minute_max(500);
  return result;
}
terminal::DataConnection connection(const std::string& id, bool remember) {
  return {id, "Connection " + id, "tushare.fut_daily", "asterion.data.tushare", "",
          60, remember,           "secret-" + id};
}
} // namespace
TEST(DataConnections, UnreadableFileIsListedWithoutFailingTheSnapshot) {
  Directory root;
  fs::create_directory(root.path);
  terminal::DataConnections connections(root.path / "data-connections",
                                        std::make_shared<test::MemoryCredentials>());
  EXPECT_TRUE(connections.snapshot().empty());
  connections.save(connection("one", true), "", "replace", schema());
  ASSERT_EQ(connections.snapshot().size(), 1U);
  EXPECT_TRUE(connections.snapshot()[0].at("credential_ready").get<bool>());
  EXPECT_FALSE(connections.snapshot()[0].contains("credential"));
  std::ofstream(root.path / "data-connections" / "broken.json") << "{";
  const auto listed = connections.snapshot();
  ASSERT_EQ(listed.size(), 2U);
  const auto find = [&](const char* id) {
    return *std::find_if(listed.begin(), listed.end(),
                         [&](const auto& item) { return item.at("id") == id; });
  };
  EXPECT_EQ(find("broken").at("error"), "unreadable");
  EXPECT_FALSE(find("one").contains("error"));
  EXPECT_TRUE(fs::exists(root.path / "data-connections" / "broken.json")) << "kept for inspection";
}
TEST(DataConnections, SnapshotReflectsSavesAndRemovalsThroughTheCache) {
  Directory root;
  fs::create_directory(root.path);
  terminal::DataConnections connections(root.path / "data-connections",
                                        std::make_shared<test::MemoryCredentials>());
  connections.save(connection("one", false), "", "replace", schema());
  auto listed = connections.snapshot();
  ASSERT_EQ(listed.size(), 1U);
  EXPECT_TRUE(listed[0].at("credential_ready").get<bool>()) << "session credential in memory";
  const auto revision = listed[0].at("revision").get<std::string>();
  auto changed = connection("one", false);
  changed.name = "Renamed";
  connections.save(changed, revision, "keep", schema());
  EXPECT_EQ(connections.snapshot()[0].at("name"), "Renamed");
  connections.remove("one", connections.snapshot()[0].at("revision").get<std::string>());
  EXPECT_TRUE(connections.snapshot().empty());
}

TEST(DataConnections, IdenticalConnectionIdsHaveIndependentCredentialsAcrossEnvironments) {
  Directory root;
  fs::create_directory(root.path);
  auto credentials = std::make_shared<test::MemoryCredentials>();
  terminal::DataConnections production(root.path / "production", credentials);
  terminal::DataConnections development(root.path / "development", credentials);
  auto first = connection("same-id", true);
  auto second = first;
  second.credential = "different-development-secret";
  production.save(first, "", "replace", schema());
  development.save(second, "", "replace", schema());
  EXPECT_EQ(credentials->items.size(), 2U);
  EXPECT_EQ(production.get(first.id).credential, first.credential);
  EXPECT_EQ(development.get(second.id).credential, second.credential);
  development.remove(second.id, development.get(second.id).revision);
  EXPECT_EQ(production.get(first.id).credential, first.credential);
  EXPECT_EQ(credentials->items.size(), 1U);
}

#ifdef __APPLE__
TEST(TerminalEnvironment, SeparatesManagedPathsAndServiceIdentityWithoutCreatingFiles) {
  struct Environment {
    std::optional<std::string> profile = environment_variable("ASTERION_ENVIRONMENT");
    std::optional<std::string> directory = environment_variable("ASTERION_NODE_DIRECTORY");
    ~Environment() {
      if (profile)
        setenv("ASTERION_ENVIRONMENT", profile->c_str(), 1);
      else
        unsetenv("ASTERION_ENVIRONMENT");
      if (directory)
        setenv("ASTERION_NODE_DIRECTORY", directory->c_str(), 1);
      else
        unsetenv("ASTERION_NODE_DIRECTORY");
    }
  } restore;
  const auto home = environment_path("HOME").value();
  unsetenv("ASTERION_NODE_DIRECTORY");
  setenv("ASTERION_ENVIRONMENT", "production", 1);
  EXPECT_EQ(terminal::local_node_directory(), home / "Library/Application Support/Asterion/node");
  EXPECT_EQ(terminal::node_enrollment_directory(), home / ".asterion/nodes");
  EXPECT_TRUE(terminal::local_node_service_name().empty());
  setenv("ASTERION_ENVIRONMENT", "development", 1);
  const auto development = home / "Library/Application Support/Asterion Development/node";
  EXPECT_EQ(terminal::local_node_directory(), development);
  EXPECT_EQ(terminal::node_enrollment_directory(), development / "enrollments");
  EXPECT_EQ(terminal::local_node_service_name(), "me.asterion.node-agent.dev");
  Directory isolated;
  setenv("ASTERION_NODE_DIRECTORY", isolated.path.c_str(), 1);
  EXPECT_EQ(terminal::local_node_directory(), isolated.path);
  EXPECT_EQ(terminal::node_enrollment_directory(), isolated.path / "enrollments");
  EXPECT_EQ(terminal::local_node_program_status().at("state"), "isolated");
  EXPECT_FALSE(fs::exists(isolated.path));
  setenv("ASTERION_ENVIRONMENT", "invalid", 1);
  EXPECT_THROW(terminal::local_node_directory(), std::invalid_argument);
}
#endif
