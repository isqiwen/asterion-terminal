#include "history_archive.hpp"
#include "history_daily.hpp"
#include "history_minutes.hpp"
#include "bar_dataset_source.hpp"
#include "bar_fixture.hpp"
#include "data_store.hpp"
#include "daily_factor_source.hpp"
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
  HistorySemantics semantics() const override {
    return {source, normalization, "Asia/Shanghai", "trading_day"};
  }
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
  history_files::download_daily(provider, history_files::daily_range(input), path);
  data::v1::HistoryRecord record;
  record.set_version(1);
  *record.mutable_daily() = input;
  *record.mutable_daily_result() = history_files::daily_result(path);
  history_files::verify_daily_result(record.daily(), record.daily_result());
  archive.publish_verified(record);
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
  const auto& store = archive;
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
  // A new acquisition has independent time evidence; both originals remain intact.
  provider.source = "fixture.exchange";
  const auto repeated = publish(archive, provider, {"SHFE", "cu", "2024-03"}, "four");
  EXPECT_NE(repeated.daily_result().manifest_sha256(), first.daily_result().manifest_sha256());
  EXPECT_EQ(store.datasets({}).size(), 4);
  history_files::Archive reopened(root.path);
  EXPECT_EQ(reopened.datasets({}).size(), 4);
}
TEST(HistoryArchive, NormalizationChangesCreateNewRevisionAndRejectResume) {
  Folder root;
  history_files::Archive archive(root.path);
  AlternateDaily provider;
  const auto record = publish(archive, provider, {"SHFE", "cu", "2024-03"}, "one");
  provider.normalization = "fixture.normalization.v2";
  EXPECT_THROW(history_files::download_daily(provider, history_files::daily_range(record.daily()),
                                             record.daily_result().directory()),
               std::invalid_argument);
  const auto next = publish(archive, provider, {"SHFE", "cu", "2024-03"}, "two");
  EXPECT_NE(record.daily_result().manifest_sha256(), next.daily_result().manifest_sha256());
  EXPECT_EQ(archive.datasets({}).size(), 2);
}
TEST(HistoryArchive, UnsynchronizedAncestorsCannotPublishADatasetReference) {
  Folder root;
  history_files::Archive archive(root.path / "history");
  AlternateDaily provider;
  fail_next_directory_syncs_for_testing(1);
  EXPECT_THROW(publish(archive, provider, {"SHFE", "cu", "2024-03"}, "one"), std::runtime_error);
  EXPECT_TRUE(archive.datasets({}).empty());
  // The retry synchronizes the directory left by the failed attempt before
  // writing any dataset or authoritative index record.
  const auto record = publish(archive, provider, {"SHFE", "cu", "2024-03"}, "one");
  EXPECT_EQ(archive.datasets({}).size(), 1U);
  EXPECT_EQ(archive.get(record.daily_result().manifest_sha256()).SerializeAsString(),
            record.SerializeAsString());
  // Reconfirming an existing record must complete its directory barrier too.
  fail_next_directory_syncs_for_testing(1);
  EXPECT_THROW(archive.publish_verified(record), std::runtime_error);
  fail_next_directory_syncs_for_testing(0);
  EXPECT_NO_THROW(archive.publish_verified(record));
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
  // Republishing a known version checks only its index record: corruption is
  // reported when the data is read, and nothing is overwritten.
  EXPECT_NO_THROW(archive.publish_verified(record));
  EXPECT_THROW(history_files::read_daily_page(record.daily(), record.daily_result(), query),
               std::exception);
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
  EXPECT_THROW(history_files::download_daily(provider, range, root.path), std::invalid_argument);
  EXPECT_TRUE(std::filesystem::is_empty(root.path));
}

TEST(HistoryArchive, DataResolvesPublishedVersionsWithoutDownloadTasks) {
  Folder root;
  data::Store store(root.path, "historical-data", "task");
  history_files::Archive archive(root.path / "history");
  AlternateDaily provider;
  const auto original = publish(archive, provider, {"SHFE", "cu", "2024-03"}, "first");
  const auto id = original.daily_result().manifest_sha256();
  auto contract = test::contract();
  contract.set_venue("SHFE");
  contract.set_symbol("cu2403");
  contract.set_product("cu");
  contract.set_delivery_month("2024-03");
  contract.mutable_price_increment()->set_units(1);
  data::v1::BarDatasetRequest request;
  request.add_source_dataset_ids(id);
  request.add_settlement_dataset_ids(id);
  *request.mutable_contract() = contract;
  const auto captured = store.sources(request);
  const auto before = data::resolve_bar_dataset(captured);
  EXPECT_EQ(before.bars_size(), 1);
  EXPECT_EQ(before.source_dataset_ids(0), id);
  provider.normalization = "fixture.normalization.v2";
  const auto newer = publish(archive, provider, {"SHFE", "cu", "2024-03"}, "second");
  EXPECT_NE(newer.daily_result().manifest_sha256(), id);
  EXPECT_EQ(data::resolve_bar_dataset(store.sources(request)).SerializeAsString(),
            before.SerializeAsString());
  EXPECT_EQ(data::daily_factor_dataset(archive.get(id)).source_dataset_id(), id);
  request.add_source_dataset_ids("first");
  EXPECT_THROW(store.sources(request), std::invalid_argument);
}

