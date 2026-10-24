#include "logic_store.hpp"
#include "research_logic.hpp"
#include "strategy_store.hpp"
#include "risk_module.hpp"
#include "data/bar_fixture.hpp"
#include <asterion/protocol/research_strategy.hpp>
#include <asterion/protocol/trading.hpp>
#include "research_execution.hpp"
#include "tasks/task_store_support.hpp"
#include "program_files.hpp"
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/protocol/task_execution.hpp>
#include "task_store.hpp"
#include "sqlite_database.hpp"
#include <asterion/protocol/research_logic.hpp>
#include <asterion/foundation/decimal.hpp>
#include <asterion/kernel/process/child.hpp>
#include <gtest/gtest.h>
using namespace asterion;
namespace fs = std::filesystem;

namespace {

struct LogicStore : testing::Test
{
    fs::path root = fs::temp_directory_path() / ("asterion-logic-store-" + unique_process_id());
    resource::proto::Owner owner;
    std::unique_ptr<tasks::Store> store;

    void SetUp() override
    {
        ASSERT_TRUE(fs::create_directory(root));
        root = fs::canonical(root);
        owner.set_node_id("fixture-node");
        owner.set_service_id("research-one");
        owner.set_domain("research");
        store = std::make_unique<tasks::Store>(root, tasks::Identity{"research-one"});
    }

    void TearDown() override
    {
        store.reset();
        std::error_code error;
        fs::remove_all(root, error);
    }

    research::proto::LogicMutation registration() const
    {
        research::proto::LogicMutation request;
        request.set_id("definition-operation");
        auto& value = *request.mutable_registration();
        value.set_id("definition-one");
        value.set_name("Synthetic two means");
        value.set_purpose("Pure logic, not market performance");
        value.set_fast(1);
        value.set_slow(3);
        value.set_input(research::proto::LogicDefinition::COMPLETED_CLOSE);
        value.set_output(research::proto::LogicDefinition::TARGET_SIGN);
        return request;
    }

    research::proto::LogicMutation example(const research::proto::LogicDefinition& definition) const
    {
        research::proto::LogicMutation request;
        request.set_id("example-operation");
        auto& value = *request.mutable_example();
        value.set_id("case-one");
        *value.mutable_definition() = protocol::logic_object(definition);
        value.set_name("Prior four close expectations");
        value.set_use_scope("Fictional complete-close expectations only");
        value.set_fictional(true);
        for (const auto* price : {"10", "10", "13", "7"}) {
            const auto index = value.observations_size();
            auto& row = *value.add_observations();
            row.set_closed_at_ns((index + 1) * 60000000000LL);
            row.set_available_at_ns(row.closed_at_ns());
            row.mutable_close()->set_units(Decimal::parse(price).raw());
            if (index < 2)
                row.set_warmup(true);
            else
                row.set_target_sign(index == 2 ? 1 : -1);
        }
        return request;
    }
};

} // namespace

TEST_F(LogicStore, ReadOnlyDiscoveryDoesNotCreateASecondStoreOrInitializeDomainTables)
{
    research::Logic view(*store, owner);
    EXPECT_TRUE(view.definitions("", 20).empty());
    EXPECT_TRUE(view.examples("", "", 20).empty());
    EXPECT_THROW(view.apply(registration()), Error);
    store.reset();
    sqlite::Database database(root / "tasks.sqlite", sqlite::Database::Access::read_only);
    sqlite::Database::Statement count(
        database,
        "SELECT count(*) FROM sqlite_master WHERE name LIKE 'research_logic_%'");
    ASSERT_TRUE(count.step());
    EXPECT_EQ(count.integer(0), 0);
}

