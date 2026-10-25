#include "research_batch.hpp"
#include "research_submission.hpp"
#include "data_submission.hpp"
#include "support/data_discovery.hpp"
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <asterion/research.pb.h>
#include "batch_store.hpp"
#include "program_files.hpp"
#include "tasks/task_store_support.hpp"
#include <asterion/kernel/process/child.hpp>
#include <asterion/foundation/error.hpp>
#include "engine.hpp"
#include "tasks/paper_input.hpp"
#include <asterion/protocol/research_batch.hpp>
#include <asterion/protocol/research_assessment.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <set>
#include <array>
using namespace asterion;

namespace {

struct Source
{
    resource::proto::Owner owner;
    research::proto::BacktestBatchDraft draft;
    task::proto::TaskResponse verified;
    std::string input_digest;

    Source(std::array<const char*, 6> prices = {"100", "100", "100", "100", "100", "100"})
    {
        owner.set_domain("research");
        owner.set_node_id("fixture-node");
        owner.set_service_id("research-one");
        std::vector<MarketBar> bars;
        for (int i = 0; i < 6; ++i) {
            const auto day = "2026-09-" + std::to_string(21 + i);
            bars.push_back(
                test::flat(day, 1790000000000000000LL + i * 86400000000000LL, prices[i]));
        }
        auto dataset = test::dataset(bars);
        dataset.set_interval_minutes(0);
        dataset.clear_source_dataset_ids();
        dataset.clear_history_evidence();
        dataset.set_source(dataset.daily().daily_source());
        dataset.set_revision(protocol::bar_dataset_revision(dataset));
        auto input = testing_support::backtest_input(dataset);
        input.mutable_data_owner()->set_node_id("fixture-node");
        input.mutable_data_owner()->set_service_id("historical-data");
        input.mutable_strategies(0)->clear_rule();
        input.mutable_strategies(0)->mutable_momentum()->set_lookback(2);
        auto& task = *verified.mutable_result_task();
        task.set_id("source");
        task.set_kind(task::proto::BACKTEST);
        task.set_state(task::proto::SUCCEEDED);
        task.set_program_artifact(std::string(64, 'a'));
        task.set_risk_artifact(std::string(64, 'b'));
        *task.mutable_input() = input;
        *verified.mutable_backtest() = backtest::run(input);
        task.set_result_digest(sha256_bytes(verified.backtest().SerializeAsString()));
        input_digest = sha256_bytes(input.SerializeAsString());
        draft.set_id("finite-costs");
        draft.set_name("Explicit cost scenarios");
        draft.set_purpose("Post hoc sensitivity");
        draft.set_limitations("Synthetic source already observed; not independent validation");
        *draft.mutable_source()->mutable_owner() = owner;
        draft.mutable_source()->set_kind("backtest_result");
        draft.mutable_source()->set_id(task.id());
        draft.mutable_source()->set_version(task.result_digest());
        draft.set_run_budget(2);
        draft.set_time_budget_seconds(30);
        for (unsigned ticks : {1U, 2U}) {
            auto& m = *draft.add_members();
            m.set_task_id("scenario-" + std::to_string(ticks));
            m.set_slippage_ticks(ticks);
        }
    }

    research::proto::BacktestBatchDefinition capture() const
    {
        return research::capture_backtest_batch(
            owner,
            draft,
            verified,
            input_digest,
            std::string(64, 'a'),
            std::string(64, 'b'),
            1);
    }
};

} // namespace

TEST(FeeSensitivity, ExactMultiplierChangesSixFeesAndPreservesEveryOtherOriginalByte)
{
    Source source;
    auto original = source.verified.result_task().input();
    auto* fees = original.mutable_paper()
                     ->mutable_contracts(0)
                     ->mutable_cost_schedule()
                     ->mutable_versions(0)
                     ->mutable_values();
    fees->mutable_open_fee_rate()->set_units(Decimal::parse("0.001").raw());
    fees->mutable_close_today_fee_rate()->set_units(Decimal::parse("0.002").raw());
    fees->mutable_close_yesterday_fee_rate()->set_units(Decimal::parse("0.003").raw());
    const auto bytes = original.SerializeAsString();
    auto changed = protocol::backtest_fee_variant(original, Decimal::parse("1.5"));
    const auto& actual = changed.paper().contracts(0).cost_schedule().versions(0).values();
    EXPECT_EQ(actual.open_fee().units(), Decimal::parse("3").raw());
    EXPECT_EQ(actual.close_today_fee().units(), Decimal::parse("4.5").raw());
    EXPECT_EQ(actual.close_yesterday_fee().units(), Decimal::parse("6").raw());
    EXPECT_EQ(actual.open_fee_rate().units(), Decimal::parse("0.0015").raw());
    EXPECT_EQ(actual.close_today_fee_rate().units(), Decimal::parse("0.003").raw());
    EXPECT_EQ(actual.close_yesterday_fee_rate().units(), Decimal::parse("0.0045").raw());
    *changed.mutable_paper()->mutable_contracts(0)->mutable_cost_schedule() =
        original.paper().contracts(0).cost_schedule();
    EXPECT_EQ(changed.SerializeAsString(), bytes);
    EXPECT_EQ(original.SerializeAsString(), bytes);
}

TEST(FeeSensitivity, ZeroFeesRemainAnExplicitScenarioAndUnchangedOrInvalidMultipliersAreRejected)
{
    Source source;
    const auto& original = source.verified.result_task().input();
    const auto zero = protocol::backtest_fee_variant(original, Decimal{});
    EXPECT_EQ(zero.paper().contracts(0).cost_schedule().versions(0).values().open_fee().units(), 0);
    EXPECT_THROW(
        protocol::backtest_fee_variant(original, Decimal::parse("1")),
        std::invalid_argument);
    EXPECT_THROW(protocol::backtest_fee_variant(zero, Decimal::parse("2")), std::invalid_argument);
    EXPECT_THROW(
        protocol::backtest_fee_variant(original, Decimal::parse("-0.1")),
        std::invalid_argument);
    EXPECT_THROW(
        protocol::backtest_fee_variant(original, Decimal::parse("100.00000001")),
        std::invalid_argument);
    auto precision = original;
    precision.mutable_paper()
        ->mutable_contracts(0)
        ->mutable_cost_schedule()
        ->mutable_versions(0)
        ->mutable_values()
        ->mutable_open_fee()
        ->set_units(1);
    const auto before = precision.SerializeAsString();
    EXPECT_THROW(protocol::backtest_fee_variant(precision, Decimal::parse("1.5")), std::exception);
    EXPECT_EQ(precision.SerializeAsString(), before);
}

TEST(FeeSensitivity, ActualAccountReexecutionReconcilesFourToSixFeesAndTwentySixToTwentyFourNet)
{
    Source source({"100", "101", "102", "100", "103", "103"});
    auto original = source.verified.result_task().input();
    auto* costs = original.mutable_paper()
                      ->mutable_contracts(0)
                      ->mutable_cost_schedule()
                      ->mutable_versions(0)
                      ->mutable_values();
    costs->mutable_open_fee()->set_units(Decimal::parse("1").raw());
    costs->mutable_close_today_fee()->set_units(Decimal::parse("2").raw());
    costs->mutable_close_yesterday_fee()->set_units(Decimal::parse("2").raw());
    const auto before = backtest::run(original);
    const auto fixed = protocol::backtest_fee_variant(original, Decimal::parse("1.5"));
    const auto after = backtest::run(fixed);
    ASSERT_EQ(before.account().fills_size(), 3);
    ASSERT_EQ(after.account().fills_size(), before.account().fills_size());
    for (int i = 0; i < before.account().fills_size(); ++i) {
        EXPECT_EQ(
            before.account().fills(i).quantity().SerializeAsString(),
            after.account().fills(i).quantity().SerializeAsString());
        EXPECT_EQ(
            before.account().fills(i).price().SerializeAsString(),
            after.account().fills(i).price().SerializeAsString());
    }
    EXPECT_EQ(before.account().fees().units(), Decimal::parse("4").raw());
    EXPECT_EQ(after.account().fees().units(), Decimal::parse("6").raw());
    EXPECT_EQ(
        before.account().equity().units() - original.paper().deposit().units(),
        Decimal::parse("26").raw());
    EXPECT_EQ(
        after.account().equity().units() - fixed.paper().deposit().units(),
        Decimal::parse("24").raw());
}

