#include "research_models.hpp"
#include "research_execution.hpp"
#include "backtest/engine.hpp"
#include "backtest/model_strategy.hpp"
#include "tasks/paper_input.hpp"
#include <asterion/domain/trading_days.hpp>
#include "model_store.hpp"
#include "strategy_store.hpp"
#include <asterion/protocol/research_strategy.hpp>
#include "sqlite_database.hpp"
#include "factor_engine.hpp"
#include "task_store.hpp"
#include "tasks/task_store_support.hpp"
#include "program_files.hpp"
#include "data/bar_fixture.hpp"
#include <asterion/protocol/research_model.hpp>
#include <asterion/protocol/task_execution.hpp>
#include <asterion/protocol/task.hpp>
#include <asterion/protocol/factor.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/foundation/error.hpp>
#include <gtest/gtest.h>
#include <thread>
#include <array>
#include <algorithm>
#include <cmath>
using namespace asterion;

namespace {

struct ModelSourceFixture : testing::Test
{
    std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("asterion-model-source-" + unique_process_id());
    resource::proto::Owner owner;
    std::unique_ptr<tasks::Store> store;
    task::proto::TaskResponse evidence;
    research::proto::ModelDraft draft;
    std::string program;

    virtual std::vector<MarketBar> source_bars() const
    {
        std::vector<MarketBar> bars;
        for (int i = 0; i < 90; ++i)
            bars.push_back(
                test::flat(
                    "2026-09-25",
                    1790298000000000000LL + i * 60000000000LL,
                    std::to_string(100 + i + i % 3).c_str()));
        return bars;
    }

    void SetUp() override
    {
        ASSERT_TRUE(std::filesystem::create_directory(root));
        owner.set_domain("research");
        owner.set_node_id("fixture-node");
        owner.set_service_id("research-one");
        store = std::make_unique<tasks::Store>(root, tasks::Identity{"research-one"});
        const auto bars = source_bars();
        factor::proto::FactorInput input;
        *input.add_series()->mutable_bars() = test::dataset(bars);
        input.mutable_data_owner()->set_node_id("fixture-node");
        input.mutable_data_owner()->set_service_id("synthetic-data");
        input.set_dataset_revision(protocol::factor_revision(input.series()));
        input.set_factor(protocol::proto::PRICE_MOMENTUM);
        input.add_lookbacks(2);
        input.set_horizon(2);
        input.set_holdout_start(45);
        input.set_retain_observations(true);
        program = sha256_file(current_executable());
        (void)programs::Files(root / "programs", owner).preserve(current_executable(), program);
        auto submission = store->submission("factor-source", input);
        submission.set_program_artifact(program);
        tasks::submit(*store, std::move(submission));
        const auto token = store->commit(store->claim("factor-source")).token();
        store->commit(tasks::finish(*store, "factor-source", token, factor::run(input)));
        evidence = tasks::verified_result(*store, "factor-source");
        draft.set_id("model-draft");
        *draft.mutable_owner() = owner;
        auto& spec = *draft.mutable_specification();
        spec.set_name("Fixed return regression");
        spec.set_purpose("Synthetic post hoc fit");
        spec.set_limitations("Original holdout already observed; historical availability unknown");
        *spec.mutable_source()->mutable_owner() = owner;
        spec.mutable_source()->set_kind("factor_result");
        spec.mutable_source()->set_id("factor-source");
        spec.mutable_source()->set_version(evidence.result_task().result_digest());
        spec.set_method(research::proto::MODEL_LINEAR);
        spec.set_fit_intercept(true);
        spec.set_training_end(12);
        spec.set_weights(research::proto::ModelSpecification::EQUAL);
        spec.set_run_budget(1);
        spec.set_time_budget_seconds(10);
        spec.set_valid_until_ms(10000);
        spec.set_mode(research::proto::ModelSpecification::POST_HOC_DIAGNOSTIC);
        refresh();
    }

    research::proto::ModelMutation save() const
    {
        research::proto::ModelMutation command;
        command.set_id("save-draft");
        command.mutable_save()->set_draft_id(draft.id());
        *command.mutable_save()->mutable_specification() = draft.specification();
        return command;
    }

    research::proto::ModelMutation fix(const research::proto::ModelDraft& value) const
    {
        research::proto::ModelMutation command;
        command.set_id("fix-model");
        command.mutable_fix()->set_plan_id("model-plan");
        command.mutable_fix()->set_draft_id(value.id());
        command.mutable_fix()->set_expected_digest(value.digest());
        return command;
    }

    void refresh()
    {
        draft.set_digest(protocol::model_draft_digest(draft));
    }

    research::proto::ModelPlan capture()
    {
        refresh();
        return research::capture_model_plan(
            owner,
            draft,
            "model-plan",
            evidence,
            sha256_file(root / "factor-source/input.pb"),
            std::filesystem::file_size(root / "factor-source/input.pb"),
            program,
            1);
    }

    task::proto::TaskResponse publish_mapping_model()
    {
        draft.mutable_specification()->set_valid_until_ms(
            SystemClock{}.utc_now() / 1000000 + 600000);
        refresh();
        research::Models models(*store, owner, sqlite::Database::Access::writer);
        const auto saved = models.apply(save()).draft();
        const auto fixed = research::capture_model_plan(
            owner,
            saved,
            "model-plan",
            evidence,
            sha256_file(root / "factor-source/input.pb"),
            std::filesystem::file_size(root / "factor-source/input.pb"),
            program,
            1);
        models.apply(fix(saved), &fixed);
        research::proto::ModelTrainingInput input;
        *input.mutable_plan() = fixed;
        tasks::submit(*store, store->submission("mapping-model", input));
        const auto token = store->commit(store->claim("mapping-model")).token();
        task::proto::TaskFinish finish;
        finish.set_id("mapping-model");
        finish.set_token(token);
        *finish.mutable_model_training() =
            research::train_model(input, root, {}, {}, SystemClock{});
        store->commit(tasks::finish(*store, finish));
        return tasks::verified_result(*store, "mapping-model");
    }

    research::proto::ModelMutation mapping_command(const task::proto::TaskResponse& model) const
    {
        research::proto::ModelMutation command;
        command.set_id("save-mapping");
        auto& request = *command.mutable_mapping();
        request.set_mapping_id("return-directions");
        auto& spec = *request.mutable_specification();
        spec.set_name("Synthetic return directions");
        spec.set_purpose("Post hoc mapping only, never position weights");
        auto& object = *spec.mutable_model();
        *object.mutable_owner() = owner;
        object.set_kind(
            model.has_model_time() ? "model_time_validation_result" : "model_training_result");
        object.set_id(model.result_task().id());
        object.set_version(model.result_task().result_digest());
        spec.mutable_short_threshold()->set_units(-1000000);
        spec.mutable_long_threshold()->set_units(1000000);
        spec.set_middle(research::proto::ModelMappingSpecification::NO_NEW_DIRECTION);
        spec.set_valid_until_ms(
            (model.has_model_time()
                    ? model.result_task().model_time().plan().specification().valid_until_ms()
                    : model.result_task().model_training().plan().specification().valid_until_ms())
            - 1000);
        return command;
    }

    void TearDown() override
    {
        sqlite::fail_next_commits_for_testing(0);
        store.reset();
        std::filesystem::remove_all(root);
    }
};

} // namespace

TEST_F(ModelSourceFixture, FixedPlanPinsRealSourceAndExcludesActualCrossBoundaryLabels)
{
    const auto original = evidence.SerializeAsString();
    const auto plan = capture();
    EXPECT_EQ(plan.source().program_artifact(), program);
    EXPECT_EQ(plan.program_artifact(), program);
    EXPECT_TRUE(plan.source().historical_availability_unknown());
    EXPECT_EQ(
        plan.specification().mode(),
        research::proto::ModelSpecification::POST_HOC_DIAGNOSTIC);
    const auto rows =
        research::model_training_indices(plan, evidence.result_task().factor(), evidence.factor());
    EXPECT_EQ(rows, (std::vector<std::uint32_t>{2, 3, 4, 5, 6, 7, 8, 9}));
    EXPECT_EQ(evidence.SerializeAsString(), original);
    draft.mutable_specification()->set_training_end(8);
    const auto shorter = capture();
    EXPECT_EQ(
        research::model_training_indices(
            shorter,
            evidence.result_task().factor(),
            evidence.factor()),
        (std::vector<std::uint32_t>{2, 3, 4, 5}));
    EXPECT_NE(plan.digest(), shorter.digest());
    EXPECT_EQ(plan.specification().training_end(), 12U);
    EXPECT_EQ(protocol::model_plan_object(plan).kind(), "model_plan");
}

TEST_F(ModelSourceFixture, DraftCanKeepMissingBusinessFieldsButCannotBecomeARunnablePlan)
{
    auto complete = draft;
    draft.mutable_specification()->Clear();
    draft.mutable_specification()->set_name("Incomplete research");
    refresh();
    EXPECT_NO_THROW(protocol::validate_model_draft(draft));
    EXPECT_THROW(capture(), std::invalid_argument);
    draft = complete;
    draft.mutable_specification()->set_standardize(true);
    EXPECT_NO_THROW(capture());
    draft = complete;
    draft.mutable_specification()->clear_fit_intercept();
    EXPECT_THROW(capture(), std::invalid_argument);
    draft = complete;
    draft.mutable_specification()->mutable_alpha()->set_units(0);
    EXPECT_THROW(capture(), std::invalid_argument);
    draft.mutable_specification()->set_method(research::proto::MODEL_RIDGE);
    EXPECT_NO_THROW(capture());
    draft = complete;
    draft.mutable_specification()->set_training_end(46);
    EXPECT_THROW(capture(), std::invalid_argument);
    draft = complete;
    draft.mutable_specification()->set_training_end(4);
    EXPECT_THROW(capture(), Error);
}

TEST_F(ModelSourceFixture, ChangedOwnerSourceProgramOrOriginalBytesRefuseFixing)
{
    auto original = draft;
    draft.mutable_specification()->mutable_source()->mutable_owner()->set_service_id(
        "another-research");
    EXPECT_THROW(capture(), Error);
    draft = original;
    draft.mutable_specification()->mutable_source()->set_version(std::string(64, 'f'));
    EXPECT_THROW(capture(), Error);
    draft = original;
    evidence.mutable_result_task()->clear_program_artifact();
    EXPECT_THROW(capture(), std::invalid_argument);
    evidence = tasks::verified_result(*store, "factor-source");
    evidence.mutable_factor()->set_input_count(1);
    EXPECT_THROW(capture(), Error);
}

