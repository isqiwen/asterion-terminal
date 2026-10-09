#include "ctp_connections.hpp"
#include <asterion/foundation/error.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/child.hpp>
#include <fstream>
#include <gtest/gtest.h>
using namespace asterion;
namespace fs = std::filesystem;
namespace {
struct Directory {
  fs::path path = fs::temp_directory_path() / ("asterion-ctp-connections-" + unique_process_id());
  Directory() { fs::create_directories(path); }
  ~Directory() {
    fail_next_directory_syncs_for_testing(0);
    std::error_code ignored;
    fs::remove_all(path, ignored);
  }
};
terminal::CtpConnection simnow(const std::string& id) {
  return {id,
          "SimNow " + id,
          "9999",
          "000001",
          "simnow_client_test",
          "tcp://180.168.146.187:10201",
          "tcp://180.168.146.187:10211",
          {}};
}
} // namespace
TEST(CtpConnections, DirectoryPublicationFailureDoesNotCommitAnAccountOrMarketSelection) {
  Directory root;
  const auto directory = root.path / "ctp-connections";
  terminal::CtpConnections connections(directory);
  for (int retry = 0; retry != 2; ++retry) {
    fail_next_directory_syncs_for_testing(1);
    EXPECT_THROW(connections.save(simnow("account"), ""), std::runtime_error);
    fail_next_directory_syncs_for_testing(0);
    EXPECT_FALSE(fs::exists(directory / "account.json"));
    EXPECT_FALSE(fs::exists(directory / "market"));
  }
  connections.save(simnow("account"), "");
  ASSERT_TRUE(connections.market());
  EXPECT_EQ(connections.market()->id, "account");
}
TEST(CtpConnections, SavedConnectionRoundTripsAndRevisionGuardsEdits) {
  Directory root;
  terminal::CtpConnections connections(root.path / "ctp-connections");
  EXPECT_TRUE(connections.snapshot().empty());
  connections.save(simnow("a"), "");
  const auto saved = connections.get("a");
  EXPECT_EQ(saved.trade_front, "tcp://180.168.146.187:10201");
  EXPECT_EQ(saved.market_front, "tcp://180.168.146.187:10211");
  ASSERT_EQ(connections.snapshot().size(), 1);
  EXPECT_EQ(connections.snapshot()[0].at("revision"), saved.revision);
  EXPECT_FALSE(connections.snapshot()[0].contains("version"));
  // A second save without the current revision is a concurrent edit.
  EXPECT_THROW(connections.save(simnow("a"), ""), Error);
  auto edited = simnow("a");
  edited.user_id = "000002";
  connections.save(edited, saved.revision);
  EXPECT_EQ(connections.get("a").user_id, "000002");
  EXPECT_THROW(connections.remove("a", saved.revision), Error);
  connections.remove("a", connections.get("a").revision);
  EXPECT_TRUE(connections.snapshot().empty());
}
TEST(CtpConnections, OneAccountSuppliesMarketDataUntilAnotherIsChosenOrItIsRemoved) {
  Directory root;
  terminal::CtpConnections connections(root.path / "ctp-connections");
  EXPECT_FALSE(connections.market());
  connections.save(simnow("a"), "");
  connections.save(simnow("b"), "");
  ASSERT_TRUE(connections.market());
  EXPECT_EQ(connections.market()->id, "a");
  EXPECT_EQ(connections.snapshot().size(), 2) << "the choice is not listed as an account";
  EXPECT_THROW(connections.select_market("missing"), std::exception);
  connections.select_market("b");
  EXPECT_EQ(connections.market()->id, "b");
  connections.remove("b", connections.get("b").revision);
  EXPECT_FALSE(connections.market()) << "a removed account is never silently replaced";
  connections.select_market("a");
  EXPECT_EQ(connections.market()->id, "a");
}
TEST(CtpConnections, RejectsMalformedSettingsBeforeWriting) {
  Directory root;
  terminal::CtpConnections connections(root.path / "ctp-connections");
  for (const auto& mutate : std::vector<void (*)(terminal::CtpConnection&)>{
           [](auto& c) { c.name.clear(); }, [](auto& c) { c.broker_id.clear(); },
           [](auto& c) { c.user_id = "000 001"; }, [](auto& c) { c.app_id = std::string(33, 'a'); },
           [](auto& c) { c.trade_front = "180.168.146.187:10201"; },
           [](auto& c) { c.market_front = "tcp://host:70000"; },
           [](auto& c) { c.trade_front = "tcp://bad host:1"; }, [](auto& c) { c.app_id.clear(); },
           [](auto& c) { c.trade_front.clear(); }, [](auto& c) { c.market_front.clear(); }}) {
    auto connection = simnow("bad");
    mutate(connection);
    EXPECT_THROW(connections.save(connection, ""), std::invalid_argument);
  }
  EXPECT_THROW(connections.save(simnow("../escape"), ""), std::invalid_argument);
  EXPECT_TRUE(connections.snapshot().empty());
}
TEST(CtpConnections, UnreadableFileIsListedWithoutFailingTheSnapshot) {
  Directory root;
  terminal::CtpConnections connections(root.path / "ctp-connections");
  connections.save(simnow("good"), "");
  std::ofstream(root.path / "ctp-connections" / "broken.json") << "{";
  const auto listed = connections.snapshot();
  ASSERT_EQ(listed.size(), 2);
  EXPECT_EQ(listed[1].at("id"), "broken");
  EXPECT_EQ(listed[1].at("error"), "unreadable");
  EXPECT_THROW(connections.get("broken"), std::exception);
}
TEST(CtpConnections, UnsupportedFormatIsRejectedWithoutRewriting) {
  Directory root;
  const auto directory = root.path / "ctp-connections";
  terminal::CtpConnections connections(directory);
  connections.save(simnow("a"), "");
  auto old = connections.snapshot().front();
  old["version"] = 2;
  const auto bytes = old.dump();
  replace_file_durably(directory / "a.json", bytes);
  EXPECT_THROW(connections.get("a"), std::invalid_argument);
  std::ifstream input(directory / "a.json");
  EXPECT_EQ(std::string(std::istreambuf_iterator<char>(input), {}), bytes);
}