TEST(FeeSensitivity, HigherOpeningFeesChangeAdmissionRatherThanSubtractingFromAnOldCurve)
{
    Source source({"100", "101", "102", "100", "103", "103"});
    auto original = source.verified.result_task().input();
    original.mutable_paper()->mutable_deposit()->set_units(Decimal::parse("101").raw());
    auto* costs = original.mutable_paper()
                      ->mutable_contracts(0)
                      ->mutable_cost_schedule()
                      ->mutable_versions(0)
                      ->mutable_values();
    costs->mutable_open_fee()->set_units(Decimal::parse("1").raw());
    costs->mutable_close_today_fee()->set_units(Decimal::parse("2").raw());
    costs->mutable_close_yesterday_fee()->set_units(Decimal::parse("2").raw());
    const auto fixed = protocol::backtest_fee_variant(original, Decimal::parse("1.5"));
    const auto before = backtest::run(original);
    const auto after = backtest::run(fixed);
    ASSERT_EQ(before.account().fills_size(), 3);
    EXPECT_EQ(after.account().fills_size(), 0);
    ASSERT_GT(after.target_rejections_size(), 0);
    for (const auto& rejection : after.target_rejections()) {
        EXPECT_EQ(rejection.reason(), backtest::proto::BacktestTargetRejection::INSUFFICIENT_FUNDS);
        EXPECT_FALSE(rejection.intent_id().empty());
    }
    protocol::validate_backtest_result(fixed, after);
    const auto shown = protocol::decode_backtest_result(after);
    EXPECT_EQ(shown.at("target_rejections").at(0).at("reason"), "insufficient_funds");
    EXPECT_EQ(after.account().fees().units(), 0);
    EXPECT_EQ(after.account().equity().units(), original.paper().deposit().units());
    EXPECT_NE(before.account().equity().units(), after.account().equity().units());
}

TEST(ResearchBatches, FeeMembersPinDistinctMultipliersAndCannotHideSlippageChanges)
{
    Source source;
    for (int i = 0; i < 2; ++i) {
        auto* member = source.draft.mutable_members(i);
        member->set_slippage_ticks(0);
        member->mutable_fee_multiplier()->set_units(Decimal::parse(i ? "2" : "1.5").raw());
    }
    const auto fixed = source.capture();
    const auto reuse = protocol::backtest_batch_member_reuse(fixed, "scenario-1");
    ASSERT_TRUE(reuse.has_fee_multiplier());
    EXPECT_FALSE(reuse.has_slippage_ticks());
    EXPECT_EQ(reuse.fee_multiplier().units(), Decimal::parse("1.5").raw());
    const auto input = protocol::backtest_batch_member_input(
        source.verified.result_task().input(),
        source.draft.members(0));
    EXPECT_EQ(input.paper().contracts(0).slippage_ticks(), 0U);
    EXPECT_EQ(
        input.paper().contracts(0).cost_schedule().versions(0).values().open_fee().units(),
        Decimal::parse("3").raw());
    source.draft.mutable_members(1)->mutable_fee_multiplier()->set_units(
        Decimal::parse("1.5").raw());
    EXPECT_THROW(source.capture(), std::invalid_argument);
    source.draft.mutable_members(1)->mutable_fee_multiplier()->set_units(Decimal::parse("2").raw());
    source.draft.mutable_members(0)->set_slippage_ticks(1);
    EXPECT_THROW(source.capture(), std::invalid_argument);
    source.draft.mutable_members(0)->set_slippage_ticks(0);
    source.draft.mutable_members(0)->mutable_fee_multiplier()->set_units(Decimal::parse("1").raw());
    EXPECT_THROW(source.capture(), std::invalid_argument);
}

TEST(ResearchBatches, FiniteManifestPinsOriginalDataConditionsAndEveryActualMember)
{
    Source source;
    const auto original = source.verified.SerializeAsString();
    const auto definition = source.capture();
    EXPECT_EQ(definition.request().SerializeAsString(), source.draft.SerializeAsString());
    EXPECT_EQ(definition.work_units_per_run(), 6U);
    EXPECT_EQ(definition.data_owner().service_id(), "historical-data");
    const auto reuse = protocol::backtest_batch_member_reuse(definition, "scenario-2");
    EXPECT_EQ(reuse.choice(), backtest::proto::BacktestReuse::FIXED);
    EXPECT_EQ(reuse.source_result_digest(), source.draft.source().version());
    EXPECT_EQ(reuse.source_input_sha256(), source.input_digest);
    EXPECT_EQ(reuse.slippage_ticks(), 2U);
    EXPECT_EQ(source.verified.SerializeAsString(), original);
    EXPECT_THROW(
        protocol::backtest_batch_member_reuse(definition, "unplanned"),
        std::invalid_argument);
}

TEST(ResearchBatches, BudgetDuplicatesAndAlreadySeenBaselineCannotSilentlyChangeTheManifest)
{
    Source source;
    source.draft.set_time_budget_seconds(0);
    EXPECT_THROW(source.capture(), std::invalid_argument);
    source.draft.set_time_budget_seconds(30);
    source.draft.set_run_budget(1);
    EXPECT_THROW(source.capture(), std::invalid_argument);
    source.draft.set_run_budget(2);
    source.draft.mutable_members(1)->set_task_id("scenario-1");
    EXPECT_THROW(source.capture(), std::invalid_argument);
    source.draft.mutable_members(1)->set_task_id("scenario-2");
    source.draft.mutable_members(1)->set_slippage_ticks(1);
    EXPECT_THROW(source.capture(), std::invalid_argument);
    source.draft.mutable_members(1)->set_slippage_ticks(0);
    EXPECT_THROW(source.capture(), std::invalid_argument);
}

TEST(ResearchBatches, DifferentOwnerSourceOrImplementationCannotBeSubstituted)
{
    Source source;
    source.owner.set_node_id("other-node");
    EXPECT_THROW(source.capture(), std::invalid_argument);
    source.owner.set_node_id("fixture-node");
    source.input_digest = std::string(64, 'c');
    EXPECT_THROW(source.capture(), std::invalid_argument);
    source.input_digest = sha256_bytes(source.verified.result_task().input().SerializeAsString());
    source.verified.mutable_result_task()->set_program_artifact(std::string(64, 'c'));
    EXPECT_THROW(source.capture(), std::invalid_argument);
    source.verified.mutable_result_task()->set_program_artifact(std::string(64, 'a'));
    source.verified.mutable_result_task()->set_state(task::proto::INTERRUPTED);
    EXPECT_THROW(source.capture(), std::invalid_argument);
}