TEST_F(ModelSourceFixture, ReadOnlyModelDiscoveryDoesNotCreateAnyAuthority)
{
    const auto before = sha256_file(root / "tasks.sqlite");
    research::Models models(*store, owner);
    EXPECT_FALSE(models.draft("missing"));
    EXPECT_TRUE(models.drafts("", 10).empty());
    EXPECT_TRUE(models.plans("", 10).empty());
    EXPECT_TRUE(models.source_consumers("factor-source").empty());
    resource::proto::CatalogueQuery catalogue;
    catalogue.set_limit(10);
    for (const auto* kind : {"model_draft", "model_plan"}) {
        catalogue.set_kind(kind);
        const auto result = models.resources(catalogue);
        EXPECT_TRUE(result.items().empty());
        EXPECT_EQ(result.owner().SerializeAsString(), owner.SerializeAsString());
    }

    EXPECT_THROW(models.plan("missing"), Error);
    EXPECT_THROW(models.apply(save()), Error);
    EXPECT_EQ(sha256_file(root / "tasks.sqlite"), before);
}

TEST_F(
    ModelSourceFixture,
    PersistentModelReceiptsAndImmutablePlansSurviveDraftRevisionAndRestartWithoutTasks)
{
    const auto clock = std::make_shared<ManualClock>(1000000);
    research::proto::ModelPlan original;
    research::proto::ModelMutation fixed;
    std::string revised;
    {
        research::Models models(*store, owner, sqlite::Database::Access::writer, clock);
        const auto saved = models.apply(save());
        EXPECT_EQ(models.apply(save()).SerializeAsString(), saved.SerializeAsString());
        fixed = fix(saved.draft());
        original = capture();
        EXPECT_EQ(
            models.apply(fixed, &original).plan().SerializeAsString(),
            original.SerializeAsString());
        EXPECT_EQ(models.apply(fixed).plan().SerializeAsString(), original.SerializeAsString());
        EXPECT_THROW(store->describe("model-plan"), std::out_of_range);
        auto changed = save();
        changed.set_id("revise-draft");
        changed.mutable_save()->set_expected_digest(saved.draft().digest());
        changed.mutable_save()->mutable_specification()->set_purpose(
            "New question cannot rewrite fixed training conditions");
        revised = models.apply(changed).draft().digest();
        EXPECT_NE(revised, saved.draft().digest());
        EXPECT_EQ(models.plan("model-plan").SerializeAsString(), original.SerializeAsString());
        EXPECT_EQ(
            models.source_consumers("factor-source"),
            (std::vector<std::string>{"model-draft:model-draft", "model-plan:model-plan"}));
        auto reused = save();
        reused.mutable_save()->mutable_specification()->set_name(
            "different body with reused command id");
        EXPECT_THROW(models.apply(reused), Error);
    }
    store.reset();
    store = std::make_unique<tasks::Store>(root, tasks::Identity{"research-one"});
    research::Models restored(*store, owner);
    EXPECT_EQ(restored.draft(draft.id())->digest(), revised);
    EXPECT_EQ(restored.receipt(fixed)->plan().SerializeAsString(), original.SerializeAsString());
    EXPECT_EQ(restored.plan("model-plan").SerializeAsString(), original.SerializeAsString());
    EXPECT_THROW(store->describe("model-plan"), std::out_of_range);
}

TEST_F(ModelSourceFixture, FailedDraftCommitLeavesNoReceiptAndStaleDraftCannotFixPlan)
{
    const auto clock = std::make_shared<ManualClock>(1000000);
    research::Models models(*store, owner, sqlite::Database::Access::writer, clock);
    sqlite::fail_next_commits_for_testing(1);
    EXPECT_THROW(models.apply(save()), std::runtime_error);
    EXPECT_FALSE(models.receipt(save()));
    EXPECT_FALSE(models.draft(draft.id()));
    EXPECT_TRUE(models.source_consumers("factor-source").empty());
    const auto saved = models.apply(save());
    auto stale = save();
    stale.set_id("stale-save");
    EXPECT_THROW(models.apply(stale), Error);
    auto captured = capture();
    auto fixed = fix(saved.draft());
    auto updated = save();
    updated.set_id("new-body");
    updated.mutable_save()->set_expected_digest(saved.draft().digest());
    updated.mutable_save()->mutable_specification()->set_training_end(8);
    models.apply(updated);
    EXPECT_THROW(models.apply(fixed, &captured), Error);
    EXPECT_FALSE(models.receipt(fixed));
    EXPECT_TRUE(models.plans("", 10).empty());
}

TEST_F(
    ModelSourceFixture,
    TrainingAdmissionRequiresOriginalProgramAndUnexpiredPlanButKeepsReadableEvidence)
{
    const auto clock = std::make_shared<ManualClock>(1000000);
    research::Models models(*store, owner, sqlite::Database::Access::writer, clock);
    const auto saved = models.apply(save());
    const auto fixed = fix(saved.draft());
    const auto captured = capture();
    models.apply(fixed, &captured);
    const auto object = protocol::model_plan_object(captured);
    EXPECT_EQ(models.admission(object, program).SerializeAsString(), captured.SerializeAsString());
    EXPECT_THROW(models.admission(object, std::string(64, 'f')), Error);
    clock->advance(10000000000LL);
    EXPECT_THROW(models.admission(object, program), Error);
    EXPECT_EQ(models.plan(captured.id()).SerializeAsString(), captured.SerializeAsString());
    EXPECT_EQ(models.receipt(fixed)->plan().SerializeAsString(), captured.SerializeAsString());
    auto other = owner;
    other.set_node_id("other-node");
    EXPECT_THROW(research::Models wrong(*store, other), Error);
}

TEST_F(
    ModelSourceFixture,
    OriginalRowsProduceCompleteFitAndPredictionsWithoutFilteringByFutureLabels)
{
    research::proto::ModelTrainingInput input;
    *input.mutable_plan() = capture();
    ManualClock clock(1000000);
    std::size_t done = 0, total = 0;
    const auto source_before = sha256_file(root / "factor-source/input.pb");
    const auto result = research::train_model(
        input,
        root,
        {},
        [&](auto completed, auto count) {
            EXPECT_LE(done, completed);
            done = completed;
            total = count;
        },
        clock);
    EXPECT_NO_THROW(protocol::validate_model_training_result(input, result));
    EXPECT_EQ(input.plan().training_samples(), 8U);
    EXPECT_EQ(result.training_samples(), 8U);
    EXPECT_EQ(result.rows_size(), 90);
    EXPECT_EQ(done, 106U);
    EXPECT_EQ(total, 106U);
    EXPECT_EQ(result.fitted_at_ms(), 1);
    EXPECT_FALSE(result.rows(0).has_prediction());
    EXPECT_EQ(
        result.rows(0).prediction_state(),
        research::proto::ModelTrainingRow::FEATURE_UNAVAILABLE);
    for (int i = 2; i < 10; ++i)
        EXPECT_EQ(result.rows(i).reason(), research::proto::ModelTrainingRow::FITTED);
    EXPECT_EQ(result.rows(10).reason(), research::proto::ModelTrainingRow::CROSSED_LABEL);
    EXPECT_EQ(result.rows(11).reason(), research::proto::ModelTrainingRow::CROSSED_LABEL);
    EXPECT_FALSE(evidence.factor().observations(89).has_forward_return());
    EXPECT_TRUE(result.rows(89).has_prediction());
    EXPECT_EQ(result.rows(89).role(), research::proto::ModelTrainingRow::HOLDOUT);
    EXPECT_EQ(sha256_file(root / "factor-source/input.pb"), source_before);
    auto wrong = result;
    wrong.mutable_rows(45)->set_reason(research::proto::ModelTrainingRow::FITTED);
    EXPECT_THROW(protocol::validate_model_training_result(input, wrong), std::invalid_argument);
    wrong = result;
    wrong.mutable_rows(0)->set_prediction(0);
    EXPECT_THROW(protocol::validate_model_training_result(input, wrong), std::invalid_argument);
}

TEST_F(
    ModelSourceFixture,
    ChangingOnlyFutureSourceRowsKeepsFitAndMissingOriginalFilesNeverTriggerReconstruction)
{
    ManualClock clock(1000000);
    research::proto::ModelTrainingInput input;
    *input.mutable_plan() = capture();
    const auto original = research::train_model(input, root, {}, {}, clock);
    std::vector<MarketBar> bars;
    for (int i = 0; i < 90; ++i)
        bars.push_back(
            test::flat(
                "2026-09-25",
                1790298000000000000LL + i * 60000000000LL,
                std::to_string(100 + i + i % 3 + (i >= 12 ? 5000 + i * 50 : 0)).c_str()));
    auto future = evidence.result_task().factor();
    *future.mutable_series(0)->mutable_bars() = test::dataset(bars);
    future.set_dataset_revision(protocol::factor_revision(future.series()));
    auto submission = store->submission("future-source", future);
    submission.set_program_artifact(program);
    tasks::submit(*store, std::move(submission));
    const auto token = store->commit(store->claim("future-source")).token();
    store->commit(tasks::finish(*store, "future-source", token, factor::run(future)));
    const auto published = tasks::verified_result(*store, "future-source");
    auto other = draft;
    other.set_id("future-model-draft");
    other.mutable_specification()->mutable_source()->set_id("future-source");
    other.mutable_specification()->mutable_source()->set_version(
        published.result_task().result_digest());
    other.set_digest(protocol::model_draft_digest(other));
    research::proto::ModelTrainingInput changed;
    *changed.mutable_plan() = research::capture_model_plan(
        owner,
        other,
        "future-model-plan",
        published,
        sha256_file(root / "future-source/input.pb"),
        std::filesystem::file_size(root / "future-source/input.pb"),
        program,
        1);
    const auto actual = research::train_model(changed, root, {}, {}, clock);
    EXPECT_NE(input.plan().source().result().version(), changed.plan().source().result().version());
    EXPECT_DOUBLE_EQ(actual.coefficient(), original.coefficient());
    EXPECT_DOUBLE_EQ(actual.intercept(), original.intercept());
    EXPECT_DOUBLE_EQ(actual.residual_sum_squares(), original.residual_sum_squares());
    EXPECT_EQ(actual.training_samples(), original.training_samples());
    draft.mutable_specification()->set_standardize(true);
    research::proto::ModelTrainingInput standardized;
    *standardized.mutable_plan() = capture();
    const auto original_scaled = research::train_model(standardized, root, {}, {}, clock);
    other.mutable_specification()->set_standardize(true);
    other.set_digest(protocol::model_draft_digest(other));
    *changed.mutable_plan() = research::capture_model_plan(
        owner,
        other,
        "future-standardized-plan",
        published,
        sha256_file(root / "future-source/input.pb"),
        std::filesystem::file_size(root / "future-source/input.pb"),
        program,
        1);
    const auto future_scaled = research::train_model(changed, root, {}, {}, clock);
    EXPECT_EQ(
        future_scaled.standardization().SerializeAsString(),
        original_scaled.standardization().SerializeAsString());
    EXPECT_DOUBLE_EQ(future_scaled.coefficient(), original_scaled.coefficient());
    EXPECT_DOUBLE_EQ(future_scaled.intercept(), original_scaled.intercept());
    EXPECT_DOUBLE_EQ(future_scaled.residual_sum_squares(), original_scaled.residual_sum_squares());
    const auto retained_program = root / "programs" / program;
    std::filesystem::rename(retained_program, root / "held-program");
    EXPECT_THROW(research::train_model(input, root, {}, {}, clock), std::exception);
    std::filesystem::rename(root / "held-program", retained_program);
    std::filesystem::rename(root / "factor-source/input.pb", root / "held-input.pb");
    EXPECT_THROW(research::train_model(input, root, {}, {}, clock), std::exception);
    EXPECT_FALSE(std::filesystem::exists(root / "factor-source/input.pb"));
    std::filesystem::rename(root / "held-input.pb", root / "factor-source/input.pb");
}

