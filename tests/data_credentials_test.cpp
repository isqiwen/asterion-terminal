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
  fs::path path = fs::temp_directory_path() / ("asterion-credentials-" + unique_process_id());
  ~Directory() {
    std::error_code ignored;
    fs::remove_all(path, ignored);
  }
};
const terminal::DataCredentialLimits limits{true, 64, true, 500};
terminal::DataCredential credential(const std::string& provider, bool remember) {
  return {provider, 60, remember, "secret-" + provider};
}
} // namespace
TEST(DataCredentials, UnreadableFileIsListedWithoutFailingTheSnapshot) {
  Directory root;
  fs::create_directory(root.path);
  terminal::DataCredentials credentials(root.path / "data-providers",
                                        std::make_shared<test::MemoryCredentials>());
  EXPECT_TRUE(credentials.snapshot().empty());
  credentials.save(credential("asterion.data.tushare", true), limits);
  ASSERT_EQ(credentials.snapshot().size(), 1U);
  EXPECT_TRUE(credentials.snapshot()[0].at("credential_ready").get<bool>());
  EXPECT_FALSE(credentials.snapshot()[0].contains("credential"));
  std::ofstream(root.path / "data-providers" / "broken.json") << "{";
  const auto listed = credentials.snapshot();
  ASSERT_EQ(listed.size(), 2U);
  const auto find = [&](const char* provider) {
    return *std::find_if(listed.begin(), listed.end(),
                         [&](const auto& item) { return item.at("provider") == provider; });
  };
  EXPECT_EQ(find("broken").at("error"), "unreadable");
  EXPECT_FALSE(find("asterion.data.tushare").contains("error"));
  EXPECT_TRUE(fs::exists(root.path / "data-providers" / "broken.json")) << "kept for inspection";
}
TEST(DataCredentials, OneEntryPerProviderKeepsItsCredentialUntilReplacedOrCleared) {
  Directory root;
  fs::create_directory(root.path);
  terminal::DataCredentials credentials(root.path / "data-providers",
                                        std::make_shared<test::MemoryCredentials>());
  EXPECT_FALSE(credentials.find("one"));
  credentials.save(credential("one", false), limits);
  ASSERT_EQ(credentials.snapshot().size(), 1U);
  EXPECT_TRUE(credentials.snapshot()[0].at("credential_ready").get<bool>())
      << "session credential in memory";
  // Saving without a credential changes the settings and keeps the secret.
  credentials.save({"one", 30, false, ""}, limits);
  ASSERT_EQ(credentials.snapshot().size(), 1U);
  EXPECT_EQ(credentials.snapshot()[0].at("requests_per_minute"), 30);
  EXPECT_EQ(credentials.find("one")->credential, "secret-one");
  credentials.save({"one", 30, false, "replaced"}, limits);
  EXPECT_EQ(credentials.find("one")->credential, "replaced");
  credentials.clear("one");
  EXPECT_TRUE(credentials.snapshot().empty());
  EXPECT_FALSE(credentials.find("one"));
  EXPECT_THROW(credentials.save({"one", 30, false, ""}, limits), std::invalid_argument)
      << "a cleared provider has nothing to keep";
}

TEST(DataCredentials, TheSameProviderHasIndependentCredentialsAcrossEnvironments) {
  Directory root;
  fs::create_directory(root.path);
  auto keychain = std::make_shared<test::MemoryCredentials>();
  terminal::DataCredentials production(root.path / "production", keychain);
  terminal::DataCredentials development(root.path / "development", keychain);
  auto first = credential("same.provider", true);
  auto second = first;
  second.credential = "different-development-secret";
  production.save(first, limits);
  development.save(second, limits);
  EXPECT_EQ(keychain->items.size(), 2U);
  EXPECT_EQ(production.find(first.provider)->credential, first.credential);
  EXPECT_EQ(development.find(second.provider)->credential, second.credential);
  development.clear(second.provider);
  EXPECT_EQ(production.find(first.provider)->credential, first.credential);
  EXPECT_EQ(keychain->items.size(), 1U);
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
