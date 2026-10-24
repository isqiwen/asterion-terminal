#include "trading_backup.hpp"
#include "domain_backup.hpp"
#include "research_backup.hpp"
#include "research_execution.hpp"
#include "program_files.hpp"
#include "task_store.hpp"
#include "tasks/task_store_support.hpp"
#include <asterion/protocol/account_review.hpp>
#include "account_journal.hpp"
#include "facts_archive.hpp"
#include "risk_module.hpp"
#include "trading/journal_fixture.hpp"
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <gtest/gtest.h>
#include <algorithm>
using namespace asterion;
namespace fs = std::filesystem;

namespace {

struct Folder
{
    fs::path path = fs::temp_directory_path() / ("asterion-trading-backup-" + unique_process_id());

    Folder()
    {
        fs::create_directory(path);
    }

    ~Folder()
    {
        std::error_code error;
        fs::remove_all(path, error);
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

Json manifest()
{
    return {
        {"account_id", "fixture-account"},
        {"type", "live_ctp"},
        {"environment", "simulation"},
        {"broker",
            {{"front", "tcp://127.0.0.1:41205"},
                {"broker_id", "9999"},
                {"user_id", "000001"},
                {"app_id", "test-app"}}},
        {"policy",
            {{"risk",
                 {{"max_order_quantity", "5"},
                     {"max_gross_quantity", "10"},
                     {"max_working_orders", 2}}},
                {"max_price_deviation", "0.02"},
                {"contracts",
                    Json::array(
                        {{{"venue", "SHFE"},
                            {"symbol", "rb2610"},
                            {"currency", "CNY"},
                            {"price_increment", "1"},
                            {"multiplier", "10"},
                            {"quantity_increment", "1"},
                            {"product", "rb"},
                            {"delivery_month", "2026-10"}}})}}}
    };
}

protocol::proto::TradingFactsCapture capture(const std::string& id, const std::string& artifact)
{
    protocol::proto::TradingFactsCapture result;
    auto& facts = *result.mutable_facts();
    *facts.mutable_source() = owner();
    facts.set_account_id("fixture-account");
    facts.set_environment(protocol::proto::BROKER_SIMULATION);
    facts.set_instance("fixture-instance");
    facts.set_id(id);
    facts.set_captured_at_ms(1790000000000);
    facts.set_phase("disconnected");
    facts.set_policy_revision("policy-one");
    facts.set_risk_artifact(artifact);
    auto& ref = *result.mutable_reference();
    *ref.mutable_owner() = owner();
    ref.set_kind("trading_facts");
    ref.set_id(id);
    ref.set_version(sha256_bytes(facts.SerializeAsString()));
    return result;
}

struct Seed
{
    protocol::proto::TradingFactsStatus published;
    resource::proto::Object pending;
};

Seed seed(const fs::path& root)
{
    fs::create_directory(root);
    const auto risk = risk_providers::Module::selected();
    fs::create_directories(root / "plugins" / risk.artifact());
    risk.capture(root / "plugins" / risk.artifact());
    Progress progress;
    const auto validate = [](const Json& header) {
        (void)trading::account_manifest(header);
    };
    const auto post = [](std::function<void()> done) {
        done();
    };
    {
        trading::AccountJournal journal(root, validate, post, progress);
        journal.initialize(
            {{"manifest", manifest()},
                {"account_id", "fixture-account"},
                {"policy_revision", "policy-one"},
                {"risk_artifact", risk.artifact()}});
    }
    Seed result;
    test::SmallJournalSegments budget(2);
    FileLock writer(
        root / "facts",
        trading::AccountJournal::ownership_file,
        FileLock::Access::exclusive_existing);
    SqliteJournal journal(root, {"plugins", "ctp-flow", "archives", "facts"});
    journal.start();
    trading::FactsArchive archive(
        root,
        owner(),
        "fixture-account",
        protocol::proto::BROKER_SIMULATION);
    const auto append = [&](const protocol::proto::TradingFactsReceipt& receipt) {
        const auto record = trading::entry(trading::FactsDecision{receipt});
        journal.append(record.body, record.command_id);
    };
    auto fixed = capture("fixed-one", risk.artifact());
    auto& receipt = *result.published.mutable_operation();
    *receipt.mutable_reference() = fixed.reference();
    receipt.set_state(protocol::proto::TradingFactsReceipt::ADMITTED);
    receipt.set_admitted_at_ms(1790000000000);
    append(receipt);
    archive.stage(fixed);
    (void)archive.publish(fixed.reference());
    receipt.set_state(protocol::proto::TradingFactsReceipt::PUBLISHED);
    receipt.set_published_at_ms(1790000000001);
    append(receipt);
    archive.seal(receipt);
    *result.published.mutable_capture() = fixed;
    auto missing = capture("pending-one", risk.artifact());
    result.pending = missing.reference();
    protocol::proto::TradingFactsReceipt admission;
    *admission.mutable_reference() = result.pending;
    admission.set_state(protocol::proto::TradingFactsReceipt::ADMITTED);
    admission.set_admitted_at_ms(1790000000002);
    append(admission);
    return result;
}

} // namespace

TEST(
    TradingColdBackup,
    IndependentRestorePreservesFixedFactsLedgerAndProgramsWithoutRestoringRuntime)
{
    Folder folder;
    const auto root = folder.path / "source";
    const auto original = seed(root);
    fs::create_directory(root / "ctp-flow");
    replace_file_durably(root / "ctp-flow/runtime.bin", "isolated-runtime-cache");
    const auto before = sha256_file(root / "journal.sqlite");
    const auto destination = folder.path / "backup";
    {
        auto exported = trading::cold_backup(root, owner());
        EXPECT_EQ(exported.objects.size(), 2U);
        EXPECT_TRUE(
            std::ranges::find(exported.snapshot.limitations, "admitted_facts_payload_missing")
            != exported.snapshot.limitations.end());
        EXPECT_TRUE(std::ranges::all_of(exported.snapshot.dependencies, [](const auto& ref) {
            return ref.included;
        }));
        Progress progress;
        EXPECT_THROW(
            (void)trading::AccountJournal(
                root,
                [](const Json& h) {
                    (void)trading::account_manifest(h);
                },
                [](std::function<void()> done) {
                    done();
                },
                progress),
            std::runtime_error);
        std::vector<backup_files::Snapshot> snapshots;
        snapshots.push_back(std::move(exported.snapshot));
        backup_files::Cut cut(std::move(snapshots));
        const auto plan = cut.preview("trading-cut");
        EXPECT_TRUE(std::ranges::any_of(plan.at("scopes"), [](const auto& field) {
            return field.at("key") == "account_id" && field.at("value") == "fixture-account";
        }));
        EXPECT_TRUE(std::ranges::any_of(plan.at("scopes"), [](const auto& field) {
            return field.at("key") == "environment" && field.at("value") == "simulation";
        }));
        EXPECT_EQ(plan.at("files").dump().find("runtime.bin"), std::string::npos);
        cut.save("trading-cut", destination, sha256_bytes(plan.dump()));
    }
    EXPECT_EQ(sha256_file(root / "journal.sqlite"), before);
    const auto restore = folder.path / "restore";
    backup_files::restore_files(destination, restore);
    fs::rename(root, folder.path / "preserved-original");
    const auto restored = restore / "owners/0";
    EXPECT_EQ(sha256_file(restored / "journal.sqlite"), before);
    auto inspected = trading::cold_backup(restored, owner());
    EXPECT_EQ(inspected.objects.size(), 2U);
    EXPECT_EQ(
        trading::FactsArchive::read_published(restored, original.published.operation().reference())
            .SerializeAsString(),
        original.published.SerializeAsString());
    trading::FactsArchive archive(
        restored,
        owner(),
        "fixture-account",
        protocol::proto::BROKER_SIMULATION);
    EXPECT_THROW(archive.prepared(original.pending), Error);
    EXPECT_FALSE(fs::exists(restored / "ctp-flow"));
    EXPECT_FALSE(fs::exists(restored / "journal.sqlite-shm"));
    EXPECT_FALSE(fs::exists(folder.path / "account-owners"));
}

TEST(
    TradingColdBackup,
    RunningWriterForeignOwnerAndDamagedPublishedOriginalCannotBePassedOffAsValid)
{
    Folder folder;
    const auto root = folder.path / "source";
    const auto original = seed(root);
    Progress progress;
    {
        trading::AccountJournal writer(
            root,
            [](const Json& h) {
                (void)trading::account_manifest(h);
            },
            [](std::function<void()> done) {
                done();
            },
            progress);
        EXPECT_THROW(trading::cold_backup(root, owner()), std::runtime_error);
    }
    auto wrong = owner();
    wrong.set_service_id("another-trading");
    EXPECT_THROW(trading::cold_backup(root, wrong), Error);
    const auto before = sha256_file(root / "journal.sqlite");
    replace_file_durably(
        root / "facts/objects" / (original.published.operation().reference().version() + ".pb"),
        "corrupt-isolated-original");
    EXPECT_THROW(trading::cold_backup(root, owner()), std::exception);
    EXPECT_EQ(sha256_file(root / "journal.sqlite"), before);
}

TEST(TradingColdBackup, JointResearchCutClosesOnlyTheExactPublishedSourceAndRestoresBothOwners)
{
    Folder folder;
    const auto source_root = folder.path / "trading";
    const auto source = seed(source_root);
    const auto research_root = folder.path / "research";
    fs::create_directory(research_root);
    auto research_owner = owner();
    research_owner.set_domain("research");
    research_owner.set_service_id("research-one");
    const auto program = sha256_file(current_executable());
    research::proto::AccountReviewRequest selected;
    *selected.mutable_source() = source.published.operation().reference();
    selected.set_account_id("fixture-account");
    selected.set_environment(protocol::proto::BROKER_SIMULATION);
    const auto fixed =
        protocol::make_account_review_input(research_owner, selected, source.published, program);
    std::string result_digest;
    {
        tasks::Store store(research_root, {"research-one"});
        (void)programs::Files(research_root / "programs", research_owner)
            .preserve(current_executable(), program);
        tasks::submit(store, "review-one", fixed);
        auto read = store.prepare_input("review-one");
        read.load_for_claim();
        const auto token = store.commit(store.claim(read)).token();
        auto attempt = store.confirm_attempt(std::move(read));
        attempt.set_token(token);
        store.commit(
            tasks::finish(
                store,
                tasks::execute(attempt, research_root, {}, [](auto, auto) {}, program)));
        result_digest = store.describe("review-one").result_digest();
    }
    {
        auto partial = terminal_backup::cold_cut({{research_owner, research_root}});
        const auto plan = partial.preview("research-without-trading");
        EXPECT_TRUE(std::ranges::any_of(plan.at("dependencies"), [](const auto& dependency) {
            return dependency.at("object").at("kind") == "trading_facts"
                && !dependency.at("included").template get<bool>();
        }));
    }
    Json plan;
    const auto destination = folder.path / "backup";
    {
        auto cut =
            terminal_backup::cold_cut({{research_owner, research_root}, {owner(), source_root}});
        plan = cut.preview("joint-trading-research");
        EXPECT_TRUE(std::ranges::all_of(plan.at("dependencies"), [](const auto& dependency) {
            return dependency.at("included").template get<bool>();
        }));
        bool source_included = false;
        for (const auto& dependency : plan.at("dependencies"))
            if (dependency.at("object").at("kind") == "trading_facts") {
                source_included = dependency.at("included");
                EXPECT_EQ(dependency.at("object").at("version"), selected.source().version());
                EXPECT_EQ(dependency.at("object").at("owner").at("service"), "trading-one");
            }
        EXPECT_TRUE(source_included);
        EXPECT_FALSE(plan.at("limitations").empty())
            << "Source coverage and old programme gaps remain honest";
        cut.save("joint-trading-research", destination, sha256_bytes(plan.dump()));
    }
    const auto restore = folder.path / "restore";
    backup_files::restore_files(destination, restore);
    fs::rename(source_root, folder.path / "preserved-trading");
    fs::rename(research_root, folder.path / "preserved-research");
    fs::path restored_research, restored_trading;
    unsigned index = 0;
    for (const auto& identity : plan.at("owners")) {
        const auto directory = restore / "owners" / std::to_string(index++);
        if (identity.at("domain") == "research")
            restored_research = directory;
        if (identity.at("domain") == "trading")
            restored_trading = directory;
    }
    ASSERT_FALSE(restored_research.empty());
    ASSERT_FALSE(restored_trading.empty());
    {
        auto verified = terminal_backup::cold_cut(
            {{research_owner, restored_research}, {owner(), restored_trading}});
        const auto restored_plan = verified.preview("joint-restored-check");
        for (const auto& dependency : restored_plan.at("dependencies"))
            EXPECT_TRUE(dependency.at("included"));
    }
    tasks::Store store(restored_research, {"research-one"});
    const auto result = tasks::verified_result(store, "review-one");
    EXPECT_EQ(result.result_task().result_digest(), result_digest);
    EXPECT_EQ(result.result_task().account_review().SerializeAsString(), fixed.SerializeAsString());
    EXPECT_EQ(
        trading::FactsArchive::read_published(restored_trading, selected.source())
            .SerializeAsString(),
        source.published.SerializeAsString());
    EXPECT_FALSE(fs::exists(folder.path / "account-owners"));
}