TEST_F(ModelSourceFixture, CancellationRealTimeBudgetAndExpiryDoNotReturnAModel)
{
    ManualClock clock(1000000);
    research::proto::ModelTrainingInput input;
    *input.mutable_plan() = capture();
    std::stop_source stopped;
    stopped.request_stop();
    try {
        (void)research::train_model(input, root, stopped.get_token(), {}, clock);
        FAIL() << "cancelled training returned a model";
    } catch (const Error& error) {
        EXPECT_EQ(error.code(), ErrorCode::cancelled);
    }
    draft.mutable_specification()->set_time_budget_seconds(1);
    *input.mutable_plan() = capture();
    bool paused = false;
    const auto before = std::chrono::steady_clock::now();
    try {
        (void)research::train_model(
            input,
            root,
            {},
            [&](auto done, auto) {
                if (!done && !paused) {
                    paused = true;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
                }
            },
            clock);
        FAIL() << "exhausted training budget returned a model";
    } catch (const Error& error) {
        EXPECT_EQ(error.code(), ErrorCode::resource_exhausted);
    }
    EXPECT_GE(std::chrono::steady_clock::now() - before, std::chrono::seconds(1));
    clock.advance(10000000000LL);
    try {
        (void)research::train_model(input, root, {}, {}, clock);
        FAIL() << "expired model returned an artifact";
    } catch (const Error& error) {
        EXPECT_EQ(error.code(), ErrorCode::conflict);
    }
}

TEST_F(ModelSourceFixture, ExecutionPinsPlanProgramAndDoesNotAcquireDataOrUnrelatedParameters)
{
    const auto plan = capture();
    task::proto::Task task;
    task.set_id("training-task");
    task.set_kind(task::proto::MODEL_TRAINING);
    task.set_program_artifact(program);
    *task.mutable_model_training()->mutable_plan() = plan;
    *task.mutable_model_plan() = protocol::model_plan_object(plan);
    const auto execution = protocol::task_execution(task, std::string(64, 'a'));
    EXPECT_EQ(execution.model_training().plan().digest(), plan.digest());
    EXPECT_EQ(execution.program_artifact(), program);
    EXPECT_FALSE(execution.has_data_owner());
    const auto refs = protocol::task_data_references(execution);
    EXPECT_TRUE(refs.minutes.empty());
    EXPECT_TRUE(refs.snapshots.empty());
    auto invalid = task;
    invalid.mutable_model_plan()->set_version(std::string(64, 'b'));
    EXPECT_THROW(protocol::task_execution(invalid, std::string(64, 'a')), std::invalid_argument);
    invalid = task;
    invalid.set_program_artifact(std::string(64, 'b'));
    EXPECT_THROW(protocol::task_execution(invalid, std::string(64, 'a')), std::invalid_argument);
    auto wrong_execution = execution;
    wrong_execution.mutable_data_owner()->set_service_id("unexpected-data");
    EXPECT_THROW(protocol::task_data_references(wrong_execution), std::invalid_argument);
    wrong_execution = execution;
    wrong_execution.set_program_artifact(std::string(64, 'b'));
    EXPECT_THROW(protocol::task_data_references(wrong_execution), std::invalid_argument);
}

TEST_F(ModelSourceFixture, OriginalTaskPublishesModelAndReopensExactResultWithoutRetraining)
{
    draft.mutable_specification()->set_standardize(true);
    research::proto::ModelTrainingInput input;
    *input.mutable_plan() = capture();
    auto submission = store->submission("model-training", input);
    const auto admitted = tasks::submit(*store, std::move(submission));
    EXPECT_EQ(admitted.kind(), task::proto::MODEL_TRAINING);
    EXPECT_EQ(admitted.total(), 122U);
    EXPECT_EQ(admitted.model_plan().version(), input.plan().digest());
    const auto token = store->commit(store->claim("model-training")).token();
    auto read = store->prepare_input("model-training");
    read.load_for_claim();
    const auto attempt = store->confirm_attempt(std::move(read));
    EXPECT_EQ(attempt.execution().model_training().plan().digest(), input.plan().digest());
    ManualClock clock(1000000);
    const auto trained = research::train_model(input, root, {}, {}, clock);
    ASSERT_TRUE(trained.has_standardization());
    EXPECT_EQ(trained.standardization().training_samples(), 8U);
    task::proto::TaskFinish finished;
    finished.set_id("model-training");
    finished.set_token(token);
    *finished.mutable_model_training() = trained;
    store->commit(tasks::finish(*store, finished));
    const auto original = tasks::verified_result(*store, "model-training");
    EXPECT_EQ(original.result_task().state(), task::proto::SUCCEEDED);
    EXPECT_EQ(original.model_training().SerializeAsString(), trained.SerializeAsString());
    const auto projected = protocol::decode_task_result(original, "model-training");
    EXPECT_EQ(projected.at("kind"), "model_training");
    EXPECT_EQ(
        projected.at("experiment").at("plan").at("object").at("version"),
        input.plan().digest());
    EXPECT_TRUE(projected.at("result").at("rows").at(0).at("prediction").is_null());
    EXPECT_EQ(projected.at("result").at("standardization").at("training_samples"), 8);
    auto wrong_payload = original;
    wrong_payload.mutable_model_training()->set_coefficient(trained.coefficient() + 1);
    EXPECT_THROW(
        protocol::decode_task_result(wrong_payload, "model-training"),
        std::invalid_argument);
    store.reset();
    store = std::make_unique<tasks::Store>(root, tasks::Identity{"research-one"});
    const auto reopened = tasks::verified_result(*store, "model-training");
    EXPECT_EQ(reopened.model_training().SerializeAsString(), trained.SerializeAsString());
    EXPECT_EQ(reopened.result_task().result_digest(), original.result_task().result_digest());
    const auto replayed = tasks::submit(*store, "model-training", input);
    EXPECT_EQ(replayed.state(), task::proto::SUCCEEDED);
    EXPECT_EQ(replayed.result_digest(), original.result_task().result_digest());
    auto wrong_program = store->submission("wrong-program", input);
    EXPECT_THROW(wrong_program.set_program_artifact(std::string(64, 'b')), Error);
}

TEST_F(ModelSourceFixture, ResearchExecutorUsesOriginalEvidenceAndRejectsDifferentLoadedProgram)
{
    draft.mutable_specification()->set_valid_until_ms(4102444800000LL);
    research::proto::ModelTrainingInput input;
    *input.mutable_plan() = capture();
    tasks::submit(*store, "executor-model", input);
    const auto token = store->commit(store->claim("executor-model")).token();
    auto read = store->prepare_input("executor-model");
    read.load_for_claim();
    auto attempt = store->confirm_attempt(std::move(read));
    // Restoring the small owned descriptor never reconstructs the original source files.
    attempt.mutable_task()->clear_definition();
    EXPECT_THROW(tasks::execute(attempt, root, {}, {}, std::string(64, 'b')), Error);
    const auto result = tasks::execute(attempt, root, {}, {}, program);
    ASSERT_TRUE(result.has_model_training());
    EXPECT_EQ(result.model_training().training_samples(), 8U);
    EXPECT_EQ(result.model_training().rows_size(), 90);
    protocol::validate_model_training_result(input, result.model_training());
    auto tampered = attempt;
    tampered.mutable_task()->mutable_model_plan()->set_version(std::string(64, 'b'));
    EXPECT_THROW(tasks::execute(tampered, root, {}, {}, program), std::invalid_argument);
    auto stopped = attempt;
    std::stop_source stop;
    stop.request_stop();
    EXPECT_THROW(tasks::execute(stopped, root, stop.get_token(), {}, program), Error);
}

TEST_F(ModelSourceFixture, StandardizationPersistsMatureTrainingScaleAndAppliesItWithoutRefitting)
{
    draft.mutable_specification()->set_standardize(true);
    research::proto::ModelTrainingInput input;
    *input.mutable_plan() = capture();
    ManualClock clock(1000000);
    std::uint64_t done = 0;
    const auto result = research::train_model(
        input,
        root,
        {},
        [&](auto completed, auto total) {
            EXPECT_LE(done, completed);
            EXPECT_EQ(total, 122U);
            done = completed;
        },
        clock);
    ASSERT_TRUE(result.has_standardization());
    const auto& fitted = result.standardization();
    EXPECT_EQ(fitted.training_samples(), 8U);
    long double sum = 0, squared = 0;
    for (int i = 2; i <= 9; ++i)
        sum += evidence.factor().observations(i).value();
    const auto mean = sum / 8;
    for (int i = 2; i <= 9; ++i) {
        const auto delta = evidence.factor().observations(i).value() - mean;
        squared += delta * delta;
    }
    EXPECT_NEAR(fitted.mean(), static_cast<double>(mean), 1e-15);
    EXPECT_NEAR(
        fitted.sample_standard_deviation(),
        static_cast<double>(std::sqrt(squared / 7)),
        1e-15);
    const auto& future = evidence.factor().observations(70);
    const auto projected = (future.value() - fitted.mean()) / fitted.sample_standard_deviation();
    EXPECT_NEAR(
        result.rows(70).prediction(),
        std::fma(result.coefficient(), projected, result.intercept()),
        1e-14);
    EXPECT_EQ(done, 122U);
    auto invalid = result;
    invalid.clear_standardization();
    EXPECT_THROW(protocol::validate_model_training_result(input, invalid), std::invalid_argument);
    invalid = result;
    invalid.mutable_standardization()->set_sample_standard_deviation(0);
    EXPECT_THROW(protocol::validate_model_training_result(input, invalid), std::invalid_argument);
}