namespace {

struct BatchFixture : testing::Test
{
    std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("asterion-batch-test-" + unique_process_id());
    Source source;
    std::filesystem::path runtime;
    std::unique_ptr<tasks::Store> store;
    research::proto::BacktestBatchDefinition definition;

    void SetUp() override
    {
        std::filesystem::create_directory(root);
        store = std::make_unique<tasks::Store>(root, tasks::Identity{"research-one"});
        const auto artifact = sha256_file(current_executable());
        (void)programs::Files(root / "programs", source.owner)
            .preserve(current_executable(), artifact);
        auto submission = task_submission(*store, "source", source.verified.result_task().input());
        submission.set_program_artifact(artifact);
        tasks::submit(*store, std::move(submission));
        const auto token = store->commit(store->claim("source")).token();
        store->commit(tasks::finish(*store, "source", token, source.verified.backtest()));
        source.verified = tasks::verified_result(*store, "source");
        source.draft.mutable_source()->set_version(source.verified.result_task().result_digest());
        source.input_digest = sha256_file(root / "source/input.pb");
        definition = research::capture_backtest_batch(
            source.owner,
            source.draft,
            source.verified,
            source.input_digest,
            source.verified.result_task().program_artifact(),
            source.verified.result_task().risk_artifact(),
            1);
    }

    void TearDown() override
    {
        sqlite::fail_next_commits_for_testing(0);
        store.reset();
        std::filesystem::remove_all(root);
        if (!runtime.empty())
            std::filesystem::remove_all(runtime);
    }

    research::proto::BacktestBatchMutation save() const
    {
        research::proto::BacktestBatchMutation command;
        command.set_id("save-command");
        *command.mutable_save() = source.draft;
        return command;
    }

    research::proto::BacktestBatchMutation start() const
    {
        research::proto::BacktestBatchMutation command;
        command.set_id("start-command");
        *command.mutable_start() = protocol::backtest_batch_object(definition);
        return command;
    }

    research::proto::BacktestBatchMutation stop() const
    {
        research::proto::BacktestBatchMutation command;
        command.set_id("stop-command");
        *command.mutable_stop()->mutable_batch() = protocol::backtest_batch_object(definition);
        command.mutable_stop()->set_reason("Explicitly stop new admissions");
        return command;
    }

    backtest::proto::BacktestInput input(unsigned ticks) const
    {
        return protocol::backtest_slippage_variant(source.verified.result_task().input(), ticks);
    }
};

} // namespace

TEST_F(BatchFixture, SavedReceiptsKeepOriginalIntentAndRestartDoesNotCreateUnstartedTasks)
{
    std::string receipt;
    {
        research::Batches batches(*store, source.owner, sqlite::Database::Access::writer);
        const auto saved = batches.apply(save(), &definition);
        receipt = saved.SerializeAsString();
        EXPECT_EQ(saved.phase(), research::proto::BATCH_SAVED);
        EXPECT_EQ(batches.source_consumers("source"), std::vector<std::string>{source.draft.id()});
        EXPECT_FALSE(store->find_task("scenario-1"));
        EXPECT_EQ(batches.apply(save()).SerializeAsString(), receipt);
        auto changed = save();
        changed.mutable_save()->set_time_budget_seconds(31);
        EXPECT_THROW(batches.apply(changed, &definition), Error);
        EXPECT_EQ(batches.apply(start()).phase(), research::proto::BATCH_RUNNING);
        EXPECT_FALSE(store->find_task("scenario-1"));
    }
    store.reset();
    store = std::make_unique<tasks::Store>(root, tasks::Identity{"research-one"});
    research::Batches recovered(*store, source.owner, sqlite::Database::Access::writer);
    const auto interrupted = recovered.interrupt_running();
    ASSERT_EQ(interrupted.size(), 1U);
    EXPECT_EQ(interrupted[0].phase(), research::proto::BATCH_INTERRUPTED);
    EXPECT_FALSE(interrupted[0].has_elapsed_ms());
    EXPECT_FALSE(store->find_task("scenario-1"));
    EXPECT_EQ(recovered.apply(save()).SerializeAsString(), receipt);
    auto retry = start();
    retry.set_id("another-start");
    EXPECT_THROW(recovered.apply(retry), Error);
}

TEST_F(BatchFixture, DurableStopFencesPendingAdmissionsAndKeepsUnknownSeparateFromRejected)
{
    research::Batches batches(*store, source.owner, sqlite::Database::Access::writer);
    batches.apply(save(), &definition);
    batches.apply(start());
    const auto digest = sha256_bytes(input(1).SerializeAsString());
    batches.begin_member(source.draft.id(), "scenario-1", digest);
    batches.verify_admission(protocol::backtest_batch_object(definition), "scenario-1", digest);
    EXPECT_EQ(batches.apply(stop()).phase(), research::proto::BATCH_STOPPING);
    EXPECT_THROW(
        batches.verify_admission(protocol::backtest_batch_object(definition), "scenario-1", digest),
        Error);
    EXPECT_THROW(
        batches.begin_member(
            source.draft.id(),
            "scenario-2",
            sha256_bytes(input(2).SerializeAsString())),
        Error);
    EXPECT_EQ(batches.finish(source.draft.id(), 10).phase(), research::proto::BATCH_STOPPING);
    EXPECT_EQ(
        batches.state(source.draft.id()).members(0).admission(),
        research::proto::BATCH_MEMBER_PENDING);
    protocol::proto::Error rejected;
    rejected.set_code("conflict");
    rejected.set_message("Known refusal before task registration");
    batches.reject_member(source.draft.id(), "scenario-1", rejected);
    const auto stopped = batches.finish(source.draft.id(), 10);
    EXPECT_EQ(stopped.phase(), research::proto::BATCH_STOPPED);
    EXPECT_EQ(stopped.members(1).admission(), research::proto::BATCH_MEMBER_WAITING);
    EXPECT_FALSE(store->find_task("scenario-1"));
}

TEST_F(BatchFixture, OnlyMatchingActualTasksConfirmMembersAndCompleteResultsRemainOwnedByTaskStore)
{
    research::Batches batches(*store, source.owner, sqlite::Database::Access::writer);
    batches.apply(save(), &definition);
    batches.apply(start());
    const auto fixed = input(1);
    const auto digest = sha256_bytes(fixed.SerializeAsString());
    batches.begin_member(source.draft.id(), "scenario-1", digest);
    EXPECT_THROW(batches.confirm_member(source.draft.id(), "scenario-1"), Error);
    auto submission = task_submission(*store, "scenario-1", fixed);
    submission.set_program_artifact(definition.program_artifact());
    task::proto::TaskDerivation derivation;
    derivation.set_source_id("source");
    derivation.set_source_result_digest(source.draft.source().version());
    derivation.set_data_choice("fixed");
    *derivation.mutable_batch() = protocol::backtest_batch_object(definition);
    submission.set_derivation(derivation);
    batches.verify_admission(derivation.batch(), "scenario-1", digest);
    tasks::submit(*store, std::move(submission));
    EXPECT_EQ(
        batches.confirm_member(source.draft.id(), "scenario-1").members(0).admission(),
        research::proto::BATCH_MEMBER_ADMITTED);
    EXPECT_EQ(batches.finish(source.draft.id(), 10).phase(), research::proto::BATCH_RUNNING);
    const auto token = store->commit(store->claim("scenario-1")).token();
    store->commit(tasks::finish(*store, "scenario-1", token, backtest::run(fixed)));
    const auto actual = tasks::verified_result(*store, "scenario-1");
    EXPECT_EQ(
        actual.result_task().metadata().derivation().batch().SerializeAsString(),
        derivation.batch().SerializeAsString());
    EXPECT_EQ(actual.backtest().SerializeAsString(), backtest::run(fixed).SerializeAsString());
    batches.begin_member(
        source.draft.id(),
        "scenario-2",
        sha256_bytes(input(2).SerializeAsString()));
    protocol::proto::Error error;
    error.set_code("unavailable");
    error.set_message("Known independent fixture refusal");
    batches.reject_member(source.draft.id(), "scenario-2", error);
    const auto completed = batches.finish(source.draft.id(), 20);
    EXPECT_EQ(completed.phase(), research::proto::BATCH_COMPLETED);
    EXPECT_EQ(completed.members(1).admission(), research::proto::BATCH_MEMBER_REJECTED);
    EXPECT_FALSE(store->find_task("scenario-2"));
    EXPECT_EQ(
        tasks::verified_result(*store, "scenario-1").SerializeAsString(),
        actual.SerializeAsString());
}