#include "terminal/credential_fixture.hpp"
TEST(CtpConnections, MarketCredentialsSurviveReopenWithoutEnteringAccountFilesOrSnapshots) {
  Directory root;
  auto keychain = std::make_shared<test::MemoryCredentials>();
  const auto directory = root.path / "accounts";
  terminal::CtpConnections connections(directory, keychain);
  connections.save(simnow("a"), "");
  connections.remember_market_credentials("a", {"fixture-password", "fixture-auth"});
  terminal::CtpConnections reopened(directory, keychain);
  const auto saved = reopened.market_credentials(reopened.get("a"));
  EXPECT_EQ(saved.password, "fixture-password");
  EXPECT_EQ(saved.auth_code, "fixture-auth");
  std::ifstream input(directory / "a.json");
  const std::string file{std::istreambuf_iterator<char>(input), {}};
  EXPECT_EQ(file.find("fixture-password"), std::string::npos);
  EXPECT_EQ(reopened.snapshot().dump().find("fixture-auth"), std::string::npos);
  auto renamed = reopened.get("a");
  renamed.name = "Renamed";
  reopened.save(renamed, renamed.revision);
  EXPECT_EQ(reopened.market_credentials(reopened.get("a")).password, "fixture-password");
  terminal::CtpConnections other(root.path / "other-environment", keychain);
  other.save(simnow("a"), "");
  EXPECT_THROW(other.market_credentials(other.get("a")), Error);
  reopened.forget_market_credentials("a");
  EXPECT_TRUE(keychain->items.empty());
  EXPECT_THROW(reopened.market_credentials(reopened.get("a")), Error);
  reopened.remember_market_credentials("a", saved);
  auto edited = reopened.get("a");
  edited.market_front = "tcp://other-host:1234";
  reopened.save(edited, edited.revision);
  EXPECT_TRUE(keychain->items.empty());
  EXPECT_THROW(reopened.market_credentials(reopened.get("a")), Error);
  reopened.remember_market_credentials("a", saved);
  reopened.remove("a", reopened.get("a").revision);
  EXPECT_TRUE(keychain->items.empty());
}
TEST(CtpConnections, MarketCredentialFailuresNeverExposeSecretsOrWriteAccountPasswords) {
  Directory root;
  auto keychain = std::make_shared<test::MemoryCredentials>();
  terminal::CtpConnections connections(root.path / "accounts", keychain);
  connections.save(simnow("a"), "");
  EXPECT_THROW(connections.remember_market_credentials("a", {"password", ""}), Error);
  EXPECT_TRUE(keychain->items.empty());
  connections.remember_market_credentials("a", {"fixture-password", "fixture-auth"});
  keychain->items.begin()->second = "{\"password\":\"sensitive-broken-value";
  try {
    (void)connections.market_credentials(connections.get("a"));
    FAIL() << "corrupt keychain entry accepted";
  } catch (const Error& error) {
    EXPECT_STREQ(error.what(), "invalid saved market credentials");
  }
  terminal::CtpConnections unavailable(root.path / "accounts");
  EXPECT_THROW(unavailable.remember_market_credentials("a", {"password", "auth"}), Error);
  EXPECT_THROW(unavailable.market_credentials(unavailable.get("a")), Error);
}