TEST_F(
    ModelSourceFixture,
    NativeModelProjectionPreservesExplicitSpecificationAndRejectsWrongReplies)
{
    const auto plan = capture();
    const auto encoded = protocol::decode_model_specification(plan.specification());
    EXPECT_EQ(
        protocol::encode_model_specification(encoded).SerializeAsString(),
        plan.specification().SerializeAsString());
    auto invalid = encoded;
    invalid["training_end"] = -1;
    EXPECT_THROW(protocol::encode_model_specification(invalid), std::invalid_argument);
    invalid = encoded;
    invalid["training_end"] = 12.5;
    EXPECT_THROW(protocol::encode_model_specification(invalid), std::invalid_argument);
    invalid = encoded;
    invalid["extra"] = true;
    EXPECT_THROW(protocol::encode_model_specification(invalid), Error);
    research::proto::ModelRequest request;
    request.mutable_read()->set_kind(research::proto::ModelRead::PLAN);
    request.mutable_read()->set_id(plan.id());
    research::proto::ModelReply reply;
    *reply.mutable_plan() = plan;
    const auto json = protocol::decode_model_reply(reply, request, owner);
    EXPECT_EQ(json.at("plan").at("object").at("version"), plan.digest());
    auto wrong_owner = owner;
    wrong_owner.set_service_id("other-research");
    EXPECT_THROW(protocol::decode_model_reply(reply, request, wrong_owner), std::invalid_argument);
    request.mutable_read()->set_id("other-plan");
    EXPECT_THROW(protocol::decode_model_reply(reply, request, owner), std::invalid_argument);
    request.Clear();
    request.mutable_list()->set_kind(research::proto::ModelRead::PLAN);
    request.mutable_list()->set_limit(2);
    reply.Clear();
    *reply.add_plans() = plan;
    *reply.add_plans() = plan;
    EXPECT_THROW(protocol::decode_model_reply(reply, request, owner), std::invalid_argument);
}

TEST_F(
    ModelSourceFixture,
    FixedPredictionMappingKeepsPublicationIdentityAndOriginalReceiptAcrossRestart)
{
    const auto model = publish_mapping_model();
    const auto before = model.SerializeAsString();
    const auto command = mapping_command(model);
    research::proto::ModelMapping original;
    {
        research::Models models(*store, owner, sqlite::Database::Access::writer);
        const auto captured = models.capture_mapping(command.mapping(), model);
        original = models.apply(command, nullptr, &captured).mapping();
        EXPECT_EQ(original.model_published_at_ms(), model.result_task().updated_at_ms());
        const auto projection = research::project_model_mapping(original, model, 0, 10);
        EXPECT_EQ(projection.total(), 90U);
        EXPECT_EQ(projection.next_begin(), 10U);
        EXPECT_EQ(
            projection.rows(0).intent(),
            research::proto::ModelMappedRow::PREDICTION_UNAVAILABLE);
        EXPECT_FALSE(projection.rows(0).observation().has_prediction());
        EXPECT_EQ(
            projection.rows(2).observation().SerializeAsString(),
            model.model_training().rows(2).SerializeAsString());
        research::proto::ModelRequest query;
        *query.mutable_preview()->mutable_mapping() = protocol::model_mapping_object(original);
        query.mutable_preview()->set_limit(10);
        research::proto::ModelReply preview;
        *preview.mutable_projection() = projection;
        const auto mapped = protocol::decode_model_reply(preview, query, owner).at("projection");
        EXPECT_EQ(mapped.at("scope"), "post_hoc_diagnostic");
        EXPECT_TRUE(mapped.at("rows").at(0).at("prediction").is_null());
        EXPECT_EQ(mapped.at("rows").at(0).at("intent"), "prediction_unavailable");
        preview.mutable_projection()->mutable_rows(0)->mutable_observation()->set_event_index(1);
        EXPECT_THROW(protocol::decode_model_reply(preview, query, owner), std::invalid_argument);
        ManualClock expired((original.specification().valid_until_ms() + 1) * 1000000);
        const auto expired_rows = research::project_model_mapping(original, model, 2, 2, expired);
        for (const auto& row : expired_rows.rows())
            EXPECT_EQ(row.intent(), research::proto::ModelMappedRow::EXPIRED);
        EXPECT_THROW(
            research::project_model_mapping(original, model, 90, 1),
            std::invalid_argument);

        EXPECT_EQ(original.plan().version(), model.result_task().model_plan().version());
        EXPECT_EQ(
            models.apply(command).mapping().SerializeAsString(),
            original.SerializeAsString());
        EXPECT_EQ(
            models.source_consumers("mapping-model"),
            (std::vector<std::string>{"model-mapping:return-directions"}));
        auto changed = command;
        changed.mutable_mapping()->mutable_specification()->mutable_long_threshold()->set_units(
            2000000);
        EXPECT_THROW(models.apply(changed, nullptr, &captured), Error);
        auto new_identity = command;
        new_identity.set_id("replace-mapping");
        EXPECT_THROW(models.apply(new_identity, nullptr, &captured), Error);
        research::proto::ModelRequest request;
        *request.mutable_mutation() = command;
        research::proto::ModelReply reply;
        *reply.mutable_mapping() = original;
        const auto json = protocol::decode_model_reply(reply, request, owner);
        EXPECT_EQ(json.at("mapping").at("mode"), "post_hoc_diagnostic");
        EXPECT_EQ(
            protocol::encode_model_mapping_specification(json.at("mapping").at("specification"))
                .SerializeAsString(),
            command.mapping().specification().SerializeAsString());
    }
    store.reset();
    store = std::make_unique<tasks::Store>(root, tasks::Identity{"research-one"});
    research::Models reopened(*store, owner);
    EXPECT_EQ(reopened.mapping(original.id()).SerializeAsString(), original.SerializeAsString());
    EXPECT_EQ(
        reopened.receipt(command)->mapping().SerializeAsString(),
        original.SerializeAsString());
    EXPECT_EQ(tasks::verified_result(*store, "mapping-model").SerializeAsString(), before);
}

TEST_F(
    ModelSourceFixture,
    PredictionMappingRejectsWrongBytesBandOwnerAndLifetimeWithoutAnAcceptedReceipt)
{
    const auto model = publish_mapping_model();
    const auto command = mapping_command(model);
    research::Models models(*store, owner, sqlite::Database::Access::writer);
    auto wrong = model;
    wrong.mutable_model_training()->set_coefficient(model.model_training().coefficient() + 1);
    EXPECT_THROW(models.capture_mapping(command.mapping(), wrong), Error);
    auto bad = command;
    bad.mutable_mapping()
        ->mutable_specification()
        ->mutable_model()
        ->mutable_owner()
        ->set_service_id("other");
    EXPECT_THROW(models.capture_mapping(bad.mapping(), model), Error);
    bad = command;
    bad.mutable_mapping()->mutable_specification()->mutable_short_threshold()->set_units(1000000);
    EXPECT_THROW(protocol::validate_model_mutation(bad), std::invalid_argument);
    bad = command;
    bad.mutable_mapping()->mutable_specification()->set_valid_until_ms(
        model.result_task().model_training().plan().specification().valid_until_ms() + 1);
    EXPECT_THROW(models.capture_mapping(bad.mapping(), model), Error);
    const auto captured = models.capture_mapping(command.mapping(), model);
    sqlite::fail_next_commits_for_testing(1);
    EXPECT_ANY_THROW(models.apply(command, nullptr, &captured));
    sqlite::fail_next_commits_for_testing(0);
    EXPECT_FALSE(models.receipt(command));
    EXPECT_THROW(models.mapping(command.mapping().mapping_id()), Error);
    EXPECT_TRUE(models.source_consumers("mapping-model").empty());
    EXPECT_EQ(models.apply(command, nullptr, &captured).mapping().digest(), captured.digest());
}

TEST(ModelSourceClocks, NightSessionKeepsTheOriginalTradingDayIndependentOfUtc)
{
    factor::proto::FactorInput input;
    *input.add_series()->mutable_bars() =
        test::dataset({test::flat("2026-09-25", 1790254800000000000LL, "100")});
    const auto clocks = protocol::factor_observation_clocks(input);
    ASSERT_EQ(clocks.size(), 1U);
    EXPECT_EQ(clocks[0].trading_day, "2026-09-25");
    EXPECT_EQ(clocks[0].timestamp_ns, 1790254800000000000LL);
}