TEST_F(BatchFixture, FailedMetadataCommitDoesNotReserveTargetsOrInventAcknowledgement)
{
    research::Batches batches(*store, source.owner, sqlite::Database::Access::writer);
    sqlite::fail_next_commits_for_testing(1);
    EXPECT_THROW(batches.apply(save(), &definition), std::exception);
    EXPECT_TRUE(batches.states("", 10).empty());
    EXPECT_FALSE(batches.receipt(save()));
    EXPECT_EQ(batches.apply(save(), &definition).phase(), research::proto::BATCH_SAVED);
}

TEST_F(
    BatchFixture,
    StopAfterCompletePreparationPersistsAbandonmentInsteadOfAdmittingOrPoisoningTheStore)
{
    research::Batches batches(*store, source.owner, sqlite::Database::Access::writer);
    batches.apply(save(), &definition);
    batches.apply(start());
    const auto fixed = input(1);
    const auto digest = sha256_bytes(fixed.SerializeAsString());
    batches.begin_member(source.draft.id(), "scenario-1", digest);
    auto submission = task_submission(*store, "scenario-1", fixed);
    submission.set_program_artifact(definition.program_artifact());
    task::proto::TaskDerivation origin;
    origin.set_source_id("source");
    origin.set_source_result_digest(source.draft.source().version());
    origin.set_data_choice("fixed");
    *origin.mutable_batch() = protocol::backtest_batch_object(definition);
    submission.set_derivation(origin);
    batches.verify_admission(origin.batch(), "scenario-1", digest);
    store->admit_submission(submission);
    submission.prepare_files();
    const auto prepared = store->prepared_submission_record("scenario-1");
    batches.apply(stop());
    EXPECT_THROW(batches.verify_admission(origin.batch(), "scenario-1", digest), Error);
    resource::proto::Object consumer;
    *consumer.mutable_owner() = source.owner;
    consumer.set_id("scenario-1");
    consumer.set_kind("backtest");
    consumer.set_version(digest);
    store->release_prepared_reservation(submission);
    store->commit(store->abandon_preparation(prepared, consumer));
    EXPECT_FALSE(store->find_task("scenario-1"));
    EXPECT_EQ(
        store->abandoned_preparation("scenario-1")->SerializeAsString(),
        consumer.SerializeAsString());
    EXPECT_TRUE(std::filesystem::exists(root / "scenario-1/input.pb"));
    protocol::proto::Error refused;
    refused.set_code("cancelled");
    refused.set_message("Batch stopped before task registration");
    batches.reject_member(source.draft.id(), "scenario-1", refused);
    EXPECT_EQ(batches.finish(source.draft.id(), 10).phase(), research::proto::BATCH_STOPPED);
    auto unrelated =
        task_submission(*store, "independent-after-stop", source.verified.result_task().input());
    unrelated.set_program_artifact(definition.program_artifact());
    EXPECT_NO_THROW(tasks::submit(*store, std::move(unrelated)));
    EXPECT_EQ(store->describe("independent-after-stop").state(), task::proto::QUEUED);
}

TEST_F(BatchFixture, ConfirmedTaskCancellationIsNotConfusedWithUnadmittedPreparation)
{
    research::Batches batches(*store, source.owner, sqlite::Database::Access::writer);
    batches.apply(save(), &definition);
    batches.apply(start());
    const auto fixed = input(1);
    const auto digest = sha256_bytes(fixed.SerializeAsString());
    batches.begin_member(source.draft.id(), "scenario-1", digest);
    auto submission = task_submission(*store, "scenario-1", fixed);
    submission.set_program_artifact(definition.program_artifact());
    task::proto::TaskDerivation origin;
    origin.set_source_id("source");
    origin.set_source_result_digest(source.draft.source().version());
    origin.set_data_choice("fixed");
    *origin.mutable_batch() = protocol::backtest_batch_object(definition);
    submission.set_derivation(origin);
    tasks::submit(*store, std::move(submission));
    batches.apply(stop());
    EXPECT_EQ(
        batches.confirm_member(source.draft.id(), "scenario-1").members(0).admission(),
        research::proto::BATCH_MEMBER_ADMITTED);
    store->commit(store->cancel("scenario-1"));
    EXPECT_EQ(store->describe("scenario-1").state(), task::proto::CANCELLED);
    EXPECT_TRUE(store->describe("scenario-1").result_digest().empty());
    EXPECT_FALSE(store->abandoned_preparation("scenario-1"));
    EXPECT_EQ(batches.finish(source.draft.id(), 10).phase(), research::proto::BATCH_STOPPED);
    EXPECT_EQ(
        batches.state(source.draft.id()).members(1).admission(),
        research::proto::BATCH_MEMBER_WAITING);
}

TEST_F(BatchFixture, UnrelatedTargetIdentityIsNeitherAdoptedNorCancelledAsABatchMember)
{
    research::Batches batches(*store, source.owner, sqlite::Database::Access::writer);
    batches.apply(save(), &definition);
    batches.apply(start());
    auto unrelated = task_submission(*store, "scenario-1", source.verified.result_task().input());
    unrelated.set_program_artifact(definition.program_artifact());
    tasks::submit(*store, std::move(unrelated));
    const auto actual = store->describe("scenario-1").SerializeAsString();
    EXPECT_THROW(
        batches.begin_member(
            source.draft.id(),
            "scenario-1",
            sha256_bytes(input(1).SerializeAsString())),
        Error);
    protocol::proto::Error error;
    error.set_code("conflict");
    error.set_message("Target is already owned by another task");
    const auto refused = batches.reject_member(source.draft.id(), "scenario-1", error);
    EXPECT_EQ(refused.members(0).admission(), research::proto::BATCH_MEMBER_REJECTED);
    EXPECT_TRUE(batches.member_tasks(refused).empty());
    EXPECT_EQ(store->describe("scenario-1").SerializeAsString(), actual);
    EXPECT_FALSE(store->describe("scenario-1").metadata().derivation().has_batch());
}

