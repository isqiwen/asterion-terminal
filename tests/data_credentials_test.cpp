#include "timing.hpp"
#include "credential_fixture.hpp"
#include "application_environment.hpp"
#include "node_client.hpp"
#include "node_enrollment.hpp"
#include <asterion/kernel/durable_file.hpp>
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
    fail_next_directory_syncs_for_testing(0);
    std::error_code ignored;
    fs::remove_all(path, ignored);
  }
};
const terminal::DataCredentialLimits limits{true, 64, true, 500};
terminal::DataCredential credential(const std::string& provider, bool remember) {
  return {provider, 60, remember, "secret-" + provider};
}
} // namespace
TEST(DataCredentials, DirectoryPublicationFailureDoesNotChangeTheCredentialStore) {
  Directory root;
  fs::create_directory(root.path);
  auto keychain = std::make_shared<test::MemoryCredentials>();
  const auto directory = root.path / "providers";
  terminal::DataCredentials credentials(directory, keychain);
  for (int retry = 0; retry != 2; ++retry) {
    fail_next_directory_syncs_for_testing(1);
    EXPECT_THROW(credentials.save(credential("provider", true), limits), std::runtime_error);
    fail_next_directory_syncs_for_testing(0);
    EXPECT_TRUE(keychain->items.empty());
    EXPECT_FALSE(fs::exists(directory / "provider.json"));
  }
  credentials.save(credential("provider", true), limits);
  EXPECT_EQ(credentials.find("provider")->credential, "secret-provider");
}
#ifndef _WIN32
TEST(NodeEnrollment, ManagedKeyIsNotAcknowledgedWhenDirectoryPublicationFails) {
  Directory root;
  struct Environment {
    std::optional<std::string> directory = environment_variable("ASTERION_NODE_DIRECTORY");
    ~Environment() {
      if (directory)
        setenv("ASTERION_NODE_DIRECTORY", directory->c_str(), 1);
      else
        unsetenv("ASTERION_NODE_DIRECTORY");
    }
  } environment;
  setenv("ASTERION_NODE_DIRECTORY", root.path.c_str(), 1);
  fail_next_directory_syncs_for_testing(1);
  EXPECT_THROW(terminal::prepare_ssh_key("task"), std::runtime_error);
  fail_next_directory_syncs_for_testing(0);
  EXPECT_FALSE(fs::exists(root.path / "enrollments/.ssh-keys/task/identity"));
  const auto prepared = terminal::prepare_ssh_key("task");
  const auto identity = root.path / "enrollments/.ssh-keys/task/identity";
  const auto before = fs::last_write_time(identity).time_since_epoch().count();
  fail_next_directory_syncs_for_testing(1);
  EXPECT_THROW(terminal::prepare_ssh_key("task"), std::runtime_error);
  fail_next_directory_syncs_for_testing(0);
  EXPECT_EQ(terminal::prepare_ssh_key("task"), prepared);
  EXPECT_EQ(fs::last_write_time(identity).time_since_epoch().count(), before);
}
#endif
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

#ifdef __APPLE__
#include <csignal>
#include <cerrno>
#include <unistd.h>
TEST(KeychainHelper, BoundedTransportPreservesTheCredentialContract) {
  auto store = terminal::keychain_store(ASTERION_TEST_KEYCHAIN_HELPER);
  ASSERT_NE(store, nullptr);
  EXPECT_EQ(store->load("normal"), "fixture-only-token");
  EXPECT_FALSE(store->load("absent"));
  EXPECT_NO_THROW(store->store("normal", "fixture-only-token"));
  EXPECT_NO_THROW(store->erase("normal"));
  EXPECT_THROW(store->store("normal", std::string(4097, 'x')), Error);
  EXPECT_THROW(store->store("normal", std::string("a\0b", 3)), Error);
  for (int i = 0; i < 8; ++i)
    EXPECT_THROW(store->store("early-exit", "fixture-only-token"), Error);
  EXPECT_EQ(store->load("normal"), "fixture-only-token");
  EXPECT_THROW(
      terminal::keychain_store(ASTERION_TEST_KEYCHAIN_HELPER, std::chrono::milliseconds(0)), Error);
  EXPECT_THROW(terminal::keychain_store(ASTERION_TEST_KEYCHAIN_HELPER, std::chrono::seconds(31)),
               Error);
}
TEST(KeychainHelper, HungIoExitAndExcessOutputAreTerminatedAndReaped) {
  Directory root;
  fs::create_directory(root.path);
  // Prime first-launch executable validation before measuring a helper that
  // must enter its fault mode and publish the PID we verify reaped.
  ASSERT_EQ(terminal::keychain_store(ASTERION_TEST_KEYCHAIN_HELPER)->load("normal"),
            "fixture-only-token");
  auto store =
      terminal::keychain_store(ASTERION_TEST_KEYCHAIN_HELPER,
                               asterion::testing_support::bound(std::chrono::milliseconds(500)));
  ASSERT_NE(store, nullptr);
  for (const auto* mode : {"hang", "closed-output", "overflow"}) {
    const auto pid_file = root.path / mode;
    const auto started = std::chrono::steady_clock::now();
    try {
      store->load(std::string(mode) + ":" + pid_file.string());
      FAIL() << "faulty helper succeeded";
    } catch (const Error& error) {
      if (std::string_view(mode) == "overflow") {
        EXPECT_EQ(error.code(), ErrorCode::resource_exhausted);
        EXPECT_STREQ(error.what(), "keychain helper response exceeds the credential limit");
      } else {
        EXPECT_EQ(error.code(), ErrorCode::unavailable);
        EXPECT_STREQ(error.what(), "keychain helper timed out");
      }
    }
    EXPECT_LT(std::chrono::steady_clock::now() - started,
              asterion::testing_support::bound(std::chrono::seconds(3)));
    pid_t pid = 0;
    std::ifstream(pid_file) >> pid;
    ASSERT_GT(pid, 0);
    errno = 0;
    EXPECT_EQ(::kill(pid, 0), -1);
    EXPECT_EQ(errno, ESRCH) << "helper process survived or remained a zombie";
    EXPECT_EQ(store->load("normal"), "fixture-only-token");
  }
}
#endif
