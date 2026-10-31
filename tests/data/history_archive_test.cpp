#include "history_archive.hpp"
#include "data/history_requests.hpp"
#include "history_minutes.hpp"
#include "bar_dataset_source.hpp"
#include "data/history_fixture.hpp"
#include "data_store.hpp"
#include <asterion/system/durable_file.hpp>
#include <asterion/system/process/artifact.hpp>
#include <asterion/system/process/child.hpp>
#include <gtest/gtest.h>
#include <fstream>
using namespace asterion;

namespace {

struct Folder
{
    std::filesystem::path path = std::filesystem::temp_directory_path() / unique_process_id();

    Folder()
    {
        std::filesystem::create_directory(path);
    }

    ~Folder()
    {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

// An unrelated provider proves that storage never assumes a Tushare plugin or code.
class AlternateMinutes final : public HistoricalBarPort
{
public:
    std::string source = "fixture.exchange", normalization = "fixture.normalization.v1";

    HistorySemantics semantics() const override
    {
        return {source, normalization, "Asia/Shanghai", "bar_end"};
    }

    std::vector<HistoricalBar> read(const HistoricalBarRange& r, std::stop_token) override
    {
        return {
            {r.begin_ns,
                test::dec("1.00000001"),
                test::dec("2"),
                test::dec("1"),
                test::dec("2"),
                test::dec("1"),
                test::dec("2"),
                test::dec("1"),
                "2024-01-02"}
        };
    }
};

// Where a saved selection reads its daily facts; nothing is read of it here.
data::proto::DailyTable daily_table()
{
    return protocol::encode_daily_table(
        {{"snapshot", std::string(64, 'd')},
            {"daily_source", "fixture.daily.v1"},
            {"calendar_source", "fixture.calendar.v1"},
            {"contracts_source", "fixture.contracts.v1"}});
}

// The one bar of 2024-01-02 09:01.
const auto minute = parse_shanghai_time("2024-01-02 09:01:00");

data::proto::HistoryRecord publish(
    history_files::Archive& archive,
    AlternateMinutes& provider,
    const HistoryIdentity& identity,
    const std::string& acquisition)
{
    const auto input = asterion::testing_support::minute_request(
        {{"contract_id", identity.key()},
            {"source", provider.source},
            {"source_instrument", identity.key()},
            {"interval_minutes", 1},
            {"begin_ns", std::to_string(minute)},
            {"end_ns", std::to_string(minute)},
            {"requests_per_minute", 500}});
    const auto path = archive.directory(identity, provider.source, 1, acquisition);
    history_files::download_minutes(provider, history_files::minute_range(input), path);
    data::proto::HistoryRecord record;
    *record.mutable_minutes() = input;
    *record.mutable_minute_result() = history_files::minute_result(path);
    history_files::verify_minute_result(record.minutes(), record.minute_result());
    archive.publish_verified(record);
    return record;
}

data::proto::MinutePageQuery first_page(const data::proto::HistoryRecord& record)
{
    data::proto::MinutePageQuery query;
    query.set_dataset_id(record.minute_result().manifest_sha256());
    query.set_limit(10);
    return query;
}

} // namespace

TEST(HistoryIdentity, DistinguishesDecadesAndRejectsAliasesAndTraversal)
{
    EXPECT_THROW(validate_history_source(".."), std::invalid_argument);
    EXPECT_THROW(validate_history_source("."), std::invalid_argument);
    auto a = HistoryIdentity::parse("CZCE/sr/2024-01"),
         b = HistoryIdentity::parse("CZCE/sr/2034-01");
    EXPECT_EQ(a.exchange_id(), b.exchange_id());
    EXPECT_NE(a.key(), b.key());
    EXPECT_THROW(HistoryIdentity::parse("CZCE/SR401"), std::invalid_argument);
    EXPECT_THROW(HistoryIdentity::parse("SHFE/../2024-01"), std::invalid_argument);
    EXPECT_THROW(HistoryIdentity::parse("SHFE/cu/2024-13"), std::invalid_argument);
}

TEST(HistoryArchive, IndependentSourcesContractsVersionsAndRepositoryQueries)
{
    Folder root;
    history_files::Archive archive(root.path);
    EXPECT_THROW(
        archive.directory({"SHFE", "cu", "2024-03"}, "..", 0, "valid"),
        std::invalid_argument);
    EXPECT_THROW(
        archive.directory({"SHFE", "cu", "2024-03"}, "fixture", 0, ".."),
        std::invalid_argument);
    AlternateMinutes provider;
    auto first = publish(archive, provider, {"SHFE", "cu", "2024-03"}, "one");
    provider.source = "fixture.second";
    auto second = publish(archive, provider, {"SHFE", "cu", "2024-03"}, "two");
    publish(archive, provider, {"DCE", "m", "2024-03"}, "three");
    const auto& store = archive;
    EXPECT_EQ(store.datasets({}).size(), 3);
    EXPECT_EQ(store.datasets({"SHFE", "cu", "", ""}).size(), 2);
    EXPECT_EQ(store.datasets({"", "", "SHFE/cu/2024-03", "fixture.second"}).size(), 1);
    EXPECT_NE(first.minute_result().manifest_sha256(), second.minute_result().manifest_sha256());
    const auto loaded = archive.get(first.minute_result().manifest_sha256());
    const auto page = history_files::read_minute_page(
        loaded.minutes(),
        loaded.minute_result(),
        first_page(first));
    ASSERT_EQ(page.bars_size(), 1);
    EXPECT_EQ(page.bars(0).open().units(), 100000001);
    // A new acquisition has independent time evidence; both originals remain intact.
    provider.source = "fixture.exchange";
    const auto repeated = publish(archive, provider, {"SHFE", "cu", "2024-03"}, "four");
    EXPECT_NE(repeated.minute_result().manifest_sha256(), first.minute_result().manifest_sha256());
    EXPECT_EQ(store.datasets({}).size(), 4);
    history_files::Archive reopened(root.path);
    EXPECT_EQ(reopened.datasets({}).size(), 4);
}

TEST(HistoryArchive, NormalizationChangesCreateNewRevisionAndRejectResume)
{
    Folder root;
    history_files::Archive archive(root.path);
    AlternateMinutes provider;
    const auto record = publish(archive, provider, {"SHFE", "cu", "2024-03"}, "one");
    provider.normalization = "fixture.normalization.v2";
    EXPECT_THROW(
        history_files::download_minutes(
            provider,
            history_files::minute_range(record.minutes()),
            record.minute_result().directory()),
        std::invalid_argument);
    const auto next = publish(archive, provider, {"SHFE", "cu", "2024-03"}, "two");
    EXPECT_NE(record.minute_result().manifest_sha256(), next.minute_result().manifest_sha256());
    EXPECT_EQ(archive.datasets({}).size(), 2);
}

TEST(HistoryArchive, UnsynchronizedAncestorsCannotPublishADatasetReference)
{
    Folder root;
    history_files::Archive archive(root.path / "history");
    AlternateMinutes provider;
    fail_next_directory_syncs_for_testing(1);
    EXPECT_THROW(publish(archive, provider, {"SHFE", "cu", "2024-03"}, "one"), std::runtime_error);
    EXPECT_TRUE(archive.datasets({}).empty());
    // The retry synchronizes the directory left by the failed attempt before
    // writing any dataset or authoritative index record.
    const auto record = publish(archive, provider, {"SHFE", "cu", "2024-03"}, "one");
    EXPECT_EQ(archive.datasets({}).size(), 1U);
    EXPECT_EQ(
        archive.get(record.minute_result().manifest_sha256()).SerializeAsString(),
        record.SerializeAsString());
    // Reconfirming an existing record must complete its directory barrier too.
    fail_next_directory_syncs_for_testing(1);
    EXPECT_THROW(archive.publish_verified(record), std::runtime_error);
    fail_next_directory_syncs_for_testing(0);
    EXPECT_NO_THROW(archive.publish_verified(record));
}

TEST(HistoryArchive, RejectsCorruptChunksWithoutOverwriting)
{
    Folder root;
    history_files::Archive archive(root.path);
    AlternateMinutes provider;
    const auto record = publish(archive, provider, {"SHFE", "cu", "2024-03"}, "one");
    const auto chunk =
        std::filesystem::path(record.minute_result().directory()) / "minutes-0.parquet";
    replace_file_durably(chunk, "corrupted");
    const auto query = first_page(record);
    EXPECT_THROW(
        history_files::read_minute_page(record.minutes(), record.minute_result(), query),
        std::exception);
    // Republishing a known version checks only its index record: corruption is
    // reported when the data is read, and nothing is overwritten.
    EXPECT_NO_THROW(archive.publish_verified(record));
    EXPECT_THROW(
        history_files::read_minute_page(record.minutes(), record.minute_result(), query),
        std::exception);
    std::ifstream in(chunk);
    std::string content;
    in >> content;
    EXPECT_EQ(content, "corrupted");
}

TEST(HistoryArchive, RejectsUnconfirmedSourceAndPartialDataset)
{
    Folder root;
    AlternateMinutes provider;
    const HistoricalBarRange range{{"SHFE", "cu", "2024-03"}, 1, minute, minute, "fixture.other"};
    EXPECT_THROW(
        history_files::download_minutes(provider, range, root.path),
        std::invalid_argument);
    EXPECT_TRUE(std::filesystem::is_empty(root.path));
}

TEST(HistoryArchive, DataResolvesPublishedVersionsWithoutDownloadTasks)
{
    Folder root;
    data::Store store(root.path, "historical-data");
    history_files::Archive archive(root.path / "history");
    AlternateMinutes provider;
    const auto original = publish(archive, provider, {"SHFE", "cu", "2024-03"}, "first");
    const auto id = original.minute_result().manifest_sha256();
    const auto before = archive.get(id);
    EXPECT_EQ(before.minute_result().manifest_sha256(), id);
    // A later publication is another version: the first reads as it did.
    provider.normalization = "fixture.normalization.v2";
    const auto newer = publish(archive, provider, {"SHFE", "cu", "2024-03"}, "second");
    EXPECT_NE(newer.minute_result().manifest_sha256(), id);
    EXPECT_EQ(archive.get(id).SerializeAsString(), before.SerializeAsString());
    // A selection names versions by their identity, not by the task that made them.
    data::proto::BarDatasetRequest request;
    request.add_source_dataset_ids("first");
    *request.mutable_daily() = daily_table();
    request.set_contract("SHFE/cu/2024-03");
    EXPECT_THROW(store.sources(request), std::invalid_argument);
}

TEST(HistoryArchive, NamedDatasetsAreImmutableAndSurviveRestart)
{
    Folder root;
    history_files::Archive archive(root.path / "history");
    AlternateMinutes provider;
    const auto original = publish(archive, provider, {"SHFE", "cu", "2024-03"}, "named");
    data::proto::NamedDataset saved;
    saved.set_name("铜分钟研究");
    auto* input = saved.add_selections();
    input->add_source_dataset_ids(original.minute_result().manifest_sha256());
    *input->mutable_daily() = daily_table();
    input->set_contract("SHFE/cu/2024-03");
    saved.add_content_revisions(std::string(64, 'a'));
    saved.set_id(protocol::named_dataset_revision(saved));
    archive.save_named_dataset(saved);
    fail_next_directory_syncs_for_testing(1);
    EXPECT_THROW(archive.save_named_dataset(saved), std::runtime_error);
    fail_next_directory_syncs_for_testing(0);
    archive.save_named_dataset(saved);
    ASSERT_EQ(archive.named_datasets().items_size(), 1);
    auto changed = saved;
    changed.mutable_selections(0)->set_end_day("2024-03-15");
    changed.set_id(protocol::named_dataset_revision(changed));
    EXPECT_NE(changed.id(), saved.id());
    archive.save_named_dataset(changed);
    history_files::Archive restored(root.path / "history");
    EXPECT_EQ(restored.named_datasets().items_size(), 2);
    EXPECT_EQ(restored.named_dataset(saved.id()).SerializeAsString(), saved.SerializeAsString());
    auto corrupted = saved;
    corrupted.set_name("unexpected rewrite");
    replace_file_durably(
        root.path / "history" / "named-datasets" / (saved.id() + ".pb"),
        corrupted.SerializeAsString());
    EXPECT_THROW(restored.named_dataset(saved.id()), std::invalid_argument);
}

TEST(HistoryArchive, NamedDatasetsRejectInvalidNamesDuplicateContractsAndMissingVersions)
{
    Folder root;
    history_files::Archive archive(root.path / "history");
    data::proto::NamedDataset value;
    value.set_name("test");
    auto* input = value.add_selections();
    input->add_source_dataset_ids(std::string(64, 'a'));
    *input->mutable_daily() = daily_table();
    input->set_contract("SHFE/rb/2026-10");
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

TEST(HistoryArchive, ReadOnlyInspectionCannotCreateOrPublishState)
{
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

TEST(HistoryArchive, IndependentCopyResolvesOwnedLocationWithoutRewritingPublicationEvidence)
{
    Folder source, destination;
    const auto history = source.path / "history";
    data::proto::HistoryRecord original;
    {
        history_files::Archive archive(history);
        AlternateMinutes provider;
        original = publish(archive, provider, {"SHFE", "cu", "2024-03"}, "restorable");
    }
    const auto id = original.minute_result().manifest_sha256();
    const auto index = history / "index" / (id + ".pb");
    const auto before = sha256_file(index);
    const auto restored_root = destination.path / "history";
    std::filesystem::copy(history, restored_root, std::filesystem::copy_options::recursive);
    std::filesystem::rename(
        history,
        source.path / "original-preserved"); // Old absolute locator is unavailable.
    history_files::Archive restored(restored_root, history_files::Archive::Access::read_only);
    const auto record = restored.get(id);
    EXPECT_EQ(record.minutes().SerializeAsString(), original.minutes().SerializeAsString());
    EXPECT_NE(record.minute_result().directory(), original.minute_result().directory());
    EXPECT_TRUE(std::filesystem::is_directory(record.minute_result().directory()));
    std::uint64_t rows = 0;
    history_files::read_minutes(record.minute_result().directory(), [&](const HistoricalBar&) {
        ++rows;
    });
    EXPECT_EQ(rows, record.minute_result().rows());
    EXPECT_EQ(sha256_file(restored_root / "index" / (id + ".pb")), before);
    EXPECT_EQ(sha256_file(source.path / "original-preserved" / "index" / (id + ".pb")), before);
    auto corrupted = original;
    corrupted.mutable_minute_result()->set_directory(
        (restored_root / "wrong-source" / "restorable").string());
    replace_file_durably(restored_root / "index" / (id + ".pb"), corrupted.SerializeAsString());
    EXPECT_THROW(restored.get(id), std::invalid_argument);
}