TEST_F(BatchFixture, NativeDraftAndReplyKeepExactOwnerBudgetsAndUnknownElapsed)
{
    research::Batches batches(*store, source.owner, sqlite::Database::Access::writer);
    const auto state = batches.apply(save(), &definition);
    const auto decoded = protocol::decode_backtest_batch_state(state);
    auto draft = decoded.at("definition");
    for (const auto* key :
        {"object",
            "owner",
            "source_input_sha256",
            "program_artifact",
            "risk_artifact",
            "data_owner",
            "work_units_per_run",
            "recorded_at_ms"})
        draft.erase(key);
    draft["id"] = source.draft.id();
    EXPECT_EQ(
        protocol::encode_backtest_batch_draft(draft).SerializeAsString(),
        source.draft.SerializeAsString());
    for (const auto value : {Json(-1), Json(0.5), Json(4294967296ULL)}) {
        auto changed = draft;
        changed["time_budget_seconds"] = value;
        EXPECT_THROW(protocol::encode_backtest_batch_draft(changed), std::invalid_argument);
    }
    auto changed = draft;
    changed["resume_automatically"] = true;
    EXPECT_THROW(protocol::encode_backtest_batch_draft(changed), Error);
    task::proto::ResearchBatchReply reply;
    *reply.mutable_state() = state;
    research::proto::BacktestBatchRequest request;
    request.mutable_read()->set_id(source.draft.id());
    EXPECT_TRUE(
        protocol::decode_backtest_batch_reply(reply, request, source.owner)
            .at("state")
            .at("elapsed_ms")
            .is_null());
    auto wrong = source.owner;
    wrong.set_service_id("another-research");
    EXPECT_THROW(
        protocol::decode_backtest_batch_reply(reply, request, wrong),
        std::invalid_argument);
    request.mutable_read()->set_id("another-batch");
    EXPECT_THROW(
        protocol::decode_backtest_batch_reply(reply, request, source.owner),
        std::invalid_argument);
    request.clear_read();
    request.mutable_list()->set_limit(1);
    reply.clear_state();
    *reply.add_states() = state;
    reply.set_next_after(source.draft.id());
    EXPECT_EQ(
        protocol::decode_backtest_batch_reply(reply, request, source.owner).at("states").size(),
        1U);
    reply.set_next_after("another-cursor");
    EXPECT_THROW(
        protocol::decode_backtest_batch_reply(reply, request, source.owner),
        std::invalid_argument);
}

TEST_F(BatchFixture, NativeProjectionRequiresActualAdmittedTaskAndKeepsTerminalMeaning)
{
    research::Batches batches(*store, source.owner, sqlite::Database::Access::writer);
    batches.apply(save(), &definition);
    batches.apply(start());
    const auto fixed = input(1);
    batches.begin_member(source.draft.id(), "scenario-1", sha256_bytes(fixed.SerializeAsString()));
    auto submission = task_submission(*store, "scenario-1", fixed);
    submission.set_program_artifact(definition.program_artifact());
    task::proto::TaskDerivation origin;
    origin.set_source_id("source");
    origin.set_source_result_digest(source.draft.source().version());
    origin.set_data_choice("fixed");
    *origin.mutable_batch() = protocol::backtest_batch_object(definition);
    submission.set_derivation(origin);
    tasks::submit(*store, std::move(submission));
    const auto state = batches.confirm_member(source.draft.id(), "scenario-1");
    research::proto::BacktestBatchRequest request;
    request.mutable_read()->set_id(source.draft.id());
    task::proto::ResearchBatchReply reply;
    *reply.mutable_state() = state;
    EXPECT_THROW(
        protocol::decode_backtest_batch_reply(reply, request, source.owner),
        std::invalid_argument);
    for (const auto& task : batches.member_tasks(state))
        *reply.add_members() = task;
    EXPECT_EQ(
        protocol::decode_backtest_batch_reply(reply, request, source.owner)
            .at("members")
            .at(0)
            .at("state"),
        "queued");
    const auto original = reply;
    reply.mutable_members(0)
        ->mutable_metadata()
        ->mutable_derivation()
        ->mutable_batch()
        ->set_version(std::string(64, 'f'));
    EXPECT_THROW(
        protocol::decode_backtest_batch_reply(reply, request, source.owner),
        std::invalid_argument);
    reply = original;
    batches.apply(stop());
    store->commit(store->cancel("scenario-1"));
    const auto stopped = batches.finish(source.draft.id(), 0);
    *reply.mutable_state() = stopped;
    EXPECT_THROW(
        protocol::decode_backtest_batch_reply(reply, request, source.owner),
        std::invalid_argument);
    reply.clear_members();
    for (const auto& task : batches.member_tasks(stopped))
        *reply.add_members() = task;
    EXPECT_EQ(
        protocol::decode_backtest_batch_reply(reply, request, source.owner)
            .at("members")
            .at(0)
            .at("state"),
        "cancelled");
}

TEST_F(BatchFixture, ColdCataloguePreservesPendingWithoutWritesAndRejectsMissingDefinition)
{
    std::string expected;
    {
        research::Batches batches(*store, source.owner, sqlite::Database::Access::writer);
        batches.apply(save(), &definition);
        batches.apply(start());
        expected = batches
                       .begin_member(
                           source.draft.id(),
                           "scenario-1",
                           sha256_bytes(input(1).SerializeAsString()))
                       .SerializeAsString();
        resource::proto::CatalogueQuery query;
        query.set_kind("backtest_batch");
        query.set_limit(1);
        const auto page = batches.resources(query);
        ASSERT_EQ(page.items_size(), 1);
        EXPECT_EQ(
            page.items(0).object().SerializeAsString(),
            protocol::backtest_batch_object(definition).SerializeAsString());
        EXPECT_EQ(page.items(0).state(), resource::proto::CatalogueEntry::SEALED);
    }
    store.reset();
    const auto before = sha256_file(root / "tasks.sqlite");
    int visited = 0;
    research::Batches::inspect_backup(root, source.owner, {}, [&](const auto& state) {
        ++visited;
        EXPECT_EQ(state.SerializeAsString(), expected);
        EXPECT_EQ(state.phase(), research::proto::BATCH_RUNNING);
        EXPECT_FALSE(state.has_elapsed_ms());
    });
    EXPECT_EQ(visited, 1);
    EXPECT_EQ(sha256_file(root / "tasks.sqlite"), before);
    std::stop_source cancelled;
    cancelled.request_stop();
    EXPECT_THROW(
        research::Batches::inspect_backup(
            root,
            source.owner,
            {},
            [](const auto&) {},
            cancelled.get_token()),
        Error);
    EXPECT_EQ(sha256_file(root / "tasks.sqlite"), before);
    {
        sqlite::Database db(root / "tasks.sqlite", sqlite::Database::Access::writer);
        db.execute("DELETE FROM research_batch_states");
    }
    EXPECT_THROW(
        research::Batches::inspect_backup(root, source.owner, {}, [](const auto&) {}),
        Error);
}

