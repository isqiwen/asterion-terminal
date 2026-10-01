#include "history_archive.hpp"
#include "history_daily.hpp"
#include "history_minutes.hpp"
#include "bar_dataset_source.hpp"
#include "bar_fixture.hpp"
#include <asterion/kernel/durable_file.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/child.hpp>
#include <gtest/gtest.h>
#include <fstream>
using namespace asterion;
namespace {
struct Folder {
  std::filesystem::path path = std::filesystem::temp_directory_path() / unique_process_id();
  Folder() { std::filesystem::create_directory(path); }
  ~Folder() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
};
// An unrelated provider proves that storage never assumes a Tushare plugin or code.
class AlternateDaily final : public HistoricalDailyPort {
public:
  std::string source = "fixture.exchange", normalization = "fixture.normalization.v1";
  PluginDescriptor descriptor() const override {
    return {"fixture.unrelated", PluginKind::data, plugin_contract_version, {}};
  }
  HistorySemantics semantics() const override {
    return {source, normalization, "Asia/Shanghai", "trading_day"};
  }
  void start() override {}
  void stop() noexcept override {}
  std::vector<HistoricalDailyBar> read(const HistoricalDailyRange& r, std::stop_token) override {
    return {{r.begin,
             test::dec("1.00000001"),
             test::dec("2"),
             test::dec("1"),
             test::dec("2"),
             test::dec("1"),
             test::dec("2"),
             test::dec("1"),
             {},
             {},
             test::dec("2")}};
  }
};
data::v1::HistoryRecord publish(history_files::Archive& archive, AlternateDaily& provider,
                                const HistoryIdentity& identity, const std::string& acquisition) {
  const auto input = history_files::daily_request({{"version", 2},
                                                   {"contract_id", identity.key()},
                                                   {"source", provider.source},
                                                   {"source_instrument", identity.key()},
                                                   {"begin_day", "2024-01-02"},
                                                   {"end_day", "2024-01-02"},
                                                   {"requests_per_minute", 500}});
  const auto path = archive.directory(identity, provider.source, 0, acquisition);
  history_files::download_daily(provider, history_files::daily_range(input), path, 500);
  data::v1::HistoryRecord record;
  record.set_version(1);
  *record.mutable_daily() = input;
  *record.mutable_daily_result() = history_files::daily_result(path);
  archive.publish(record);
  return record;
}
} // namespace
TEST(HistoryIdentity, DistinguishesDecadesAndRejectsAliasesAndTraversal) {
  EXPECT_THROW(validate_history_source(".."), std::invalid_argument);
  EXPECT_THROW(validate_history_source("."), std::invalid_argument);
  auto a = HistoryIdentity::parse("CZCE/sr/2024-01"), b = HistoryIdentity::parse("CZCE/sr/2034-01");
  EXPECT_EQ(a.exchange_id(), b.exchange_id());
  EXPECT_NE(a.key(), b.key());
  EXPECT_THROW(HistoryIdentity::parse("CZCE/SR401"), std::invalid_argument);
  EXPECT_THROW(HistoryIdentity::parse("SHFE/../2024-01"), std::invalid_argument);
  EXPECT_THROW(HistoryIdentity::parse("SHFE/cu/2024-13"), std::invalid_argument);
}
TEST(HistoryArchive, IndependentSourcesContractsVersionsAndRepositoryQueries) {
  Folder root;
  history_files::Archive archive(root.path);
  EXPECT_THROW(archive.directory({"SHFE", "cu", "2024-03"}, "..", 0, "valid"),
               std::invalid_argument);
  EXPECT_THROW(archive.directory({"SHFE", "cu", "2024-03"}, "fixture", 0, ".."),
               std::invalid_argument);
  AlternateDaily provider;
  auto first = publish(archive, provider, {"SHFE", "cu", "2024-03"}, "one");
  provider.source = "fixture.second";
  auto second = publish(archive, provider, {"SHFE", "cu", "2024-03"}, "two");
  publish(archive, provider, {"DCE", "m", "2024-03"}, "three");
  const HistoryStorePort& store = archive;
  EXPECT_EQ(store.datasets({}).size(), 3);
  EXPECT_EQ(store.datasets({"SHFE", "cu", "", ""}).size(), 2);
  EXPECT_EQ(store.datasets({"", "", "SHFE/cu/2024-03", "fixture.second"}).size(), 1);
  EXPECT_NE(first.daily_result().manifest_sha256(), second.daily_result().manifest_sha256());
  data::v1::DailyPageQuery query;
  query.set_dataset_id(first.daily_result().manifest_sha256());
  query.set_limit(10);
  const auto loaded = archive.get(query.dataset_id());
  const auto page = history_files::read_daily_page(loaded.daily(), loaded.daily_result(), query);
  ASSERT_EQ(page.bars_size(), 1);
  EXPECT_EQ(page.bars(0).open().units(), 100000001);
  // Duplicate acquisition reuses content identity while leaving both originals intact.
  provider.source = "fixture.exchange";
  publish(archive, provider, {"SHFE", "cu", "2024-03"}, "four");
  EXPECT_EQ(store.datasets({}).size(), 3);
  history_files::Archive reopened(root.path);
  EXPECT_EQ(reopened.datasets({}).size(), 3);
}
TEST(HistoryArchive, NormalizationChangesCreateNewRevisionAndRejectResume) {
  Folder root;
  history_files::Archive archive(root.path);
  AlternateDaily provider;
  const auto record = publish(archive, provider, {"SHFE", "cu", "2024-03"}, "one");
  provider.normalization = "fixture.normalization.v2";
  EXPECT_THROW(history_files::download_daily(provider, history_files::daily_range(record.daily()),
                                             record.daily_result().directory(), 500),
               std::invalid_argument);
  const auto next = publish(archive, provider, {"SHFE", "cu", "2024-03"}, "two");
  EXPECT_NE(record.daily_result().manifest_sha256(), next.daily_result().manifest_sha256());
  EXPECT_EQ(archive.datasets({}).size(), 2);
}
TEST(HistoryArchive, RejectsUnsupportedVersionAndCorruptChunksWithoutOverwriting) {
  Folder root;
  history_files::Archive archive(root.path);
  AlternateDaily provider;
  const auto record = publish(archive, provider, {"SHFE", "cu", "2024-03"}, "one");
  auto unsupported = record.daily();
  unsupported.set_version(1);
  EXPECT_THROW(history_files::daily_range(unsupported), std::invalid_argument);
  const auto chunk = std::filesystem::path(record.daily_result().directory()) / "daily-0.parquet";
  replace_file_durably(chunk, "corrupted");
  data::v1::DailyPageQuery query;
  query.set_dataset_id(record.daily_result().manifest_sha256());
  query.set_limit(10);
  EXPECT_THROW(history_files::read_daily_page(record.daily(), record.daily_result(), query),
               std::exception);
  EXPECT_THROW(archive.publish(record), std::exception);
  std::ifstream in(chunk);
  std::string content;
  in >> content;
  EXPECT_EQ(content, "corrupted");
}
TEST(HistoryArchive, RejectsUnconfirmedSourceAndPartialDataset) {
  Folder root;
  AlternateDaily provider;
  const HistoricalDailyRange range{{"SHFE", "cu", "2024-03"},
                                   parse_trading_date("2024-01-02"),
                                   parse_trading_date("2024-01-02"),
                                   "fixture.other"};
  EXPECT_THROW(history_files::download_daily(provider, range, root.path, 500),
               std::invalid_argument);
  EXPECT_TRUE(std::filesystem::is_empty(root.path));
}