TEST_F(LogicStore, PriorExpectationsAndOriginalReceiptsRemainExactAcrossRestartAndConflicts)
{
    research::proto::LogicDefinition definition;
    research::proto::LogicExample saved;
    research::proto::LogicMutation expected;
    {
        research::Logic write(*store, owner, sqlite::Database::Access::writer);
        definition = write.apply(registration()).definition();
        expected = example(definition);
        const auto reply = write.apply(expected);
        saved = reply.example();
        EXPECT_EQ(write.apply(expected).SerializeAsString(), reply.SerializeAsString());
        const auto fixed = write.input(protocol::logic_object(saved), true);
        EXPECT_TRUE(tasks::compute_logic_trial(fixed).all_match());
        EXPECT_TRUE(store->list().tasks().empty()); // Saving an example cannot start a trial.
        auto changed = expected;
        changed.mutable_example()->mutable_observations(2)->set_target_sign(-1);
        EXPECT_THROW(write.apply(changed), Error);
        changed = registration();
        changed.mutable_registration()->set_fast(2);
        EXPECT_THROW(write.apply(changed), Error);
        EXPECT_EQ(write.example(saved.id()).SerializeAsString(), saved.SerializeAsString());
    }
    store.reset();
    store = std::make_unique<tasks::Store>(root, tasks::Identity{"research-one"});
    research::Logic view(*store, owner);
    EXPECT_EQ(view.definition(definition.id()).SerializeAsString(), definition.SerializeAsString());
    EXPECT_EQ(view.example(saved.id()).SerializeAsString(), saved.SerializeAsString());
    EXPECT_EQ(view.receipt(expected)->example().SerializeAsString(), saved.SerializeAsString());
    ASSERT_EQ(view.examples(definition.id(), "", 1).size(), 1U);
    EXPECT_TRUE(view.examples("different-definition", "", 1).empty());
    EXPECT_TRUE(view.definitions(definition.id(), 1).empty());
}

TEST_F(LogicStore, DeprecationRejectsNewAdmissionButPreservesOriginalFixedInputAndOldReceipt)
{
    research::Logic write(*store, owner, sqlite::Database::Access::writer);
    const auto registration_reply = write.apply(registration());
    const auto saved = write.apply(example(registration_reply.definition())).example();
    const auto fixed = write.input(protocol::logic_object(saved), true);
    research::proto::LogicMutation retire;
    retire.set_id("deprecate");
    *retire.mutable_retire()->mutable_definition() = protocol::logic_object(fixed.definition());
    retire.mutable_retire()->set_reason("No new trials; original evidence remains");
    const auto deprecated = write.apply(retire).definition();
    EXPECT_EQ(deprecated.digest(), fixed.definition().digest());
    EXPECT_EQ(
        write.apply(registration()).SerializeAsString(),
        registration_reply.SerializeAsString());
    EXPECT_THROW(write.verify_input(fixed, true), Error);
    EXPECT_NO_THROW(write.verify_input(fixed, false));
    auto next = example(fixed.definition());
    next.set_id("new-example-operation");
    next.mutable_example()->set_id("new-example");
    EXPECT_THROW(write.apply(next), Error);
    EXPECT_EQ(write.example(saved.id()).SerializeAsString(), saved.SerializeAsString());
    EXPECT_TRUE(tasks::compute_logic_trial(fixed).all_match());
}

TEST_F(LogicStore, FailedCommitCannotLeaveAnExampleOrReceiptClaimingPriorExpectationsWereSaved)
{
    research::Logic write(*store, owner, sqlite::Database::Access::writer);
    const auto definition = write.apply(registration()).definition();
    const auto request = example(definition);
    sqlite::fail_next_commits_for_testing(1);
    EXPECT_THROW(write.apply(request), std::runtime_error);
    EXPECT_THROW(write.example(request.example().id()), Error);
    EXPECT_FALSE(write.receipt(request));
    EXPECT_EQ(write.apply(request).example().id(), request.example().id());
}

TEST_F(LogicStore, WrongOwnerAndPartialSchemaCannotBeSilentlyRepaired)
{
    {
        research::Logic write(*store, owner, sqlite::Database::Access::writer);
        (void)write.apply(registration());
        auto other = owner;
        other.set_node_id("other-node");
        EXPECT_THROW(research::Logic wrong(*store, other), Error);
    }
    store.reset();
    {
        sqlite::Database database(root / "tasks.sqlite", sqlite::Database::Access::writer);
        database.execute("DROP TABLE research_logic_examples");
    }
    store = std::make_unique<tasks::Store>(root, tasks::Identity{"research-one"});
    EXPECT_THROW(research::Logic read(*store, owner), std::invalid_argument);
    EXPECT_THROW(
        research::Logic write(*store, owner, sqlite::Database::Access::writer),
        std::invalid_argument);
    EXPECT_TRUE(store->list().tasks().empty());
}