namespace {

research::proto::BatchAssessment assessment_fixture()
{
    Source source;
    research::proto::BatchAssessment result;
    auto& snapshot = *result.mutable_snapshot();
    *snapshot.mutable_batch()->mutable_definition() = source.capture();
    snapshot.mutable_batch()->set_phase(research::proto::BATCH_SAVED);
    for (const auto& member : source.draft.members()) {
        auto& receipt = *snapshot.mutable_batch()->add_members();
        receipt.set_task_id(member.task_id());
        receipt.set_admission(research::proto::BATCH_MEMBER_WAITING);
    }
    auto& original = *snapshot.mutable_source();
    original.set_id("source");
    original.set_state(task::proto::SUCCEEDED);
    original.set_attempt(1);
    original.set_input_sha256(source.input_digest);
    original.set_result_sha256(source.verified.result_task().result_digest());
    original.set_program_artifact(std::string(64, 'a'));
    original.set_risk_artifact(std::string(64, 'b'));
    auto& account = *original.mutable_account();
    const auto& input = source.verified.result_task().input();
    const auto& facts = source.verified.backtest();
    account.set_currency(input.paper().contracts(0).dataset().contract().currency());
    account.set_first_day(facts.settlements(0).trading_day());
    account.set_last_day(facts.settlements(facts.settlements_size() - 1).trading_day());
    account.set_settlement_days(facts.settlements_size());
    *account.mutable_deposit() = input.paper().deposit();
    *account.mutable_closing_equity() = facts.account().equity();
    *account.mutable_fees() = facts.account().fees();
    account.mutable_net_profit()->set_units(
        (Decimal::from_raw(account.closing_equity().units())
            - Decimal::from_raw(account.deposit().units()))
            .raw());
    auto& declaration = *result.mutable_declaration();
    declaration.set_id("first-assessment");
    *declaration.mutable_batch() = protocol::backtest_batch_object(snapshot.batch().definition());
    declaration.set_expected_snapshot(protocol::batch_assessment_snapshot_digest(snapshot));
    declaration.set_question("Is the declared cost range supported?");
    declaration.set_scope("Original finite synthetic cost manifest");
    declaration.set_judgment(research::proto::INSUFFICIENT);
    declaration.set_reason("No declared member has been admitted");
    declaration.set_selection_reason("No winner selected");
    declaration.set_unselected_reason("No published member result");
    declaration.set_information_use(
        "The original source was already observed; independence is not established");
    *result.mutable_owner() = source.owner;
    result.set_program_artifact(std::string(64, 'd'));
    result.set_recorded_at_ms(3);
    result.set_digest(protocol::batch_assessment_digest(result));
    return result;
}

} // namespace

TEST(ResearchBatchAssessments, UnadmittedMembersRemainVisibleWithoutInventedTasksOrARequiredWinner)
{
    const auto review = assessment_fixture();
    EXPECT_NO_THROW(protocol::validate_batch_assessment(review));
    EXPECT_EQ(review.snapshot().batch().members_size(), 2);
    EXPECT_EQ(review.snapshot().members_size(), 0);
    EXPECT_TRUE(review.declaration().selected_task_id().empty());
    const auto object = protocol::batch_assessment_object(review);
    EXPECT_EQ(object.kind(), "backtest_batch_assessment");
    EXPECT_EQ(object.version(), review.digest());
    auto substituted = review;
    substituted.mutable_snapshot()->mutable_source()->set_result_sha256(std::string(64, 'f'));
    EXPECT_THROW(protocol::validate_batch_assessment(substituted), std::invalid_argument);
}

TEST(
    ResearchBatchAssessments,
    FailedOrMissingAdmittedMembersCannotBecomeZeroProfitOrSuccessfulSelections)
{
    auto review = assessment_fixture();
    auto& snapshot = *review.mutable_snapshot();
    snapshot.mutable_batch()->set_phase(research::proto::BATCH_RUNNING);
    snapshot.mutable_batch()->set_started_at_ms(2);
    auto& receipt = *snapshot.mutable_batch()->mutable_members(0);
    receipt.set_admission(research::proto::BATCH_MEMBER_ADMITTED);
    receipt.set_input_sha256(std::string(64, 'c'));
    EXPECT_THROW(protocol::validate_batch_assessment_snapshot(snapshot), std::invalid_argument);
    auto& failed = *snapshot.add_members();
    failed.set_id(receipt.task_id());
    failed.set_state(task::proto::FAILED);
    failed.set_attempt(1);
    failed.set_error("Synthetic rejected evidence, not a zero return");
    failed.set_input_sha256(receipt.input_sha256());
    failed.set_program_artifact(snapshot.batch().definition().program_artifact());
    failed.set_risk_artifact(snapshot.batch().definition().risk_artifact());
    EXPECT_NO_THROW(protocol::validate_batch_assessment_snapshot(snapshot));
    auto invented = snapshot;
    *invented.mutable_members(0)->mutable_account() = snapshot.source().account();
    EXPECT_THROW(protocol::validate_batch_assessment_snapshot(invented), std::invalid_argument);
    review.mutable_declaration()->set_expected_snapshot(
        protocol::batch_assessment_snapshot_digest(snapshot));
    review.mutable_declaration()->set_selected_task_id(failed.id());
    review.set_digest(protocol::batch_assessment_digest(review));
    EXPECT_THROW(protocol::validate_batch_assessment(review), std::invalid_argument);
}

namespace {

research::proto::BatchAssessmentMutation assessment_command(
    const research::proto::BatchAssessmentSnapshot& snapshot,
    const std::string& id = "assessment-one",
    const std::string& operation = "assessment-command")
{
    research::proto::BatchAssessmentMutation command;
    command.set_id(operation);
    auto& declaration = *command.mutable_save();
    declaration.set_id(id);
    *declaration.mutable_batch() = protocol::backtest_batch_object(snapshot.batch().definition());
    declaration.set_expected_snapshot(protocol::batch_assessment_snapshot_digest(snapshot));
    declaration.set_question("Does the declared finite cost range support a selection?");
    declaration.set_scope("Only the original synthetic finite manifest");
    declaration.set_judgment(research::proto::INSUFFICIENT);
    declaration.set_reason("Declared cost evidence is incomplete");
    declaration.set_selection_reason("No winner selected");
    declaration.set_unselected_reason("Incomplete cost evidence is not a zero return");
    declaration.set_information_use(
        "The original source was observed; independence is not established");
    return command;
}

} // namespace

TEST_F(
    BatchFixture,
    AssessmentTransactionFailureReceiptReplayRestartAndCanonicalHoldsPreserveNoSelection)
{
    std::string saved;
    {
        research::Batches batches(*store, source.owner, sqlite::Database::Access::writer);
        research::proto::BatchAssessmentSnapshot snapshot;
        *snapshot.mutable_batch() = batches.apply(save(), &definition);
        *snapshot.mutable_source() = research::capture_batch_assessment_task(
            source.verified.result_task(),
            source.input_digest,
            &source.verified);
        const auto command = assessment_command(snapshot);
        EXPECT_TRUE(batches.assessments({}, {}, 10).empty());
        sqlite::fail_next_commits_for_testing(1);
        EXPECT_THROW(
            batches.save_assessment(command, snapshot, definition.program_artifact()),
            std::exception);
        EXPECT_FALSE(batches.assessment_receipt(command));
        EXPECT_TRUE(batches.assessments({}, {}, 10).empty());
        const auto value =
            batches.save_assessment(command, snapshot, definition.program_artifact());
        saved = value.SerializeAsString();
        EXPECT_TRUE(value.declaration().selected_task_id().empty());
        EXPECT_EQ(
            batches.save_assessment(command, snapshot, definition.program_artifact())
                .SerializeAsString(),
            saved);
        auto changed = command;
        changed.mutable_save()->set_reason("Changed decision cannot reuse an operation identity");
        EXPECT_THROW(batches.assessment_receipt(changed), Error);
        changed = command;
        changed.set_id("another-operation");
        EXPECT_THROW(
            batches.save_assessment(changed, snapshot, definition.program_artifact()),
            Error);
        const auto consumers = batches.assessment_consumers("source");
        EXPECT_NE(std::ranges::find(consumers, "assessment-one"), consumers.end());
    }
    store.reset();
    store = std::make_unique<tasks::Store>(root, tasks::Identity{"research-one"});
    research::Batches view(*store, source.owner);
    EXPECT_EQ(view.assessment("assessment-one").SerializeAsString(), saved);
    EXPECT_EQ(view.assessments(source.draft.id(), {}, 10).size(), 1U);
    EXPECT_FALSE(store->find_task("scenario-1"));
}