TEST_F(
    ModelSourceFixture,
    RetainedTimeValidationFitsEachOriginalFoldAndPublishesOnlyLaterDevelopmentPredictions)
{
    draft.mutable_specification()->set_standardize(true);
    const auto recipe = capture();
    research::proto::ModelTimeSave request;
    request.set_plan_id("time-plan");
    auto& spec = *request.mutable_specification();
    spec.set_name("Explicit chronological development folds");
    spec.set_purpose("Synthetic causal calculation boundary");
    spec.set_limitations("Historical availability unknown; not independent or tradable evidence");
    *spec.mutable_recipe() = protocol::model_plan_object(recipe);
    for (const auto& range :
        std::vector<std::array<unsigned, 4>>{{0, 12, 12, 20}, {8, 20, 20, 28}}) {
        auto& fold = *spec.add_folds();
        fold.set_training_begin(range[0]);
        fold.set_training_end(range[1]);
        fold.set_validation_begin(range[2]);
        fold.set_validation_end(range[3]);
    }
    spec.set_run_budget(2);
    spec.set_time_budget_seconds(10);
    spec.set_valid_until_ms(9000);
    const auto fixed = research::capture_model_time_plan(owner, request, recipe, evidence, 1);
    ASSERT_EQ(fixed.boundaries_size(), 8);
    const std::array<unsigned, 8> indices{0, 12, 12, 20, 8, 20, 20, 28};
    for (int i = 0; i < fixed.boundaries_size(); ++i) {
        EXPECT_EQ(fixed.boundaries(i).event_index(), indices[i]);
        EXPECT_EQ(fixed.boundaries(i).trading_day(), "2026-09-25");
        EXPECT_EQ(
            fixed.boundaries(i).timestamp_ns(),
            evidence.result_task().factor().series(0).bars().bars(indices[i]).timestamp_ns());
    }
    EXPECT_EQ(
        protocol::decode_model_time_plan(fixed).at("boundaries").at(3).at("timestamp_ns"),
        std::to_string(fixed.boundaries(3).timestamp_ns()));
    ASSERT_EQ(fixed.training_counts_size(), 2);
    EXPECT_EQ(fixed.training_counts(0), 8U);
    EXPECT_EQ(fixed.training_counts(1), 10U);
    research::proto::ModelTimeInput input;
    *input.mutable_plan() = fixed;
    ManualClock clock(1000000);
    const auto result = research::validate_model_time(input, root, {}, {}, clock);
    auto forged = input;
    forged.mutable_plan()->mutable_boundaries(0)->set_trading_day("2026-09-24");
    forged.mutable_plan()->set_digest(protocol::model_time_plan_digest(forged.plan()));
    EXPECT_THROW(research::validate_model_time(forged, root, {}, {}, clock), Error);
    ASSERT_EQ(result.folds_size(), 2);
    EXPECT_EQ(result.folds(0).evaluated(), 6U);
    EXPECT_EQ(result.folds(0).training_members(0), 2U);
    EXPECT_EQ(result.folds(0).training_members(7), 9U);
    EXPECT_EQ(result.folds(1).training_members(0), 8U);
    EXPECT_EQ(result.folds(1).training_members(9), 17U);
    EXPECT_EQ(result.folds(0).predictions(0).event_index(), 12U);
    EXPECT_EQ(result.folds(0).predictions(7).event_index(), 19U);
    EXPECT_EQ(
        result.folds(0).predictions(7).exclusion(),
        research::proto::ModelTimeRow::CROSSED_LABEL);
    EXPECT_TRUE(result.folds(0).predictions(7).has_prediction());
    EXPECT_LT(result.folds(1).predictions(7).event_index(), recipe.source().holdout_start());
    research::proto::ModelTrainingInput single;
    *single.mutable_plan() = recipe;
    const auto original = research::train_model(single, root, {}, {}, clock);
    EXPECT_DOUBLE_EQ(result.folds(0).coefficient(), original.coefficient());
    EXPECT_DOUBLE_EQ(result.folds(0).intercept(), original.intercept());
    EXPECT_EQ(
        result.folds(0).standardization().SerializeAsString(),
        original.standardization().SerializeAsString());
    EXPECT_EQ(
        protocol::encode_model_time_specification(protocol::decode_model_time_specification(spec))
            .SerializeAsString(),
        spec.SerializeAsString());
    const auto json = protocol::decode_model_time_result(input, result);
    EXPECT_EQ(json.at("scope"), "development_validation");
    EXPECT_TRUE(json.at("historical_availability_unknown"));
    EXPECT_EQ(json.at("folds").at(0).at("predictions").at(7).at("exclusion"), "crossed_label");
    auto bad = result;
    bad.mutable_folds(0)->set_evaluated(8);
    EXPECT_THROW(protocol::validate_model_time_result(input, bad), std::invalid_argument);
    bad = result;
    bad.mutable_folds(0)->set_training_members(0, 12);
    EXPECT_THROW(protocol::validate_model_time_result(input, bad), std::invalid_argument);
    auto outside = request;
    outside.mutable_specification()->mutable_folds(1)->set_validation_end(46);
    EXPECT_THROW(
        research::capture_model_time_plan(owner, outside, recipe, evidence, 1),
        std::invalid_argument);
    auto changed = evidence;
    changed.mutable_factor()->mutable_observations(20)->set_value(100);
    EXPECT_THROW(research::capture_model_time_plan(owner, request, recipe, changed, 1), Error);
    std::stop_source stop;
    stop.request_stop();
    EXPECT_THROW(research::validate_model_time(input, root, stop.get_token(), {}, clock), Error);
    ManualClock expired(9001000000);
    EXPECT_THROW(research::validate_model_time(input, root, {}, {}, expired), Error);
}

TEST_F(
    ModelSourceFixture,
    FixedTimePlanKeepsOriginalReceiptAndRecipeWithoutStartingOrReplacingTasks)
{
    draft.mutable_specification()->set_standardize(true);
    draft.mutable_specification()->set_valid_until_ms(SystemClock{}.utc_now() / 1000000 + 600000);
    refresh();
    research::proto::ModelTimePlan retained;
    research::proto::ModelTimeInput formal_input;
    task::proto::TaskResponse published_time;
    research::proto::ModelMutation command;
    {
        research::Models models(*store, owner, sqlite::Database::Access::writer);
        const auto saved = models.apply(save()).draft();
        const auto recipe = research::capture_model_plan(
            owner,
            saved,
            "model-plan",
            evidence,
            sha256_file(root / "factor-source/input.pb"),
            std::filesystem::file_size(root / "factor-source/input.pb"),
            program,
            1);
        models.apply(fix(saved), &recipe);
        command.set_id("save-time-plan");
        auto& request = *command.mutable_time_validation();
        request.set_plan_id("time-plan");
        auto& spec = *request.mutable_specification();
        spec.set_name("Bounded chronological development");
        spec.set_purpose("Original synthetic source");
        spec.set_limitations(
            "Unknown historical availability and already-observed holdout, no trading permission");
        *spec.mutable_recipe() = protocol::model_plan_object(recipe);
        for (const auto& range :
            std::vector<std::array<unsigned, 4>>{{0, 12, 12, 20}, {8, 20, 20, 28}}) {
            auto& fold = *spec.add_folds();
            fold.set_training_begin(range[0]);
            fold.set_training_end(range[1]);
            fold.set_validation_begin(range[2]);
            fold.set_validation_end(range[3]);
        }
        spec.set_run_budget(2);
        spec.set_time_budget_seconds(10);
        spec.set_valid_until_ms(recipe.specification().valid_until_ms() - 1000);
        const auto captured = models.capture_time(request, evidence);
        retained = models.apply(command, nullptr, nullptr, &captured).time_plan();
        EXPECT_EQ(retained.training_counts(0), 8U);
        EXPECT_EQ(retained.training_counts(1), 10U);
        EXPECT_EQ(
            models.apply(command).time_plan().SerializeAsString(),
            retained.SerializeAsString());
        EXPECT_EQ(
            models.time_admission(protocol::model_time_plan_object(retained), program)
                .SerializeAsString(),
            retained.SerializeAsString());
        EXPECT_THROW(
            models.time_admission(protocol::model_time_plan_object(retained), std::string(64, 'b')),
            Error);
        EXPECT_THROW(store->describe("time-plan"), std::out_of_range);
        const auto consumers = models.source_consumers("factor-source");
        EXPECT_TRUE(std::ranges::any_of(consumers, [](const auto& value) {
            return value == "model-time-plan:time-plan";
        }));
        resource::proto::CatalogueQuery query;
        query.set_kind("model_time_plan");
        query.set_limit(10);
        const auto catalogue = models.resources(query);
        ASSERT_EQ(catalogue.items_size(), 1);
        EXPECT_EQ(
            catalogue.items(0).object().SerializeAsString(),
            protocol::model_time_plan_object(retained).SerializeAsString());
        research::proto::ModelRequest read;
        read.mutable_read()->set_kind(research::proto::ModelRead::TIME_PLAN);
        read.mutable_read()->set_id(retained.id());
        research::proto::ModelReply reply;
        *reply.mutable_time_plan() = retained;
        EXPECT_EQ(
            protocol::decode_model_reply(reply, read, owner).at("time_plan").at("training_counts"),
            Json::array({8, 10}));
        auto changed = command;
        changed.mutable_time_validation()->mutable_specification()->set_purpose("Different intent");
        EXPECT_THROW(models.apply(changed, nullptr, nullptr, &captured), Error);
        auto failed = command;
        failed.set_id("failed-time-plan");
        failed.mutable_time_validation()->set_plan_id("uncommitted-time-plan");
        const auto candidate = models.capture_time(failed.time_validation(), evidence);
        sqlite::fail_next_commits_for_testing(1);
        EXPECT_ANY_THROW(models.apply(failed, nullptr, nullptr, &candidate));
        sqlite::fail_next_commits_for_testing(0);
        EXPECT_FALSE(models.receipt(failed));
        EXPECT_THROW(models.time_plan(candidate.id()), Error);
        *formal_input.mutable_plan() = retained;
        const auto queued =
            tasks::submit(*store, store->submission("time-validation", formal_input));
        EXPECT_EQ(queued.kind(), task::proto::MODEL_TIME_VALIDATION);
        EXPECT_EQ(queued.total(), 2U);
        EXPECT_EQ(
            queued.model_time_plan().SerializeAsString(),
            protocol::model_time_plan_object(retained).SerializeAsString());
        const auto token = store->commit(store->claim("time-validation")).token();
        auto prepared_read = store->prepare_input("time-validation");
        prepared_read.load_for_claim();
        const auto attempt = store->confirm_attempt(std::move(prepared_read));
        EXPECT_EQ(
            attempt.execution().model_time().plan().SerializeAsString(),
            retained.SerializeAsString());
        EXPECT_FALSE(attempt.execution().has_data_owner());
        task::proto::TaskFinish finish;
        finish.set_id("time-validation");
        finish.set_token(token);
        *finish.mutable_model_time() = research::validate_model_time(formal_input, root);
        store->commit(tasks::finish(*store, finish));
        published_time = tasks::verified_result(*store, "time-validation");
        ASSERT_EQ(published_time.result_task().state(), task::proto::SUCCEEDED);
        EXPECT_EQ(published_time.model_time().folds_size(), 2);
        const auto projected = protocol::decode_task_result(published_time, "time-validation");
        EXPECT_EQ(projected.at("kind"), "model_time_validation");
        EXPECT_EQ(projected.at("result").at("scope"), "development_validation");
        EXPECT_TRUE(projected.at("result").at("historical_availability_unknown"));
        const auto mapping_request = mapping_command(published_time);
        const auto captured_mapping =
            models.capture_mapping(mapping_request.mapping(), published_time);
        const auto fixed_mapping =
            models.apply(mapping_request, nullptr, &captured_mapping).mapping();
        EXPECT_EQ(
            models.apply(mapping_request).mapping().SerializeAsString(),
            fixed_mapping.SerializeAsString());
        EXPECT_EQ(
            fixed_mapping.model_published_at_ms(),
            published_time.result_task().updated_at_ms());
        EXPECT_FALSE(models.source_consumers("time-validation").empty());
        research::proto::ModelDecisionTrialInput decision;
        *decision.mutable_mapping() = fixed_mapping;
        *decision.mutable_time() = formal_input;
        decision.set_model_attempt(published_time.result_task().attempt());
        auto& trial = *decision.mutable_request();
        *trial.mutable_mapping() = protocol::model_mapping_object(fixed_mapping);
        trial.set_fold_index(1);
        trial.set_fictional(true);
        trial.set_purpose("Only explicit retained fold parameters predict this fictional feature");
        trial.set_time_budget_seconds(10);
        auto& next = *trial.add_cases();
        next.set_name("new input on original fold one");
        next.set_boundary_ns(fixed_mapping.recorded_at_ms() * 1000000 + 1000000);
        next.set_feature_available_at_ns(next.boundary_ns());
        next.set_feature(published_time.model_time().folds(1).standardization().mean());
        next.set_expected_intent(research::proto::ModelMappedRow::LONG_DIRECTION);
        next.set_expected_reason(research::proto::MODEL_DECISION_READY);
        ASSERT_GT(published_time.model_time().folds(1).intercept(), 0.01);
        const auto inferred = research::compute_model_decision_trial(decision, root);
        EXPECT_TRUE(inferred.all_match());
        EXPECT_DOUBLE_EQ(
            inferred.rows(0).prediction(),
            published_time.model_time().folds(1).intercept());
        decision.mutable_request()->clear_fold_index();
        EXPECT_THROW(protocol::validate_model_decision_input(decision), std::invalid_argument);
        const auto directions =
            research::project_model_mapping(fixed_mapping, published_time, 0, 9);
        EXPECT_EQ(directions.total(), 16U);
        EXPECT_EQ(directions.next_begin(), 9U);
        ASSERT_EQ(directions.rows_size(), 9);
        EXPECT_EQ(directions.rows(0).time_observation().observation().event_index(), 12U);
        EXPECT_EQ(
            directions.rows(7).time_observation().observation().exclusion(),
            research::proto::ModelTimeRow::CROSSED_LABEL);
        EXPECT_TRUE(directions.rows(7).time_observation().observation().has_prediction());
        EXPECT_NE(
            directions.rows(7).intent(),
            research::proto::ModelMappedRow::PREDICTION_UNAVAILABLE);
        EXPECT_EQ(directions.rows(8).time_observation().fold_index(), 1U);
        EXPECT_EQ(directions.rows(8).time_observation().observation().event_index(), 20U);
        research::proto::ModelRequest preview;
        *preview.mutable_preview()->mutable_mapping() =
            protocol::model_mapping_object(fixed_mapping);
        preview.mutable_preview()->set_limit(9);
        research::proto::ModelReply preview_reply;
        *preview_reply.mutable_projection() = directions;
        const auto rows =
            protocol::decode_model_reply(preview_reply, preview, owner).at("projection").at("rows");
        EXPECT_EQ(rows.at(8).at("index"), 20U);
        EXPECT_EQ(rows.at(8).at("role"), "development_validation");
        EXPECT_EQ(rows.at(7).at("exclusion"), "crossed_label");
        preview_reply.mutable_projection()
            ->mutable_rows(8)
            ->mutable_time_observation()
            ->set_fold_index(0);
        EXPECT_THROW(
            protocol::decode_model_reply(preview_reply, preview, owner),
            std::invalid_argument);
        auto wrong_scope = fixed_mapping;
        wrong_scope.mutable_folds(1)->set_training_begin(7);
        wrong_scope.set_digest(protocol::model_mapping_digest(wrong_scope));
        EXPECT_THROW(research::project_model_mapping(wrong_scope, published_time, 0, 9), Error);
        const auto tail = research::project_model_mapping(fixed_mapping, published_time, 9, 20);
        EXPECT_EQ(tail.rows_size(), 7);
        EXPECT_FALSE(tail.has_next_begin());
        EXPECT_EQ(tail.rows(6).time_observation().observation().event_index(), 27U);
        tasks::submit(*store, store->submission("cancelled-time", formal_input));
        store->commit(store->cancel("cancelled-time"));
        EXPECT_EQ(store->describe("cancelled-time").state(), task::proto::CANCELLED);
    }
    store.reset();
    store = std::make_unique<tasks::Store>(root, tasks::Identity{"research-one"});
    auto expired =
        std::make_shared<ManualClock>((retained.specification().valid_until_ms() + 1) * 1000000);
    research::Models reopened(*store, owner, sqlite::Database::Access::read_only, expired);
    EXPECT_EQ(reopened.time_plan(retained.id()).SerializeAsString(), retained.SerializeAsString());
    EXPECT_EQ(
        reopened.mapping("return-directions").specification().model().id(),
        "time-validation");
    const auto expired_projection = research::project_model_mapping(
        reopened.mapping("return-directions"),
        published_time,
        0,
        16,
        *expired);
    for (const auto& row : expired_projection.rows())
        EXPECT_EQ(row.intent(), research::proto::ModelMappedRow::EXPIRED);
    EXPECT_EQ(
        reopened.receipt(command)->time_plan().SerializeAsString(),
        retained.SerializeAsString());
    EXPECT_THROW(
        reopened.time_admission(protocol::model_time_plan_object(retained), program),
        Error);
    EXPECT_EQ(
        tasks::verified_result(*store, "time-validation").SerializeAsString(),
        published_time.SerializeAsString());
    EXPECT_EQ(
        tasks::submit(*store, "time-validation", formal_input).result_digest(),
        published_time.result_task().result_digest());
    EXPECT_EQ(store->describe("cancelled-time").state(), task::proto::CANCELLED);
}