TEST_F(LogicStore, ColdMetadataRequiresTheExactOriginalExampleAndDefinitionWithoutStartingTasks)
{
    research::proto::LogicTrialInput fixed;
    {
        research::Logic write(*store, owner, sqlite::Database::Access::writer);
        const auto definition = write.apply(registration()).definition();
        const auto saved = write.apply(example(definition)).example();
        fixed = write.input(protocol::logic_object(saved), true);
    }
    store.reset();
    std::vector<research::proto::LogicTrialInput> required{fixed};
    unsigned definitions = 0, examples = 0;
    research::Logic::inspect_backup(
        root,
        owner,
        required,
        [&](const auto&) {
            ++definitions;
        },
        [&](const auto&) {
            ++examples;
        });
    EXPECT_EQ(definitions, 1U);
    EXPECT_EQ(examples, 1U);
    auto changed = fixed;
    changed.mutable_definition()->set_purpose("Different prior purpose");
    required = {changed};
    EXPECT_THROW(
        research::Logic::inspect_backup(
            root,
            owner,
            required,
            [](const auto&) {},
            [](const auto&) {}),
        Error);
    required = {fixed};
    {
        sqlite::Database database(root / "tasks.sqlite", sqlite::Database::Access::writer);
        sqlite::Database::Statement erase(
            database,
            "DELETE FROM research_logic_examples WHERE id=?");
        erase.bind(1, fixed.example().id()).step();
    }
    EXPECT_THROW(
        research::Logic::inspect_backup(
            root,
            owner,
            required,
            [](const auto&) {},
            [](const auto&) {}),
        Error);
}

TEST_F(LogicStore, ActualTaskPublishesPriorComparisonAndReopensWithoutDataOrChangingExpectations)
{
    research::proto::LogicTrialInput fixed;
    {
        research::Logic write(*store, owner, sqlite::Database::Access::writer);
        const auto definition = write.apply(registration()).definition();
        auto request = example(definition);
        request.mutable_example()->mutable_observations(2)->set_target_sign(-1);
        fixed = write.input(protocol::logic_object(write.apply(request).example()), true);
    }
    const auto artifact = sha256_file(current_executable());
    (void)programs::Files(root / "programs", owner).preserve(current_executable(), artifact);
    auto missing_program = store->submission("missing-program", fixed);
    EXPECT_THROW(tasks::submit(*store, std::move(missing_program)), Error);
    auto submission = store->submission("prior-comparison", fixed);
    submission.set_program_artifact(artifact);
    EXPECT_EQ(tasks::submit(*store, std::move(submission)).kind(), task::proto::LOGIC_TRIAL);
    auto read = store->prepare_input("prior-comparison");
    read.load_for_claim();
    EXPECT_FALSE(read.execution().has_data_owner());
    const auto refs = protocol::task_data_references(read.execution());
    EXPECT_TRUE(refs.minutes.empty());
    EXPECT_TRUE(refs.snapshots.empty());
    const auto token = store->commit(store->claim(read)).token();
    auto attempt = store->confirm_attempt(std::move(read));
    EXPECT_EQ(attempt.task().logic_trial().SerializeAsString(), fixed.SerializeAsString());
    EXPECT_THROW(tasks::execute(attempt, root, {}, [](auto, auto) {}, "wrong-program"), Error);
    auto finish = tasks::execute(attempt, root, {}, [](auto, auto) {}, artifact);
    finish.set_token(token);
    ASSERT_TRUE(finish.has_logic_trial());
    EXPECT_FALSE(finish.logic_trial().all_match());
    EXPECT_EQ(finish.logic_trial().matched(), 3U);
    store->commit(tasks::finish(*store, std::move(finish)));
    const auto original = tasks::verified_result(*store, "prior-comparison");
    EXPECT_EQ(original.result_task().state(), task::proto::SUCCEEDED);
    EXPECT_FALSE(original.logic_trial().all_match());
    EXPECT_EQ(store->history_usage(std::string(64, 'a')).references_size(), 0);
    EXPECT_EQ(original.result_task().program_artifact(), artifact);
    store.reset();
    store = std::make_unique<tasks::Store>(root, tasks::Identity{"research-one"});
    EXPECT_EQ(
        tasks::verified_result(*store, "prior-comparison").SerializeAsString(),
        original.SerializeAsString());
    research::Logic view(*store, owner);
    EXPECT_EQ(
        view.example(fixed.example().id()).SerializeAsString(),
        fixed.example().SerializeAsString());
}