TEST_F(
    BatchFixture,
    AssessmentOldPreviewIsRejectedAndLaterInterpretationAppendsWithoutReplacingEvidence)
{
    research::Batches batches(*store, source.owner, sqlite::Database::Access::writer);
    research::proto::BatchAssessmentSnapshot snapshot;
    *snapshot.mutable_batch() = batches.apply(save(), &definition);
    *snapshot.mutable_source() = research::capture_batch_assessment_task(
        source.verified.result_task(),
        source.input_digest,
        &source.verified);
    const auto stale = assessment_command(snapshot);
    const auto running = batches.apply(start());
    EXPECT_THROW(batches.save_assessment(stale, snapshot, definition.program_artifact()), Error);
    EXPECT_FALSE(batches.assessment_receipt(stale));
    *snapshot.mutable_batch() = running;
    const auto first = batches.save_assessment(
        assessment_command(snapshot),
        snapshot,
        definition.program_artifact());
    *snapshot.mutable_batch() = batches.apply(stop());
    auto next = assessment_command(snapshot, "assessment-two", "second-assessment-command");
    next.mutable_save()->set_previous_id(first.declaration().id());
    next.mutable_save()->set_revision_reason(
        "Explicitly stopped before cost members were admitted");
    next.mutable_save()->set_validation_information_used(true);
    next.mutable_save()->set_information_use(
        "Previously observed validation information affected this post-hoc interpretation");
    const auto second = batches.save_assessment(next, snapshot, definition.program_artifact());
    EXPECT_NE(first.digest(), second.digest());
    EXPECT_EQ(
        batches.assessment(first.declaration().id()).SerializeAsString(),
        first.SerializeAsString());
    EXPECT_TRUE(second.declaration().validation_information_used());
    EXPECT_EQ(batches.assessments(source.draft.id(), {}, 10).size(), 2U);
}

TEST_F(
    BatchFixture,
    AssessmentCancellationAfterPreviewRequiresNewObservationAndCannotInventMemberAccount)
{
    research::Batches batches(*store, source.owner, sqlite::Database::Access::writer);
    batches.apply(save(), &definition);
    batches.apply(start());
    const auto fixed = input(1);
    const auto digest = sha256_bytes(fixed.SerializeAsString());
    batches.begin_member(source.draft.id(), "scenario-1", digest);
    auto submission = task_submission(*store, "scenario-1", fixed);
    submission.set_program_artifact(definition.program_artifact());
    task::proto::TaskDerivation derivation;
    derivation.set_source_id("source");
    derivation.set_source_result_digest(source.draft.source().version());
    derivation.set_data_choice("fixed");
    *derivation.mutable_batch() = protocol::backtest_batch_object(definition);
    submission.set_derivation(derivation);
    tasks::submit(*store, std::move(submission));
    research::proto::BatchAssessmentSnapshot snapshot;
    *snapshot.mutable_batch() = batches.confirm_member(source.draft.id(), "scenario-1");
    *snapshot.mutable_source() = research::capture_batch_assessment_task(
        source.verified.result_task(),
        source.input_digest,
        &source.verified);
    *snapshot.add_members() =
        research::capture_batch_assessment_task(store->describe("scenario-1"), digest);
    const auto old = assessment_command(snapshot);
    store->commit(store->cancel("scenario-1"));
    EXPECT_THROW(batches.save_assessment(old, snapshot, definition.program_artifact()), Error);
    *snapshot.mutable_members(0) =
        research::capture_batch_assessment_task(store->describe("scenario-1"), digest);
    const auto value = batches.save_assessment(
        assessment_command(snapshot),
        snapshot,
        definition.program_artifact());
    EXPECT_EQ(value.snapshot().members(0).state(), task::proto::CANCELLED);
    EXPECT_FALSE(value.snapshot().members(0).has_account());
    EXPECT_EQ(
        value.snapshot().batch().members(1).admission(),
        research::proto::BATCH_MEMBER_WAITING);
    const auto consumers = batches.assessment_consumers("scenario-1");
    EXPECT_NE(std::ranges::find(consumers, "assessment-one"), consumers.end());
}

TEST_F(
    BatchFixture,
    AssessmentColdInspectionIsReadOnlyAndMissingManifestCannotSilentlyReleaseEvidence)
{
    std::string expected;
    {
        research::Batches batches(*store, source.owner, sqlite::Database::Access::writer);
        research::proto::BatchAssessmentSnapshot snapshot;
        *snapshot.mutable_batch() = batches.apply(save(), &definition);
        *snapshot.mutable_source() = research::capture_batch_assessment_task(
            source.verified.result_task(),
            source.input_digest,
            &source.verified);
        expected = batches
                       .save_assessment(
                           assessment_command(snapshot),
                           snapshot,
                           definition.program_artifact())
                       .SerializeAsString();
    }
    store.reset();
    const auto before = sha256_file(root / "tasks.sqlite");
    int visited = 0;
    research::Batches::inspect_assessments(root, source.owner, [&](const auto& value) {
        ++visited;
        EXPECT_EQ(value.SerializeAsString(), expected);
    });
    EXPECT_EQ(visited, 1);
    EXPECT_EQ(sha256_file(root / "tasks.sqlite"), before);
    {
        sqlite::Database isolated(root / "tasks.sqlite", sqlite::Database::Access::writer);
        isolated.execute("DELETE FROM research_batch_states");
    }
    store = std::make_unique<tasks::Store>(root, tasks::Identity{"research-one"});
    research::Batches view(*store, source.owner);
    EXPECT_THROW(view.assessment_consumers("source"), Error);
}

