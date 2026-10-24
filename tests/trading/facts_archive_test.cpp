#include "facts_archive.hpp"
#include "account_journal.hpp"
#include "trading/journal_fixture.hpp"
#include <asterion/kernel/process/file_lock.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <asterion/foundation/decimal.hpp>
#include <gtest/gtest.h>
#include <fstream>
using namespace asterion;
namespace fs = std::filesystem;

namespace {

struct Folder
{
    fs::path path = fs::temp_directory_path() / ("asterion-facts-" + unique_process_id());

    Folder()
    {
        fs::create_directory(path);
    }

    ~Folder()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

resource::proto::Owner owner()
{
    resource::proto::Owner value;
    value.set_node_id("fixture-node");
    value.set_service_id("trading-one");
    value.set_domain("trading");
    return value;
}

protocol::proto::TradingFactsCapture capture()
{
    protocol::proto::TradingFactsCapture value;
    auto& f = *value.mutable_facts();
    *f.mutable_source() = owner();
    f.set_account_id("fixture-account");
    f.set_environment(protocol::proto::BROKER_SIMULATION);
    f.set_instance("fixture-process");
    f.set_id("fixed-one");
    f.set_captured_at_ms(1790000000000);
    f.set_broker_sequence(9007199254740993ULL);
    f.set_connection_generation(9007199254740995ULL);
    f.set_phase("disconnected");
    f.set_policy_revision("policy-one");
    f.set_risk_artifact(std::string(64, 'a'));
    auto& ref = *value.mutable_reference();
    *ref.mutable_owner() = owner();
    ref.set_kind("trading_facts");
    ref.set_id(f.id());
    ref.set_version(sha256_bytes(f.SerializeAsString()));
    return value;
}

trading::FactsArchive archive(const fs::path& path)
{
    return {path, owner(), "fixture-account", protocol::proto::BROKER_SIMULATION};
}

} // namespace

TEST(TradingFactsArchive, FixedPayloadSurvivesStageAndObjectPublicationRestart)
{
    Folder folder;
    const auto original = capture();
    const auto& ref = original.reference();
    auto first = archive(folder.path);
    first.stage(original);
    EXPECT_THROW(first.read(ref), Error);
    EXPECT_EQ(first.prepared(ref).SerializeAsString(), original.SerializeAsString());
    auto restart = archive(folder.path);
    EXPECT_EQ(restart.publish(ref).SerializeAsString(), original.SerializeAsString());
    EXPECT_EQ(restart.read(ref).SerializeAsString(), original.SerializeAsString());
    EXPECT_EQ(archive(folder.path).publish(ref).SerializeAsString(), original.SerializeAsString());
    EXPECT_FALSE(fs::exists(folder.path / "facts/staging/fixed-one.pb"));
    EXPECT_THROW(trading::FactsArchive::read_published(folder.path, ref), Error)
        << "an immutable object alone is not a confirmed publication";
    protocol::proto::TradingFactsReceipt receipt;
    *receipt.mutable_reference() = ref;
    receipt.set_state(protocol::proto::TradingFactsReceipt::PUBLISHED);
    receipt.set_admitted_at_ms(1790000000000);
    receipt.set_published_at_ms(1790000000001);
    restart.seal(receipt);
    const auto published = trading::FactsArchive::read_published(folder.path, ref);
    EXPECT_EQ(published.operation().SerializeAsString(), receipt.SerializeAsString());
    EXPECT_EQ(published.capture().SerializeAsString(), original.SerializeAsString());
    auto changed_receipt = receipt;
    changed_receipt.set_published_at_ms(1790000000002);
    EXPECT_THROW(restart.seal(changed_receipt), Error);
    const auto decoded = protocol::decode_trading_facts(restart.read(ref));
    EXPECT_EQ(decoded.at("broker_sequence"), "9007199254740993");
    EXPECT_EQ(decoded.at("connection_generation"), "9007199254740995");
    EXPECT_TRUE(decoded.at("funds").is_null());
    EXPECT_FALSE(decoded.at("complete_history").get<bool>());
    EXPECT_FALSE(decoded.at("atomic_ledger_cut").get<bool>());
    EXPECT_FALSE(decoded.at("missing_evidence").empty());
}

TEST(TradingFactsArchive, ConflictingOriginalAndWrongAccountEnvironmentOrOwnerCannotReplaceEvidence)
{
    Folder folder;
    auto original = capture();
    auto store = archive(folder.path);
    store.stage(original);
    auto newer = original;
    newer.mutable_facts()->set_broker_sequence(43);
    newer.mutable_reference()->set_version(sha256_bytes(newer.facts().SerializeAsString()));
    EXPECT_THROW(store.stage(newer), Error);
    EXPECT_EQ(
        store.prepared(original.reference()).SerializeAsString(),
        original.SerializeAsString());
    auto other = owner();
    other.set_service_id("trading-other");
    EXPECT_THROW(
        (trading::FactsArchive(
            folder.path,
            other,
            "fixture-account",
            protocol::proto::BROKER_SIMULATION)
                .prepared(original.reference())),
        Error);
    EXPECT_THROW(
        (trading::FactsArchive(
            folder.path,
            owner(),
            "other-account",
            protocol::proto::BROKER_SIMULATION)
                .prepared(original.reference())),
        Error);
    EXPECT_THROW(
        (trading::FactsArchive(folder.path, owner(), "fixture-account", protocol::proto::REAL)
                .prepared(original.reference())),
        Error);
}

TEST(TradingFactsArchive, MissingOrCorruptOriginalIsNeverRefrozenOrSilentlyRepaired)
{
    Folder folder;
    auto original = capture();
    auto store = archive(folder.path);
    EXPECT_THROW(store.prepared(original.reference()), Error);
    store.stage(original);
    store.publish(original.reference());
    const auto object = folder.path / "facts/objects" / (original.reference().version() + ".pb");
    write_file_durably(object, "corrupt-original");
    EXPECT_THROW(store.read(original.reference()), std::invalid_argument);
    EXPECT_THROW(store.stage(original), std::invalid_argument);
    std::ifstream input(object);
    std::string bytes{std::istreambuf_iterator<char>(input), {}};
    EXPECT_EQ(bytes, "corrupt-original");
    auto wrong = original;
    wrong.mutable_facts()->set_account_id("other-account");
    EXPECT_THROW(protocol::validate_trading_facts(wrong), std::invalid_argument);
}

TEST(TradingFactsArchive, FailedPublicationSyncRetainsTheOriginalForExactRecovery)
{
    Folder folder;
    const auto original = capture();
    auto store = archive(folder.path);
    store.stage(original);
    fs::create_directory(folder.path / "facts/objects");
    fail_next_directory_syncs_for_testing(1);
    EXPECT_THROW(store.publish(original.reference()), std::runtime_error);
    fail_next_directory_syncs_for_testing(0);
    EXPECT_EQ(
        archive(folder.path).prepared(original.reference()).SerializeAsString(),
        original.SerializeAsString());
    EXPECT_EQ(
        archive(folder.path).publish(original.reference()).SerializeAsString(),
        original.SerializeAsString());
}

TEST(TradingEvidenceOwnership, AccountWriterAndStoppedCutExcludeEachOtherBeforeSqliteCoordination)
{
    Folder folder;
    Progress progress;
    const auto post = [](std::function<void()> done) {
        done();
    };
    const auto validate = [](const Json& header) {
        if (header != Json{{"fixture", true}})
            throw std::invalid_argument("unexpected fixture header");
    };
    {
        trading::AccountJournal writer(folder.path, validate, post, progress);
        writer.initialize(Json{{"fixture", true}});
        EXPECT_THROW(
            (void)FileLock(
                folder.path / "facts",
                trading::AccountJournal::ownership_file,
                FileLock::Access::shared_existing),
            std::runtime_error);
    }
    const auto before = sha256_file(folder.path / "journal.sqlite");
    EXPECT_EQ(fs::file_size(folder.path / "facts" / trading::AccountJournal::ownership_file), 0U);
    {
        FileLock cut(
            folder.path / "facts",
            trading::AccountJournal::ownership_file,
            FileLock::Access::shared_existing);
        EXPECT_THROW(
            (void)trading::AccountJournal(folder.path, validate, post, progress),
            std::runtime_error);
        EXPECT_EQ(sha256_file(folder.path / "journal.sqlite"), before);
        EXPECT_FALSE(fs::exists(folder.path / "journal.sqlite-shm"));
        EXPECT_FALSE(fs::exists(folder.path / "journal.sqlite-wal"));
    }
    {
        trading::AccountJournal writer(folder.path, validate, post, progress);
        EXPECT_EQ(writer.initial_capacity().total_records, 1U);
    }
    Folder invalid;
    {
        SqliteJournal seed(invalid.path);
        seed.start();
        seed.append(Json{{"foreign", true}});
    }
    const auto invalid_before = sha256_file(invalid.path / "journal.sqlite");
    EXPECT_THROW(
        (void)trading::AccountJournal(invalid.path, validate, post, progress),
        std::invalid_argument);
    EXPECT_FALSE(fs::exists(invalid.path / "facts"));
    EXPECT_EQ(sha256_file(invalid.path / "journal.sqlite"), invalid_before);
}

TEST(TradingEvidenceInspection, FixedDecisionsUseTheSameReplayAndLifetimeIndexChecksWithoutAWriter)
{
    Folder folder;
    test::SmallJournalSegments budget(2);
    auto receipt = protocol::proto::TradingFactsReceipt{};
    *receipt.mutable_reference() = capture().reference();
    receipt.set_admitted_at_ms(1790000000000);
    {
        SqliteJournal seed(folder.path);
        seed.start();
        seed.append(Json{{"fixture", true}});
        for (const auto* id : {"fixed-one", "fixed-two"}) {
            receipt.mutable_reference()->set_id(id);
            receipt.set_state(protocol::proto::TradingFactsReceipt::ADMITTED);
            receipt.set_published_at_ms(0);
            auto admission = trading::entry(trading::FactsDecision{receipt});
            seed.append(admission.body, admission.command_id);
            receipt.set_state(protocol::proto::TradingFactsReceipt::PUBLISHED);
            receipt.set_published_at_ms(1790000000001);
            auto published = trading::entry(trading::FactsDecision{receipt});
            seed.append(published.body, published.command_id);
        }
    }
    fs::create_directory(folder.path / "facts");
    {
        FileLock initial(folder.path / "facts", trading::AccountJournal::ownership_file);
    }
    FileLock cut(
        folder.path / "facts",
        trading::AccountJournal::ownership_file,
        FileLock::Access::shared_existing);
    const auto before = sha256_file(folder.path / "journal.sqlite");
    unsigned decisions = 0;
    const auto inspect = [&] {
        return trading::AccountJournal::inspect_backup(
            folder.path,
            [](const Json& header) {
                EXPECT_EQ(header, (Json{{"fixture", true}}));
            },
            [&](const trading::JournalRecord& record) {
                EXPECT_TRUE(std::holds_alternative<trading::FactsDecision>(record));
                ++decisions;
            });
    };
    const auto files = inspect();
    EXPECT_EQ(decisions, 4U);
    EXPECT_EQ(files.size(), 2U);
    EXPECT_EQ(sha256_file(folder.path / "journal.sqlite"), before);
    EXPECT_FALSE(fs::exists(folder.path / "journal.sqlite-shm"));
    {
        sqlite::Database corrupt(folder.path / "journal.sqlite");
        sqlite::Database::Statement index(
            corrupt,
            "UPDATE commands SET id='unknown-fixture-key' WHERE id=?");
        index
            .bind(
                1,
                trading::facts_decision_key(
                    "fixed-one",
                    protocol::proto::TradingFactsReceipt::ADMITTED))
            .step();
    }
    EXPECT_THROW((void)inspect(), std::invalid_argument);
}
