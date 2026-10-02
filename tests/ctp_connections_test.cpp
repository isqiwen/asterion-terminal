#include "ctp_connections.hpp"
#include <asterion/foundation/error.hpp>
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
  // An account without a market front cannot supply market data.
  auto trading_only = simnow("t");
  trading_only.market_front.clear();
  connections.save(trading_only, "");
  EXPECT_FALSE(connections.market());
  EXPECT_THROW(connections.select_market("t"), std::invalid_argument);
  connections.save(simnow("a"), "");
  connections.save(simnow("b"), "");
  ASSERT_TRUE(connections.market());
  EXPECT_EQ(connections.market()->id, "a");
  EXPECT_EQ(connections.snapshot().size(), 3) << "the choice is not listed as an account";
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
           [](auto& c) { c.trade_front = "tcp://bad host:1"; },
           [](auto& c) {
             c.trade_front.clear();
             c.market_front.clear();
           }}) {
    auto connection = simnow("bad");
    mutate(connection);
    EXPECT_THROW(connections.save(connection, ""), std::invalid_argument);
  }
  EXPECT_THROW(connections.save(simnow("../escape"), ""), std::invalid_argument);
  EXPECT_TRUE(connections.snapshot().empty());
  // Market data only: no application identifier and no trade front.
  auto market_only = simnow("market");
  market_only.app_id.clear();
  market_only.trade_front.clear();
  connections.save(market_only, "");
  EXPECT_TRUE(connections.get("market").trade_front.empty());
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