TEST_F(BatchFixture, AssessmentRealResearchPreviewDoesNotAdmitMembersAndSaveReplaysAcrossRestart)
{
    using namespace std::chrono_literals;
    {
        research::Batches batches(*store, source.owner, sqlite::Database::Access::writer);
        batches.apply(save(), &definition);
    }
    store.reset();
    runtime = std::filesystem::temp_directory_path()
        / ("ast-assess-" + unique_process_id().substr(0, 12));
    ASSERT_TRUE(std::filesystem::create_directory(runtime));
    std::filesystem::permissions(runtime, std::filesystem::perms::owner_all);
    replace_file_durably(runtime / "research.json", R"({"task_concurrency":2})");
    const auto endpoint = (runtime / "rpc.sock").string();
    testing_support::DataDiscovery discovery(
        endpoint + ".discovery",
        "absent-data",
        endpoint + ".absent-data");
    std::unique_ptr<ChildProcess> service;
    const auto recorder = sha256_file(ASTERION_RESEARCH_PATH);
    const auto raw = [&](research::proto::ResearchRequest query) {
        query.set_version(1);
        query.set_service_id(source.owner.service_id());
        query.set_correlation_id(unique_process_id());
        auto channel = ipc::Channel::connect(endpoint, 2s);
        channel.send(query.SerializeAsString(), 2s);
        task::proto::TaskResponse reply;
        if (!reply.ParseFromString(channel.receive(10s)))
            throw std::runtime_error("invalid assessment fixture response");
        protocol::validate_message(reply);
        if (reply.version() != query.version() || reply.service_id() != query.service_id()
            || reply.correlation_id() != query.correlation_id())
            throw std::runtime_error("assessment fixture response identity mismatch");
        return reply;
    };
    const auto call = [&](research::proto::ResearchRequest query) {
        auto reply = raw(std::move(query));
        if (reply.has_error())
            throw std::runtime_error(reply.error().message());
        return reply;
    };
    const auto start = [&] {
        std::filesystem::remove(endpoint);
        std::filesystem::remove(endpoint + ".health");
        service = std::make_unique<ChildProcess>(
            ASTERION_RESEARCH_PATH,
            std::vector<std::string>{
                "--config",
                (runtime / "research.json").string(),
                "--directory",
                root.string(),
                "--endpoint",
                endpoint,
                "--health-endpoint",
                endpoint + ".health",
                "--session",
                source.owner.service_id(),
                "--node-id",
                "fixture-node",
                "--discovery-endpoint",
                endpoint + ".discovery",
                "--file-workers",
                "1"
            },
            false,
            runtime / ("service-" + unique_process_id() + ".log"),
            true);
        discovery.research(source.owner.service_id(), service->id(), recorder);
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        for (;;) {
            try {
                research::proto::ResearchRequest query;
                query.mutable_heartbeat();
                if (call(query).health().initialized())
                    break;
            } catch (const std::exception&) {
                if (service->exited())
                    throw;
            }
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("assessment Research fixture did not initialize");
            std::this_thread::sleep_for(20ms);
        }
    };
    const auto archives = [&] {
        std::set<std::string> result;
        for (const auto& file : std::filesystem::recursive_directory_iterator(root / "programs"))
            result.insert(file.path().lexically_relative(root / "programs").generic_string());
        return result;
    };
    start();
    const auto before = archives();
    research::proto::ResearchRequest query;
    *query.mutable_assessment()->mutable_preview() = protocol::backtest_batch_object(definition);
    const auto preview = call(query).assessment();
    ASSERT_TRUE(preview.has_preview());
    EXPECT_EQ(preview.preview().batch().phase(), research::proto::BATCH_SAVED);
    EXPECT_EQ(preview.preview().batch().members_size(), 2);
    EXPECT_EQ(preview.preview().members_size(), 0);
    EXPECT_EQ(archives(), before);
    // The domain deliberately retains SQLite's exclusive owner lock. Inspect
    // the stopped owner cut, then restart; never bypass its live lock.
    service.reset();
    {
        sqlite::Database read(root / "tasks.sqlite", sqlite::Database::Access::read_only);
        sqlite::Database::Statement tables(
            read,
            "SELECT count(*) FROM sqlite_master WHERE name IN ('research_batch_assessments','research_batch_assessment_commands')");
        ASSERT_TRUE(tables.step());
        EXPECT_EQ(tables.integer(0), 0);
    }
    start();
    const auto command = assessment_command(preview.preview());
    query.Clear();
    *query.mutable_assessment()->mutable_mutation() = command;
    const auto saved = call(query).assessment().assessment();
    protocol::validate_batch_assessment(saved);
    EXPECT_EQ(saved.program_artifact(), recorder);
    EXPECT_EQ(saved.snapshot().source().program_artifact(), definition.program_artifact());
    EXPECT_NE(saved.program_artifact(), saved.snapshot().source().program_artifact());
    EXPECT_EQ(call(query).assessment().assessment().SerializeAsString(), saved.SerializeAsString());
    auto changed = query;
    changed.mutable_assessment()->mutable_mutation()->mutable_save()->set_reason(
        "Changed content cannot reuse this operation");
    EXPECT_TRUE(raw(changed).has_error());
    research::proto::ResearchRequest retirement;
    retirement.mutable_retirement()->set_id("source");
    const auto held = call(retirement).retirement();
    EXPECT_EQ(held.state(), task::proto::TaskRetirement::BLOCKED);
    EXPECT_NE(
        std::ranges::find(held.consumers(), "batch-assessment:assessment-one"),
        held.consumers().end());
    service.reset();
    start();
    EXPECT_EQ(call(query).assessment().assessment().SerializeAsString(), saved.SerializeAsString());
    query.Clear();
    query.mutable_assessment()->set_read(saved.declaration().id());
    EXPECT_EQ(call(query).assessment().assessment().SerializeAsString(), saved.SerializeAsString());
    query.Clear();
    auto& page = *query.mutable_assessment()->mutable_list();
    page.set_batch_id(source.draft.id());
    page.set_limit(10);
    const auto list = call(query).assessment();
    ASSERT_EQ(list.assessments_size(), 1);
    EXPECT_EQ(list.assessments(0).digest(), saved.digest());
    query.Clear();
    query.mutable_batch()->mutable_read()->set_id(source.draft.id());
    const auto actual = call(query).batch();
    EXPECT_EQ(actual.state().phase(), research::proto::BATCH_SAVED);
    EXPECT_EQ(actual.members_size(), 0);
    service.reset();
}

TEST_F(BatchFixture, AssessmentColdRecoveryRejectsAnOrphanedIdempotencyReceipt)
{
    {
        research::Batches batches(*store, source.owner, sqlite::Database::Access::writer);
        research::proto::BatchAssessmentSnapshot snapshot;
        *snapshot.mutable_batch() = batches.apply(save(), &definition);
        *snapshot.mutable_source() = research::capture_batch_assessment_task(
            source.verified.result_task(),
            source.input_digest,
            &source.verified);
        (void)batches.save_assessment(
            assessment_command(snapshot),
            snapshot,
            definition.program_artifact());
    }
    store.reset();
    {
        sqlite::Database isolated(root / "tasks.sqlite", sqlite::Database::Access::writer);
        isolated.execute(
            "UPDATE research_batch_assessment_commands SET assessment_id='unrecorded-decision'");
    }
    EXPECT_THROW(
        research::Batches::inspect_assessments(root, source.owner, [](const auto&) {}),
        Error);
}

TEST(ResearchBatchAssessments, NativeReplyBindsExactOwnerPreviewAndAcknowledgedHumanDeclaration)
{
    const auto fixed = assessment_fixture();
    research::proto::BatchAssessmentRequest request;
    request.set_read(fixed.declaration().id());
    research::proto::BatchAssessmentReply response;
    *response.mutable_assessment() = fixed;
    const auto decoded = protocol::decode_batch_assessment_reply(response, request, fixed.owner());
    EXPECT_EQ(decoded.at("assessment").at("object").at("version"), fixed.digest());
    EXPECT_EQ(
        protocol::encode_batch_assessment_declaration(decoded.at("assessment").at("declaration"))
            .SerializeAsString(),
        fixed.declaration().SerializeAsString());
    auto another = fixed.owner();
    another.set_service_id("another-owner");
    EXPECT_THROW(
        protocol::decode_batch_assessment_reply(response, request, another),
        std::invalid_argument);
    request.Clear();
    request.mutable_mutation()->set_id("save-review-operation");
    *request.mutable_mutation()->mutable_save() = fixed.declaration();
    EXPECT_NO_THROW(protocol::decode_batch_assessment_reply(response, request, fixed.owner()));
    request.mutable_mutation()->mutable_save()->set_reason(
        "A different acknowledged judgment must not be substituted");
    EXPECT_THROW(
        protocol::decode_batch_assessment_reply(response, request, fixed.owner()),
        std::invalid_argument);
    request.Clear();
    *request.mutable_preview() = fixed.declaration().batch();
    response.Clear();
    *response.mutable_preview() = fixed.snapshot();
    response.set_preview_digest(protocol::batch_assessment_snapshot_digest(fixed.snapshot()));
    const auto preview = protocol::decode_batch_assessment_reply(response, request, fixed.owner());
    EXPECT_EQ(preview.at("preview").at("batch").at("members").size(), 2U);
    EXPECT_TRUE(preview.at("preview").at("members").empty());
    EXPECT_TRUE(preview.at("assessment").is_null());
}