TEST(HistoryArchive, NamedDatasetsAreImmutableAndSurviveRestart) {
  Folder root;
  history_files::Archive archive(root.path / "history");
  AlternateDaily provider;
  const auto original = publish(archive, provider, {"SHFE", "cu", "2024-03"}, "named");
  data::v1::NamedDataset saved;
  saved.set_version(1);
  saved.set_name("铜日线研究");
  auto* input = saved.add_selections();
  input->add_source_dataset_ids(original.daily_result().manifest_sha256());
  input->add_settlement_dataset_ids(original.daily_result().manifest_sha256());
  *input->mutable_contract() = test::contract("SHFE", "cu2403", "cu", "2024-03");
  saved.add_content_revisions(std::string(64, 'a'));
  saved.set_id(protocol::named_dataset_revision(saved));
  archive.save_named_dataset(saved);
  fail_next_directory_syncs_for_testing(1);
  EXPECT_THROW(archive.save_named_dataset(saved), std::runtime_error);
  fail_next_directory_syncs_for_testing(0);
  archive.save_named_dataset(saved);
  ASSERT_EQ(archive.named_datasets().items_size(), 1);
  auto changed = saved;
  changed.mutable_selections(0)->mutable_contract()->mutable_multiplier()->set_units(
      Decimal::parse("20").raw());
  changed.set_id(protocol::named_dataset_revision(changed));
  EXPECT_NE(changed.id(), saved.id());
  archive.save_named_dataset(changed);
  history_files::Archive restored(root.path / "history");
  EXPECT_EQ(restored.named_datasets().items_size(), 2);
  EXPECT_EQ(restored.named_dataset(saved.id()).SerializeAsString(), saved.SerializeAsString());
  auto corrupted = saved;
  corrupted.set_name("unexpected rewrite");
  replace_file_durably(root.path / "history" / "named-datasets" / (saved.id() + ".pb"),
                       corrupted.SerializeAsString());
  EXPECT_THROW(restored.named_dataset(saved.id()), std::invalid_argument);
}

TEST(HistoryArchive, NamedDatasetsRejectInvalidNamesDuplicateContractsAndMissingVersions) {
  Folder root;
  history_files::Archive archive(root.path / "history");
  data::v1::NamedDataset value;
  value.set_version(1);
  value.set_name("test");
  auto* input = value.add_selections();
  input->add_source_dataset_ids(std::string(64, 'a'));
  input->add_settlement_dataset_ids(std::string(64, 'b'));
  *input->mutable_contract() = test::contract();
  value.add_content_revisions(std::string(64, 'c'));
  value.set_id(protocol::named_dataset_revision(value));
  EXPECT_THROW(archive.save_named_dataset(value), std::invalid_argument);
  const auto valid = value;
  *value.add_selections() = valid.selections(0);
  value.add_content_revisions(std::string(64, 'c'));
  value.set_id(protocol::named_dataset_revision(value));
  EXPECT_THROW(protocol::validate_named_dataset(value), std::invalid_argument);
  for (const std::string name : {"", " trailing ", "line\nbreak"}) {
    value = valid;
    value.set_name(name);
    value.set_id(protocol::named_dataset_revision(value));
    EXPECT_THROW(protocol::validate_named_dataset(value), std::invalid_argument);
  }
  EXPECT_THROW(archive.named_dataset("../outside"), std::invalid_argument);
  EXPECT_EQ(archive.named_datasets().items_size(), 0);
}

TEST(HistoryArchive, ReadOnlyInspectionCannotCreateOrPublishState) {
  Folder root;
  {
    history_files::Archive writer(root.path / "history");
  }
  history_files::Archive reader(root.path / "history", history_files::Archive::Access::read_only);
  EXPECT_THROW(reader.publish_verified({}), std::logic_error);
  EXPECT_THROW(reader.save_named_dataset({}), std::logic_error);
  EXPECT_THROW(reader.directory({}, "", 0, ""), std::logic_error);
  EXPECT_THROW(
      history_files::Archive(root.path / "missing", history_files::Archive::Access::read_only),
      std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(root.path / "missing"));
}