TEST_F(LogicStore, MatchingPriorTrialCanAssembleANonRunningPackageAndDraftWithoutAReferenceBacktest)
{
    research::proto::LogicTrialInput fixed;
    {
        research::Logic write(*store, owner, sqlite::Database::Access::writer);
        const auto definition = write.apply(registration()).definition();
        fixed =
            write.input(protocol::logic_object(write.apply(example(definition)).example()), true);
    }
    const auto artifact = sha256_file(current_executable());
    (void)programs::Files(root / "programs", owner).preserve(current_executable(), artifact);
    auto submission = store->submission("matching-trial", fixed);
    submission.set_program_artifact(artifact);
    tasks::submit(*store, std::move(submission));
    const auto token = store->commit(store->claim("matching-trial")).token();
    task::proto::TaskFinish finish;
    finish.set_id("matching-trial");
    finish.set_token(token);
    *finish.mutable_logic_trial() = tasks::compute_logic_trial(fixed);
    store->commit(tasks::finish(*store, std::move(finish)));
    const auto original = tasks::verified_result(*store, "matching-trial");
    research::proto::StrategyMutation assemble;
    assemble.set_id("assemble-logic");
    auto& request = *assemble.mutable_logic();
    request.set_id("logic-package");
    request.set_name("Synthetic verified rule");
    request.set_purpose("Independent research only");
    request.set_limitations("Four fictional observations are not market validation");
    auto& source = *request.mutable_source();
    *source.mutable_owner() = owner;
    source.set_kind("logic_trial_result");
    source.set_id("matching-trial");
    source.set_version(original.result_task().result_digest());
    *request.mutable_contract() =
        test::dataset({test::flat("2026-09-25", 1790298000000000000LL, "100")}).contract();
    request.set_interval_minutes(1);
    *request.mutable_strategy() = protocol::encode_strategy(
        {{"quantity", "1"},
            {"sides", "both"},
            {"rule", {{"kind", "moving_average"}, {"fast", 1}, {"slow", 3}}}});
    const auto digest = sha256_file(root / "matching-trial/input.pb");
    const auto risk = risk_providers::Module::selected().artifact();
    const auto captured =
        research::capture_logic_strategy_package(owner, request, original, digest, risk);
    ASSERT_TRUE(captured.has_logic());
    EXPECT_FALSE(captured.has_reference_result());
    EXPECT_EQ(
        captured.logic().example().SerializeAsString(),
        protocol::logic_object(fixed.example()).SerializeAsString());
    research::Strategies strategies(*store, owner, sqlite::Database::Access::writer);
    const auto package = strategies.apply(assemble, &captured).package();
    EXPECT_EQ(
        strategies.apply(assemble).package().SerializeAsString(),
        package.SerializeAsString());
    EXPECT_EQ(protocol::decode_strategy_package(package).at("evidence_relation"), "logic_trial");
    EXPECT_TRUE(protocol::decode_strategy_package(package).at("reference_result").is_null());
    EXPECT_EQ(
        strategies.source_consumers("matching-trial"),
        std::vector<std::string>{"strategy_package/logic-package"});
    research::proto::StrategyMutation deliver;
    deliver.set_id("save-logic-delivery");
    deliver.mutable_deliver()->set_id("logic-draft");
    deliver.mutable_deliver()->set_target_task_id("future-backtest");
    *deliver.mutable_deliver()->mutable_package() = protocol::strategy_package_object(package);
    const auto draft = strategies.apply(deliver).delivery();
    EXPECT_TRUE(draft.has_logic_trial());
    EXPECT_FALSE(draft.has_reference_result());
    EXPECT_EQ(draft.logic_trial().SerializeAsString(), source.SerializeAsString());
    EXPECT_FALSE(strategies.delivery_status(draft.id()).target_accepted());
    EXPECT_EQ(store->list().tasks_size(), 1);
    auto changed = request;
    changed.mutable_strategy()->mutable_moving_average()->set_fast(2);
    EXPECT_THROW(
        research::capture_logic_strategy_package(owner, changed, original, digest, risk),
        std::invalid_argument);
    auto wrong_owner = owner;
    wrong_owner.set_service_id("other-research");
    EXPECT_THROW(
        research::capture_logic_strategy_package(wrong_owner, request, original, digest, risk),
        Error);
    auto unverified = original;
    unverified.mutable_logic_trial()->set_all_match(false);
    EXPECT_THROW(
        research::capture_logic_strategy_package(owner, request, unverified, digest, risk),
        std::invalid_argument);
    auto all_warmup = original;
    auto* input = all_warmup.mutable_result_task()->mutable_logic_trial();
    input->mutable_example()->mutable_observations()->RemoveLast();
    input->mutable_example()->mutable_observations()->RemoveLast();
    input->mutable_example()->set_digest(protocol::logic_example_digest(input->example()));
    *all_warmup.mutable_logic_trial() = tasks::compute_logic_trial(*input);
    EXPECT_TRUE(all_warmup.logic_trial().all_match());
    EXPECT_EQ(all_warmup.logic_trial().decisions(), 0U);
    EXPECT_THROW(
        research::capture_logic_strategy_package(
            owner,
            request,
            all_warmup,
            sha256_bytes(input->SerializeAsString()),
            risk),
        Error);
}