TEST_F(ModelSourceFixture, NewFeatureDecisionsRespectPublicationAvailabilityOriginalScaleAndExpiry)
{
    draft.mutable_specification()->set_standardize(true);
    const auto model = publish_mapping_model();
    const auto command = mapping_command(model);
    const auto published = model.result_task().updated_at_ms();
    auto clock = std::make_shared<ManualClock>((published + 10) * 1000000);
    research::Models models(*store, owner, sqlite::Database::Access::writer, clock);
    const auto captured = models.capture_mapping(command.mapping(), model);
    const auto mapping = models.apply(command, nullptr, &captured).mapping();
    research::proto::ModelDecisionTrialInput input;
    *input.mutable_mapping() = mapping;
    *input.mutable_training() = model.result_task().model_training();
    input.set_model_attempt(model.result_task().attempt());
    auto& request = *input.mutable_request();
    *request.mutable_mapping() = protocol::model_mapping_object(mapping);
    request.set_fictional(true);
    request.set_purpose("Fictional new-feature clock contract, no market accuracy or orders");
    request.set_time_budget_seconds(10);
    const auto add = [&](const char* name,
                         std::int64_t boundary,
                         std::int64_t available,
                         std::optional<double> feature,
                         research::proto::ModelDecisionReason reason,
                         research::proto::ModelMappedRow::Intent intent) {
        auto& item = *request.add_cases();
        item.set_name(name);
        item.set_boundary_ns(boundary);
        item.set_feature_available_at_ns(available);
        if (feature)
            item.set_feature(*feature);
        item.set_expected_reason(reason);
        item.set_expected_intent(intent);
    };
    const auto mean = model.model_training().standardization().mean();
    ASSERT_GT(model.model_training().intercept(), 0.01);
    const auto ready = mapping.recorded_at_ms() * 1000000;
    add("before model",
        published * 1000000 - 1,
        1,
        mean,
        research::proto::MODEL_NOT_READY,
        research::proto::ModelMappedRow::PREDICTION_UNAVAILABLE);
    add("before mapping",
        published * 1000000,
        1,
        mean,
        research::proto::MODEL_MAPPING_NOT_READY,
        research::proto::ModelMappedRow::PREDICTION_UNAVAILABLE);
    add("exact available boundary",
        ready,
        ready,
        mean,
        research::proto::MODEL_DECISION_READY,
        research::proto::ModelMappedRow::LONG_DIRECTION);
    add("late feature",
        ready + 1,
        ready + 2,
        0.0,
        research::proto::MODEL_INPUT_NOT_READY,
        research::proto::ModelMappedRow::PREDICTION_UNAVAILABLE);
    add("missing",
        ready + 2,
        0,
        {},
        research::proto::MODEL_FEATURE_UNAVAILABLE,
        research::proto::ModelMappedRow::PREDICTION_UNAVAILABLE);
    add("expired",
        mapping.specification().valid_until_ms() * 1000000,
        1,
        mean,
        research::proto::MODEL_DECISION_EXPIRED,
        research::proto::ModelMappedRow::EXPIRED);
    clock->advance(10000000);
    const auto before = model.SerializeAsString();
    const auto result = research::compute_model_decision_trial(input, root, {}, {}, *clock);
    EXPECT_TRUE(result.all_match());
    EXPECT_EQ(result.matched(), 6U);
    EXPECT_DOUBLE_EQ(result.rows(2).prediction(), model.model_training().intercept());
    EXPECT_FALSE(result.rows(3).has_prediction());
    EXPECT_FALSE(result.rows(4).has_prediction());
    EXPECT_EQ(tasks::verified_result(*store, "mapping-model").SerializeAsString(), before);
    const auto json = protocol::decode_model_decision_request(request);
    EXPECT_EQ(
        protocol::encode_model_decision_request(json).SerializeAsString(),
        request.SerializeAsString());
    auto invalid = json;
    invalid["cases"][0]["feature"] = false;
    EXPECT_THROW(protocol::encode_model_decision_request(invalid), std::invalid_argument);
    auto wrong = input;
    wrong.mutable_request()->mutable_cases(2)->set_expected_intent(
        research::proto::ModelMappedRow::NO_NEW_DIRECTION);
    const auto mismatch = research::compute_model_decision_trial(wrong, root, {}, {}, *clock);
    EXPECT_FALSE(mismatch.all_match());
    EXPECT_EQ(mismatch.matched(), 5U);
    wrong = input;
    wrong.mutable_request()->mutable_cases(2)->set_feature(0.0);
    const auto zero = research::compute_model_decision_trial(wrong, root, {}, {}, *clock);
    EXPECT_TRUE(zero.rows(2).has_prediction());
    EXPECT_EQ(zero.rows(2).reason(), research::proto::MODEL_DECISION_READY);
    std::stop_source stop;
    stop.request_stop();
    EXPECT_THROW(
        research::compute_model_decision_trial(input, root, stop.get_token(), {}, *clock),
        Error);
    wrong = input;
    wrong.set_model_attempt(input.model_attempt() + 1);
    try {
        (void)research::compute_model_decision_trial(wrong, root, {}, {}, *clock);
        FAIL() << "A missing pinned model attempt must not produce any decisions";
    } catch (const std::invalid_argument& error) {
        EXPECT_STREQ(error.what(), "task payload must be a regular owned file");
    }
}

namespace {

struct ModelReplayFixture : ModelSourceFixture
{
    task::proto::TaskResponse time_result;

