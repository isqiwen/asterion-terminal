#include "credential_fixture.hpp"
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