    std::vector<MarketBar> source_bars() const override
    {
        const std::array<const char*, 9> days{
            "2026-09-01",
            "2026-09-02",
            "2026-09-03",
            "2026-09-04",
            "2026-09-07",
            "2026-09-08",
            "2026-09-09",
            "2026-09-10",
            "2026-09-11"
        };
        std::vector<MarketBar> bars;
        for (int i = 0; i < 90; ++i) {
            const auto day = days[static_cast<std::size_t>(i / 10)];
            const auto at = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::sys_days(parse_trading_date(day)).time_since_epoch()
                                + std::chrono::hours(1))
                                .count()
                + (i % 10 + 1) * 60000000000LL;
            bars.push_back(test::flat(day, at, std::to_string(100 + i + i % 3).c_str()));
        }
        return bars;
    }

    backtest::proto::BacktestInput seed()
    {
        draft.mutable_specification()->set_standardize(true);
        const auto single = publish_mapping_model();
        const auto& recipe = single.result_task().model_training().plan();
        research::Models models(*store, owner, sqlite::Database::Access::writer);
        research::proto::ModelMutation save;
        save.set_id("save-replay-time");
        auto& request = *save.mutable_time_validation();
        request.set_plan_id("replay-time-plan");
        auto& spec = *request.mutable_specification();
        spec.set_name("Synthetic complete-trading-day replay definition");
        spec.set_purpose("Shared account execution and causal clock evidence");
        spec.set_limitations("Unknown historical availability, declared bar-close assumption only");
        *spec.mutable_recipe() = protocol::model_plan_object(recipe);
        auto& fold = *spec.add_folds();
        fold.set_training_begin(0);
        fold.set_training_end(12);
        fold.set_validation_begin(20);
        fold.set_validation_end(40);
        spec.set_run_budget(1);
        spec.set_time_budget_seconds(10);
        spec.set_valid_until_ms(recipe.specification().valid_until_ms() - 2000);
        const auto captured = models.capture_time(request, evidence);
        const auto plan = models.apply(save, nullptr, nullptr, &captured).time_plan();
        research::proto::ModelTimeInput training;
        *training.mutable_plan() = plan;
        tasks::submit(*store, store->submission("replay-time", training));
        task::proto::TaskFinish finished;
        finished.set_id("replay-time");
        finished.set_token(store->commit(store->claim("replay-time")).token());
        *finished.mutable_model_time() = research::validate_model_time(training, root);
        store->commit(tasks::finish(*store, finished));
        time_result = tasks::verified_result(*store, "replay-time");
        auto mapping_request = mapping_command(time_result);
        mapping_request.mutable_mapping()
            ->mutable_specification()
            ->mutable_short_threshold()
            ->set_units(-100);
        mapping_request.mutable_mapping()
            ->mutable_specification()
            ->mutable_long_threshold()
            ->set_units(100);
        const auto mapping = models.capture_mapping(mapping_request.mapping(), time_result);
        const auto fixed = models.apply(mapping_request, nullptr, &mapping).mapping();
        auto input =
            testing_support::backtest_input(evidence.result_task().factor().series(0).bars());
        *input.mutable_data_owner() = evidence.result_task().factor().data_owner();
        input.clear_strategies();
        input.mutable_paper()->mutable_contracts(0)->set_slippage_ticks(1);
        auto& definition = *input.mutable_model_replay();
        *definition.mutable_mapping() = fixed;
        *definition.mutable_training() = training;
        definition.set_model_attempt(time_result.result_task().attempt());
        definition.set_fold_index(0);
        definition.mutable_quantity()->set_units(Decimal::parse("1").raw());
        definition.set_sides(protocol::proto::LONG_AND_SHORT);
        definition.set_processing_delay_bars(10);
        definition.set_time_budget_seconds(10);
        definition.set_availability(
            research::proto::ModelReplayDefinition::ASSUME_ORIGINAL_BAR_CLOSE);
        return input;
    }
};

} // namespace

TEST_F(ModelReplayFixture, FixedBarsActuallyTrainThenFillOnLaterBarsWithOriginalCostsAndAccount)
{
    const auto input = seed();
    const auto source_before = time_result.SerializeAsString();
    const auto data = research::resolve_model_replay_data(input, root, program);
    ASSERT_EQ(data.training_members.size(), 8U);
    EXPECT_EQ(data.training_members.back(), 9U);
    ASSERT_EQ(data.features.size(), 40U);
    std::size_t completed = 0, total = 0;
    const auto result = backtest::run(
        input,
        {},
        [&](auto done, auto units) {
            completed = done;
            total = units;
        },
        nullptr,
        &data);
    EXPECT_EQ(completed, 41U);
    EXPECT_EQ(total, 41U);
    EXPECT_EQ(result.account().cursor(), 40U);
    EXPECT_EQ(result.account().total(), 40U);
    EXPECT_EQ(input.paper().contracts(0).dataset().bars_size(), 90);
    EXPECT_EQ(result.settlements_size(), 4);
    EXPECT_EQ(result.settlements(3).trading_day(), "2026-09-04");
    const auto& replay = result.model_replay();
    ASSERT_EQ(replay.decisions_size(), 40);
    EXPECT_EQ(replay.fit().started_index(), 12U);
    EXPECT_EQ(replay.fit().ready_index(), 22U);
    EXPECT_DOUBLE_EQ(replay.fit().intercept(), time_result.model_time().folds(0).intercept());
    EXPECT_DOUBLE_EQ(replay.fit().coefficient(), time_result.model_time().folds(0).coefficient());
    EXPECT_EQ(
        replay.fit().standardization().SerializeAsString(),
        time_result.model_time().folds(0).standardization().SerializeAsString());
    for (int i = 0; i < 22; ++i) {
        EXPECT_FALSE(replay.decisions(i).has_target());
        EXPECT_FALSE(replay.decisions(i).has_prediction());
    }
    EXPECT_EQ(replay.decisions(22).intent(), research::proto::ModelMappedRow::LONG_DIRECTION);
    EXPECT_DOUBLE_EQ(
        replay.decisions(22).prediction(),
        time_result.model_time().folds(0).predictions(2).prediction());
    ASSERT_GT(result.account().fills_size(), 0);
    ASSERT_EQ(replay.fills_size(), result.account().fills_size());
    for (int i = 0; i < replay.fills_size(); ++i) {
        const auto& fill = replay.fills(i);
        EXPECT_EQ(fill.fill_id(), result.account().fills(i).id());
        EXPECT_GE(fill.decision_source_index(), 22U);
        EXPECT_EQ(fill.source_index(), fill.decision_source_index() + 1);
        EXPECT_GT(
            fill.boundary_ns(),
            replay.decisions(static_cast<int>(fill.decision_source_index())).boundary_ns());
    }
    const auto fill_index = static_cast<int>(replay.fills(0).source_index());
    EXPECT_EQ(
        result.account().fills(0).price().units(),
        input.paper().contracts(0).dataset().bars(fill_index).open().units()
            + Decimal::parse("1").raw());
    EXPECT_EQ(result.account().fills(0).fee().units(), Decimal::parse("2").raw());
    EXPECT_GT(result.account().fees().units(), 0);
    EXPECT_EQ(result.account().margin().units(), Decimal::parse("100").raw());
    EXPECT_GT(result.account().positions_size(), 0);
    EXPECT_EQ(
        replay.decisions(39).disposition(),
        research::proto::ModelReplayDecision::OUTSIDE_REPLAY);
    EXPECT_TRUE(result.has_model_validation());
    EXPECT_NO_THROW(protocol::validate_backtest_result(input, result));
    const auto shown = protocol::decode_backtest_result(result);
    EXPECT_EQ(shown.at("model_replay").at("scope"), "post_hoc_conditional");
    EXPECT_TRUE(shown.at("model_replay").at("historical_availability_unknown"));
    EXPECT_EQ(tasks::verified_result(*store, "replay-time").SerializeAsString(), source_before);
    const auto again_data = research::resolve_model_replay_data(input, root, program);
    const auto again = backtest::run(input, {}, {}, nullptr, &again_data);
    EXPECT_EQ(again.account().SerializeAsString(), result.account().SerializeAsString());
    EXPECT_EQ(again.model_replay().fit().coefficient(), replay.fit().coefficient());
    EXPECT_EQ(
        again.model_replay().decisions(22).SerializeAsString(),
        replay.decisions(22).SerializeAsString());
}

TEST_F(ModelReplayFixture, ActualRiskRejectsOversizedTargetsWithoutFakeFillsOrFees)
{
    auto input = seed();
    input.mutable_model_replay()->mutable_quantity()->set_units(Decimal::parse("2").raw());
    input.mutable_paper()->mutable_risk()->mutable_max_gross_quantity()->set_units(
        Decimal::parse("1").raw());
    const auto data = research::resolve_model_replay_data(input, root, program);
    const auto result = backtest::run(input, {}, {}, nullptr, &data);
    ASSERT_GT(result.target_rejections_size(), 0);
    for (const auto& rejected : result.target_rejections()) {
        EXPECT_EQ(rejected.reason(), backtest::proto::BacktestTargetRejection::GROSS_QUANTITY);
        EXPECT_EQ(rejected.target().units(), Decimal::parse("2").raw());
        EXPECT_GE(rejected.source_index(), 23U);
    }
    EXPECT_EQ(
        protocol::decode_backtest_result(result)["target_rejections"][0]["reason"],
        "gross_quantity");
    EXPECT_TRUE(result.model_replay().decisions(22).has_target());
    EXPECT_EQ(result.model_replay().decisions(22).target().units(), Decimal::parse("2").raw());
    EXPECT_EQ(result.account().fills_size(), 0);
    EXPECT_EQ(result.account().positions_size(), 0);
    EXPECT_EQ(result.account().fees().units(), 0);
    EXPECT_EQ(result.account().equity().units(), input.paper().deposit().units());
    EXPECT_EQ(
        result.account().risk().SerializeAsString(),
        input.paper().risk().SerializeAsString());
    EXPECT_NO_THROW(protocol::validate_backtest_result(input, result));
}

TEST_F(ModelReplayFixture, FutureFeaturesCannotChangeEarlierFitsAndNoTargetNeverResubmitsAnOldOne)
{
    const auto input = seed();
    const auto original = research::resolve_model_replay_data(input, root, program);
    const auto before = backtest::run(input, {}, {}, nullptr, &original);
    // Isolated engine-boundary probe only: this altered projection is never
    // published as a task under the original immutable source references.
    auto changed = original;
    changed.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    changed.features[25].reset();
    changed.features[30] = 100.0;
    const auto after = backtest::run(input, {}, {}, nullptr, &changed);
    EXPECT_DOUBLE_EQ(
        before.model_replay().fit().coefficient(),
        after.model_replay().fit().coefficient());
    EXPECT_DOUBLE_EQ(
        before.model_replay().fit().intercept(),
        after.model_replay().fit().intercept());
    for (int i = 0; i < 25; ++i)
        EXPECT_EQ(
            before.model_replay().decisions(i).SerializeAsString(),
            after.model_replay().decisions(i).SerializeAsString());
    EXPECT_EQ(
        after.model_replay().decisions(25).phase(),
        research::proto::ModelReplayDecision::FEATURE_UNAVAILABLE);
    EXPECT_FALSE(after.model_replay().decisions(25).has_target());
    for (const auto& fill : after.model_replay().fills())
        EXPECT_NE(fill.decision_source_index(), 25U);
    auto wrong = input;
    wrong.mutable_paper()
        ->mutable_contracts(0)
        ->mutable_dataset()
        ->mutable_bars(5)
        ->mutable_close()
        ->set_units(Decimal::parse("999").raw());
    EXPECT_THROW(research::resolve_model_replay_data(wrong, root, program), Error);
    EXPECT_THROW(research::resolve_model_replay_data(input, root, std::string(64, 'b')), Error);
    EXPECT_THROW(backtest::run(input), std::invalid_argument);
    std::stop_source stop;
    stop.request_stop();
    EXPECT_THROW(backtest::run(input, stop.get_token(), {}, nullptr, &original), Error);
}

TEST_F(
    ModelReplayFixture,
    DurableBacktestRetainsExactModelDefinitionAndPublishedRiskEvidenceAcrossRestart)
{
    const auto input = seed();
    const auto definition = protocol::decode_model_replay_definition(input.model_replay());
    Json selection = {
        {"mapping", definition.at("mapping").at("object")},
        {"fold_index", 0},
        {"quantity", "1"},
        {"sides", "both"},
        {"processing_delay_bars", 10},
        {"time_budget_seconds", 10},
        {"availability", "assume_original_bar_close"}
    };
    const auto encoded = protocol::encode_model_replay_selection(selection);
    EXPECT_EQ(encoded.mapping().version(), input.model_replay().mapping().digest());
    EXPECT_EQ(encoded.processing_delay_bars(), 10U);
    auto ambiguous = selection;
    ambiguous["availability"] = "historically_known";
    EXPECT_THROW(protocol::encode_model_replay_selection(ambiguous), std::invalid_argument);
    ambiguous = selection;
    ambiguous["fold_index"] = 0.5;
    EXPECT_THROW(protocol::encode_model_replay_selection(ambiguous), std::invalid_argument);
    ambiguous = selection;
    ambiguous["quantity"] = "1.5";
    EXPECT_THROW(protocol::encode_model_replay_selection(ambiguous), std::invalid_argument);
    auto submission = store->submission("model-account", input);
    submission.set_program_artifact(program);
    const auto admitted = tasks::submit(*store, std::move(submission));
    EXPECT_EQ(admitted.kind(), task::proto::BACKTEST);
    const auto token = store->commit(store->claim("model-account")).token();
    auto read = store->prepare_input("model-account");
    read.load_for_claim();
    const auto attempt = store->confirm_attempt(std::move(read));
    ASSERT_TRUE(attempt.execution().has_backtest());
    ASSERT_TRUE(attempt.execution().has_model_replay());
    EXPECT_EQ(
        attempt.execution().model_replay().SerializeAsString(),
        input.model_replay().SerializeAsString());
    EXPECT_EQ(attempt.execution().input_sha256(), sha256_bytes(input.SerializeAsString()));
    EXPECT_NO_THROW((void)protocol::task_data_references(attempt.execution()));
    auto wrong = attempt.execution();
    wrong.set_program_artifact(std::string(64, 'b'));
    EXPECT_THROW((void)protocol::task_data_references(wrong), std::invalid_argument);
    wrong = attempt.execution();
    wrong.mutable_backtest()->add_strategies();
    EXPECT_THROW((void)protocol::task_data_references(wrong), std::invalid_argument);
    const auto context = research::resolve_model_replay_data(input, root, program);
    task::proto::TaskFinish finish;
    finish.set_id("model-account");
    finish.set_token(token);
    *finish.mutable_result() = backtest::run(input, {}, {}, nullptr, &context);
    store->commit(tasks::finish(*store, finish));
    const auto original = tasks::verified_result(*store, "model-account");
    ASSERT_TRUE(original.backtest().has_model_replay());
    store.reset();
    store = std::make_unique<tasks::Store>(root, tasks::Identity{"research-one"});
    const auto restored = tasks::verified_result(*store, "model-account");
    EXPECT_EQ(restored.SerializeAsString(), original.SerializeAsString());
    EXPECT_EQ(restored.result_task().input().SerializeAsString(), input.SerializeAsString());
    auto duplicate = store->submission("model-account", input);
    duplicate.set_program_artifact(program);
    const auto replayed = tasks::submit(*store, std::move(duplicate));
    EXPECT_EQ(replayed.state(), task::proto::SUCCEEDED);
    EXPECT_EQ(replayed.result_digest(), original.result_task().result_digest());
}

TEST_F(
    ModelReplayFixture,
    FixedModelCandidateUsesTheExistingPackageDeliveryAndExactBacktestDefinitionGate)
{
    const auto input = seed();
    auto source = store->submission("model-reference", input);
    source.set_program_artifact(program);
    tasks::submit(*store, std::move(source));
    const auto token = store->commit(store->claim("model-reference")).token();
    const auto context = research::resolve_model_replay_data(input, root, program);
    task::proto::TaskFinish finish;
    finish.set_id("model-reference");
    finish.set_token(token);
    *finish.mutable_result() = backtest::run(input, {}, {}, nullptr, &context);
    store->commit(tasks::finish(*store, finish));
    const auto reference = tasks::verified_result(*store, "model-reference");
    research::proto::StrategyMutation freeze;
    freeze.set_id("freeze-model-package");
    auto& request = *freeze.mutable_freeze();
    request.set_id("model-package");
    request.set_name("Fixed conditional model candidate");
    request.set_purpose("Exact offline account replay delivery");
    request.set_limitations("Historical availability unknown, reference only; no live authority");
    auto& object = *request.mutable_source();
    *object.mutable_owner() = owner;
    object.set_kind("backtest_result");
    object.set_id("model-reference");
    object.set_version(reference.result_task().result_digest());
    const auto captured = research::capture_strategy_package(
        owner,
        request,
        reference,
        sha256_bytes(input.SerializeAsString()));
    ASSERT_TRUE(captured.definition().has_model_replay());
    EXPECT_FALSE(captured.definition().has_strategy());
    EXPECT_EQ(
        captured.definition().model_replay().SerializeAsString(),
        input.model_replay().SerializeAsString());
    auto changed = captured.definition();
    changed.mutable_model_replay()->set_processing_delay_bars(11);
    EXPECT_NE(protocol::strategy_digest(changed), captured.digest());
    auto wrong_program = captured.definition();
    wrong_program.set_program_artifact(std::string(64, 'b'));
    EXPECT_THROW(protocol::validate_strategy_definition(wrong_program), std::invalid_argument);
    auto wrong_owner = captured;
    wrong_owner.mutable_owner()->set_service_id("other-research");
    EXPECT_THROW(protocol::validate_strategy_package(wrong_owner), std::invalid_argument);
    research::Strategies packages(*store, owner, sqlite::Database::Access::writer);
    const auto saved = packages.apply(freeze, &captured).package();
    EXPECT_EQ(packages.apply(freeze).package().SerializeAsString(), saved.SerializeAsString());
    EXPECT_TRUE(protocol::decode_strategy_package(saved).at("definition").contains("model_replay"));
    EXPECT_FALSE(protocol::decode_strategy_package(saved).at("definition").contains("strategy"));
    EXPECT_FALSE(packages.source_consumers("model-reference").empty());
    research::proto::StrategyMutation deliver;
    deliver.set_id("deliver-model-package");
    deliver.mutable_deliver()->set_id("model-delivery");
    *deliver.mutable_deliver()->mutable_package() = protocol::strategy_package_object(saved);
    deliver.mutable_deliver()->set_target_task_id("model-package-child");
    const auto delivery = packages.apply(deliver).delivery();
    EXPECT_EQ(packages.apply(deliver).delivery().SerializeAsString(), delivery.SerializeAsString());
    EXPECT_THROW(store->describe("model-package-child"), std::out_of_range);
    research::proto::StrategyUse use;
    *use.mutable_package() = protocol::strategy_package_object(saved);
    *use.mutable_delivery() = protocol::strategy_delivery_object(delivery);
    packages.verify_use(use, "model-package-child", true);
    auto child = store->submission("model-package-child", input);
    child.set_program_artifact(program);
    child.set_strategy_use(use, saved.definition());
    tasks::submit(*store, std::move(child));
    const auto child_token = store->commit(store->claim("model-package-child")).token();
    finish.set_id("model-package-child");
    finish.set_token(child_token);
    const auto child_context = research::resolve_model_replay_data(input, root, program);
    *finish.mutable_result() = backtest::run(input, {}, {}, nullptr, &child_context);
    store->commit(tasks::finish(*store, finish));
    const auto published = tasks::verified_result(*store, "model-package-child");
    EXPECT_EQ(published.result_task().strategy_use().SerializeAsString(), use.SerializeAsString());
    EXPECT_EQ(published.result_task().input().SerializeAsString(), input.SerializeAsString());
    auto altered = input;
    altered.mutable_model_replay()->set_processing_delay_bars(11);
    auto wrong = store->submission("different-model-policy", altered);
    wrong.set_program_artifact(program);
    EXPECT_THROW(wrong.set_strategy_use(use, saved.definition()), Error);
    research::proto::StrategyMutation retire;
    retire.set_id("retire-model-package");
    *retire.mutable_retire()->mutable_package() = protocol::strategy_package_object(saved);
    retire.mutable_retire()->set_reason("Synthetic candidate closed");
    EXPECT_EQ(packages.apply(retire).package().digest(), saved.digest());
    auto new_delivery = deliver;
    new_delivery.set_id("another-model-delivery");
    new_delivery.mutable_deliver()->set_id("another-model-delivery");
    new_delivery.mutable_deliver()->set_target_task_id("not-admitted");
    EXPECT_THROW(packages.apply(new_delivery), Error);
    EXPECT_NO_THROW(packages.verify_use(use, "model-package-child", false));
    EXPECT_EQ(
        tasks::verified_result(*store, "model-reference").SerializeAsString(),
        reference.SerializeAsString());
    EXPECT_EQ(
        tasks::verified_result(*store, "model-package-child").SerializeAsString(),
        published.SerializeAsString());
}
