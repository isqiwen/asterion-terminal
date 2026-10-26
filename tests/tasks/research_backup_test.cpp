#include "research_backup.hpp"
#include "research_submission.hpp"
#include "data_submission.hpp"
#include "research_statistics.hpp"
#include "research_evaluation.hpp"
#include "research_sample.hpp"
#include "project_store.hpp"
#include "variable_store.hpp"
#include "strategy_store.hpp"
#include "logic_store.hpp"
#include "research_logic.hpp"
#include <asterion/protocol/research_logic.hpp>
#include "risk_module.hpp"
#include "sqlite_database.hpp"
#include <asterion/protocol/research_strategy.hpp>
#include <asterion/protocol/research_variable.hpp>
#include "factor_engine.hpp"
#include <asterion/protocol/factor.hpp>
#include "program_files.hpp"
#include "domain_backup.hpp"
#include "data_backup.hpp"
#include "data_store.hpp"
#include "data_retentions.hpp"
#include "data/history_fixture.hpp"
#include "bar_dataset_source.hpp"
#include "research_data.hpp"
#include "task_store.hpp"
#include "engine.hpp"
#include "tasks/task_store_support.hpp"
#include "tasks/paper_input.hpp"
#include <asterion/kernel/process/child.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <asterion/protocol/task_execution.hpp>
#include <asterion/domain/trading_days.hpp>
#include <gtest/gtest.h>
#include <algorithm>
#include <array>

using namespace asterion;
namespace fs = std::filesystem;

namespace {

struct Folder
{
    fs::path path = fs::temp_directory_path() / ("asterion-research-backup-" + unique_process_id());

    Folder()
    {
        fs::create_directory(path);
        path = fs::canonical(path);
    }

    ~Folder()
    {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

resource::proto::Owner owner()
{
    resource::proto::Owner result;
    result.set_node_id("fixture-node");
    result.set_service_id("research-one");
    result.set_domain("research");
    return result;
}

backtest::proto::BacktestInput input()
{
    const auto bars = test::dataset(
        {test::flat("2026-09-25", 1790298000000000000LL, "100"),
            test::flat("2026-09-25", 1790298060000000000LL, "101"),
            test::flat("2026-09-25", 1790298120000000000LL, "102")});
    auto result = testing_support::backtest_input(bars);
    result.mutable_data_owner()->set_node_id("fixture-node");
    result.mutable_data_owner()->set_service_id("historical-data");
    return result;
}

void seed(const fs::path& root, bool abandoned, bool capture_program = false)
{
    fs::create_directory(root);
    tasks::Store store(asterion::research::task_domain(), root, {"research-one"});
    auto submission = task_submission(store, "fixed-run", input());
    if (capture_program) {
        const auto artifact = sha256_file(current_executable());
        (void)programs::Files(root / "programs", owner()).preserve(current_executable(), artifact);
        asterion::tasks::set_program_artifact(submission, artifact);
    }
    store.admit_submission(submission);
    submission.prepare_files();
    const auto evidence = store.prepared_submission_record("fixed-run");
    tasks::ResearchDataRoute route{evidence.execution().data_owner(), {}, "fixture-node"};
    const auto retained =
        tasks::research_retention(evidence.execution(), route, "research-one", "fixed-run");
    tasks::save_research_retention(root / "fixed-run", retained);
    if (!abandoned)
        store.commit(store.register_submission(submission));
}

} // namespace

TEST(ResearchColdBackup, RestoresFixedInputsWithoutOriginalRootAndReportsMissingDependencies)
{
    Folder folder;
    const auto root = folder.path / "source";
    seed(root, false);
    const auto before = sha256_file(root / "tasks.sqlite");
    auto exported = research::cold_backup(root, owner());
    ASSERT_EQ(exported.references.size(), 1U);
    EXPECT_FALSE(exported.references[0].released);
    ASSERT_EQ(exported.snapshot.dependencies.size(), 2U);
    for (const auto& dependency : exported.snapshot.dependencies)
        EXPECT_FALSE(dependency.included);
    EXPECT_FALSE(exported.snapshot.limitations.empty());
    EXPECT_THROW(
        tasks::Store writer(asterion::research::task_domain(), root, {"research-one"}),
        std::runtime_error);
    std::vector<backup_files::Snapshot> snapshots;
    snapshots.push_back(std::move(exported.snapshot));
    backup_files::Cut cut(std::move(snapshots));
    const auto plan = cut.preview("research-cold");
    const auto backup = folder.path / "backup";
    cut.save("research-cold", backup, sha256_bytes(plan.dump()));
    const auto restore = folder.path / "restore";
    backup_files::restore_files(backup, restore);
    fs::rename(root, folder.path / "preserved-original");
    auto restored = research::cold_backup(restore / "owners/0", owner());
    EXPECT_EQ(restored.references[0].retention_digest, exported.references[0].retention_digest);
    EXPECT_EQ(sha256_file(restore / "owners/0/tasks.sqlite"), before);
    EXPECT_EQ(sha256_file(folder.path / "preserved-original/tasks.sqlite"), before);
    auto wrong = owner();
    wrong.set_node_id("another-node");
    EXPECT_THROW(research::cold_backup(restore / "owners/0", wrong), Error);
}

TEST(
    ResearchColdBackup,
    CapturedCalculationProgramClosesItsOwnDependencyAndSurvivesIndependentRestore)
{
    Folder folder;
    const auto root = folder.path / "captured-source";
    seed(root, false, true);
    const auto artifact = sha256_file(current_executable());
    auto exported = research::cold_backup(root, owner());
    EXPECT_EQ(
        std::ranges::count(
            exported.snapshot.limitations,
            "original_execution_program_identity_not_recorded"),
        0);
    EXPECT_TRUE(std::ranges::any_of(exported.objects, [&](const auto& object) {
        return object.kind() == "research_program" && object.version() == artifact;
    }));
    EXPECT_TRUE(std::ranges::any_of(exported.snapshot.dependencies, [&](const auto& dependency) {
        return dependency.object.kind() == "research_program"
            && dependency.object.version() == artifact;
    }));
    std::vector<backup_files::Snapshot> snapshots;
    snapshots.push_back(std::move(exported.snapshot));
    backup_files::Cut cut(std::move(snapshots));
    const auto plan = cut.preview("captured-calculation");
    const auto archive = folder.path / "archive";
    cut.save("captured-calculation", archive, sha256_bytes(plan.dump()));
    const auto restore = folder.path / "independent";
    backup_files::restore_files(archive, restore);
    const auto restored_root = restore / "owners/0";
    {
        auto restored = research::cold_backup(restored_root, owner());
        EXPECT_EQ(
            std::ranges::count(
                restored.snapshot.limitations,
                "original_execution_program_identity_not_recorded"),
            0);
        EXPECT_EQ(sha256_file(restored_root / "programs" / artifact), artifact);
    }
    const auto before = sha256_file(restored_root / "tasks.sqlite");
    fs::rename(restored_root / "programs" / artifact, folder.path / "held-program");
    EXPECT_THROW(research::cold_backup(restored_root, owner()), std::exception);
    EXPECT_EQ(sha256_file(restored_root / "tasks.sqlite"), before);
    fs::rename(folder.path / "held-program", restored_root / "programs" / artifact);
    EXPECT_NO_THROW(research::cold_backup(restored_root, owner()));
}

TEST(ResearchColdBackup, PreservesAbandonmentProofWithAndWithoutRemainingPayload)
{
    Folder folder;
    const auto root = folder.path / "source";
    seed(root, true);
    {
        tasks::Store store(asterion::research::task_domain(), root, {"research-one"});
        const auto evidence = store.prepared_submission_record("fixed-run");
        tasks::ResearchDataRoute route{evidence.execution().data_owner(), {}, "fixture-node"};
        const auto retained =
            tasks::research_retention(evidence.execution(), route, "research-one", "fixed-run");
        store.commit(store.abandon_preparation(evidence, retained.consumer()));
    }
    for (const bool removed : {false, true}) {
        if (removed)
            fs::remove_all(root / "fixed-run");
        auto exported = research::cold_backup(root, owner());
        ASSERT_EQ(exported.references.size(), 1U);
        EXPECT_TRUE(exported.references[0].released);
        EXPECT_TRUE(exported.snapshot.dependencies.empty());
        EXPECT_EQ(
            std::ranges::any_of(
                exported.snapshot.files,
                [](const auto& file) {
                    return file.relative == "fixed-run/input.pb";
                }),
            !removed);
        EXPECT_EQ(exported.objects[0].id(), "fixed-run");
    }
    replace_file_durably(root / "unclassified.private", "unclassified fixture");
    EXPECT_THROW(research::cold_backup(root, owner()), std::invalid_argument);
}

TEST(ResearchColdBackup, JointCutChecksBilateralReferencesAndRestoresBothDomains)
{
    Folder folder;
    const auto data_root = folder.path / "data";
    const auto research_root = folder.path / "research";
    fs::create_directory(data_root);
    fs::create_directory(research_root);
    const auto selected = test::seed_history(data_root, {100, 101, 102}, "joint-cut");
    auto fixed = input();
    data::proto::DataOwner source = fixed.data_owner();
    {
        data::Store data(data_root, "historical-data");
        const auto query = protocol::encode_bar_dataset_request(
            {{"source_dataset_ids", selected.at("source_dataset_ids")},
                {"daily", selected.at("daily")},
                {"begin_day", ""},
                {"end_day", ""},
                {"contract", "SHFE/rb/2026-10"}});
        *fixed.mutable_paper()->mutable_contracts(0)->mutable_dataset() =
            data::resolve_bar_dataset(data.tables(), data.sources(query));
        fixed.set_dataset_revision(protocol::dataset_revision(fixed.paper()));
        data::Retentions references(data_root, source, data);
        tasks::Store research(asterion::research::task_domain(), research_root, {"research-one"});
        auto submission = task_submission(research, "joint-run", fixed);
        research.admit_submission(submission);
        submission.prepare_files();
        const auto evidence = research.prepared_submission_record("joint-run");
        tasks::ResearchDataRoute route{source, {}, "fixture-node"};
        const auto retained =
            tasks::research_retention(evidence.execution(), route, "research-one", "joint-run");
        tasks::save_research_retention(research_root / "joint-run", retained);
        references.prepare(retained);
        research.commit(research.register_submission(submission));
    }
    resource::proto::Owner data_owner;
    data_owner.set_node_id(source.node_id());
    data_owner.set_service_id(source.service_id());
    data_owner.set_domain("data");
    {
        auto partial = terminal_backup::cold_cut({{owner(), research_root}});
        const auto plan = partial.preview("partial-cut");
        EXPECT_TRUE(std::ranges::any_of(plan.at("dependencies"), [](const auto& value) {
            return !value.at("included").template get<bool>();
        }));
    }
    {
        auto joint = terminal_backup::cold_cut({{owner(), research_root}, {data_owner, data_root}});
        const auto plan = joint.preview("joint-cut");
        for (const auto& dependency : plan.at("dependencies"))
            EXPECT_TRUE(dependency.at("included"));
        EXPECT_FALSE(plan.at("limitations").empty());
        joint.save("joint-cut", folder.path / "backup", sha256_bytes(plan.dump()));
    }
    {
        auto checked = terminal_backup::cold_cut(
            {{data_owner, folder.path / "backup/owners/0"},
                {owner(), folder.path / "backup/owners/1"}});
        (void)checked.preview("checked-backup");
    }
    // Domain inspection must leave the published backup itself immutable.
    EXPECT_NO_THROW(backup_files::inspect(folder.path / "backup"));
    backup_files::restore_files(folder.path / "backup", folder.path / "restore");
    fs::rename(data_root, folder.path / "preserved-data");
    fs::rename(research_root, folder.path / "preserved-research");
    {
        auto verified = terminal_backup::cold_cut(
            {{data_owner, folder.path / "restore/owners/0"},
                {owner(), folder.path / "restore/owners/1"}});
        const auto plan = verified.preview("restored-cut");
        for (const auto& dependency : plan.at("dependencies"))
            EXPECT_TRUE(dependency.at("included"));
    }
    // Both owners selected, but one authority lost its counterpart: never report
    // this as a complete or merely partial successful backup.
    resource::proto::Object consumer;
    {
        auto evidence = research::cold_backup(folder.path / "restore/owners/1", owner());
        consumer = evidence.references[0].consumer;
    }
    {
        data::Store data(folder.path / "restore/owners/0", "historical-data");
        data::Retentions references(folder.path / "restore/owners/0", source, data);
        auto unmatched = references.get(consumer).retention();
        unmatched.mutable_consumer()->set_id("missing-consumer");
        references.prepare(unmatched);
    }
    EXPECT_THROW(
        terminal_backup::cold_cut(
            {{data_owner, folder.path / "restore/owners/0"},
                {owner(), folder.path / "restore/owners/1"}}),
        Error);
}

TEST(ResearchColdBackup, RetainedFactorRowsRestoreWithActualProgramAndExactDataDependencies)
{
    for (const int mode : {0, 1, 2}) {
        const bool rolling = mode == 1;
        const bool holdout = mode == 2;
        Folder folder;
        const auto data_root = folder.path / "data";
        const auto research_root = folder.path / "research";
        fs::create_directory(data_root);
        fs::create_directory(research_root);
        std::vector<int> prices(rolling ? 160 : holdout ? 90 : 40);
        for (int i = 0; i < static_cast<int>(prices.size()); ++i)
            prices[static_cast<std::size_t>(i)] = i < 3 ? 100 : 100 + i + i % 3;
        const auto selected = test::seed_history(data_root, prices, "retained-factor");
        data::proto::DataOwner source;
        source.set_node_id("fixture-node");
        source.set_service_id("historical-data");
        factor::proto::FactorResult expected;
        research::proto::StatisticsResult expected_statistics;
        research::proto::HypothesisPlan expected_plan;
        research::proto::ProjectConclusion expected_conclusion;
        research::proto::VariableDefinition expected_variable;
        const auto artifact = sha256_file(current_executable());
        {
            data::Store data(data_root, "historical-data");
            const auto query = protocol::encode_bar_dataset_request(
                {{"source_dataset_ids", selected.at("source_dataset_ids")},
                    {"daily", selected.at("daily")},
                    {"begin_day", ""},
                    {"end_day", ""},
                    {"contract", "SHFE/rb/2026-10"}});
            factor::proto::FactorInput fixed;
            *fixed.mutable_data_owner() = source;
            *fixed.add_series()->mutable_bars() =
                data::resolve_bar_dataset(data.tables(), data.sources(query));
            fixed.set_dataset_revision(protocol::factor_revision(fixed.series()));
            fixed.set_factor(protocol::proto::PRICE_MOMENTUM);
            fixed.add_lookbacks(2);
            fixed.set_horizon(holdout ? 3 : 2);
            if (rolling) {
                fixed.add_lookbacks(5);
                fixed.add_lookbacks(10);
                fixed.mutable_walk_forward()->set_training_events(80);
                fixed.mutable_walk_forward()->set_validation_events(40);
            } else if (holdout)
                fixed.set_holdout_start(45);
            else
                fixed.set_full_sample(true);
            fixed.set_retain_observations(true);

            data::Retentions references(data_root, source, data);
            tasks::Store research(
                asterion::research::task_domain(),
                research_root,
                {"research-one"});
            asterion::research::Projects projects(
                research,
                owner(),
                sqlite::Database::Access::writer);
            research::proto::ProjectMutation draft;
            draft.set_id("factor-plan-draft");
            draft.mutable_save()->set_project_id("factor-question");
            auto& definition = *draft.mutable_save()->mutable_definition();
            definition.set_title("Synthetic fixed factor question");
            definition.set_question("What does this original factor evidence show?");
            definition.set_hypothesis("An unproven declared relation");
            definition.set_support("Declared evidence criterion");
            definition.set_refutation("Opposite direction or insufficient evidence");
            const auto saved = projects.apply(draft).draft();
            research::proto::ProjectMutation fix;
            fix.set_id("factor-plan-fix");
            fix.mutable_fix()->set_project_id(saved.id());
            fix.mutable_fix()->set_plan_id("factor-plan");
            fix.mutable_fix()->set_expected_draft_digest(saved.digest());
            expected_plan = projects.apply(fix).plan();
            research::proto::ProjectBinding binding;
            *binding.mutable_owner() = owner();
            binding.set_project_id(saved.id());
            binding.set_plan_id(expected_plan.id());
            binding.set_definition_digest(expected_plan.definition_digest());
            (void)programs::Files(research_root / "programs", owner())
                .preserve(current_executable(), artifact);
            if (!rolling) {
                asterion::research::Variables variables(
                    research,
                    owner(),
                    sqlite::Database::Access::writer);
                research::proto::VariableMutation registration;
                registration.set_id("cold-variable-command");
                auto& definition = *registration.mutable_registration();
                definition.set_id("cold-variable");
                definition.set_name("Original expression factor");
                definition.set_purpose(
                    "Synthetic completed-close predictor, distinct from the forward label");
                definition.set_expression("close / lag(close,window) - 1");
                definition.set_window(2);
                expected_variable = variables.apply(registration).definition();
                fixed.set_factor(protocol::proto::FACTOR_UNSPECIFIED);
                *fixed.mutable_variable_definition() = expected_variable;
            }
            expected = factor::run(fixed);
            auto submission = task_submission(research, "factor-rows", fixed);
            asterion::tasks::set_program_artifact(submission, artifact);
            asterion::tasks::set_research_plan(research, submission, binding);
            research.admit_submission(submission);
            submission.prepare_files();
            tasks::ResearchDataRoute route{source, {}, "fixture-node"};
            const auto retained = tasks::research_retention(
                submission.execution(),
                route,
                "research-one",
                "factor-rows");
            tasks::save_research_retention(research_root / "factor-rows", retained);
            references.prepare(retained);
            research.commit(research.register_submission(submission));
            references.confirm(retained);
            const auto token = research.commit(research.claim("factor-rows")).token();
            research.commit(tasks::finish(research, "factor-rows", token, expected));
            research::proto::ProjectMutation conclude;
            conclude.set_id("cold-human-conclusion-command");
            auto& declaration = *conclude.mutable_conclude();
            declaration.set_id("cold-human-conclusion");
            *declaration.mutable_plan() = binding;
            *declaration.mutable_result()->mutable_owner() = owner();
            declaration.mutable_result()->set_kind("factor_result");
            declaration.mutable_result()->set_id("factor-rows");
            declaration.mutable_result()->set_version(
                research.describe("factor-rows").result_digest());
            declaration.set_judgment(research::proto::INSUFFICIENT);
            declaration.set_reason("Human review of isolated synthetic evidence; no market claim");
            expected_conclusion = projects.apply(conclude).conclusion();
            if (!rolling) {
                const auto source_result = tasks::verified_result(research, "factor-rows");
                research::proto::StatisticsRequest selected_statistics;
                *selected_statistics.mutable_source() = declaration.result();
                selected_statistics.set_use_scope("Synthetic original feature distribution backup");
                selected_statistics.set_column(research::proto::StatisticsRequest::FEATURE);
                selected_statistics.set_role(
                    holdout ? research::proto::StatisticsRequest::DEVELOPMENT
                            : research::proto::StatisticsRequest::FULL_SAMPLE);
                selected_statistics.set_end_index(static_cast<unsigned>(prices.size()));
                selected_statistics.set_bins(8);
                const auto statistics_input = tasks::capture_statistics_input(
                    owner(),
                    selected_statistics,
                    source_result,
                    sha256_file(research_root / "factor-rows/input.pb"),
                    fs::file_size(research_root / "factor-rows/input.pb"));
                auto statistics = task_submission(research, "statistics-rows", statistics_input);
                asterion::tasks::set_program_artifact(statistics, artifact);
                tasks::submit(research, std::move(statistics));
                const auto statistics_token =
                    research.commit(research.claim("statistics-rows")).token();
                expected_statistics = tasks::compute_statistics(statistics_input, research_root);
                task::proto::TaskFinish completed;
                completed.set_id("statistics-rows");
                completed.set_token(statistics_token);
                *completed.mutable_statistics() = expected_statistics;
                research.commit(tasks::finish(research, std::move(completed)));
            }
        }
        resource::proto::Owner data_owner;
        data_owner.set_node_id(source.node_id());
        data_owner.set_service_id(source.service_id());
        data_owner.set_domain("data");
        {
            auto joint =
                terminal_backup::cold_cut({{owner(), research_root}, {data_owner, data_root}});
            const auto plan = joint.preview("factor-observations");
            for (const auto& dependency : plan.at("dependencies"))
                EXPECT_TRUE(dependency.at("included"));
            joint.save("factor-observations", folder.path / "archive", sha256_bytes(plan.dump()));
        }
        backup_files::restore_files(folder.path / "archive", folder.path / "restore");
        fs::rename(data_root, folder.path / "held-original-data");
        fs::rename(research_root, folder.path / "held-original-research");
        {
            auto checked = terminal_backup::cold_cut(
                {{data_owner, folder.path / "restore/owners/0"},
                    {owner(), folder.path / "restore/owners/1"}});
            const auto plan = checked.preview("verified-factor-observations");
            for (const auto& dependency : plan.at("dependencies"))
                EXPECT_TRUE(dependency.at("included"));
        }
        {
            tasks::Store restored(
                asterion::research::task_domain(),
                folder.path / "restore/owners/1",
                {"research-one"});
            asterion::research::Projects projects(restored, owner());
            EXPECT_EQ(
                projects.plan(expected_plan.id()).SerializeAsString(),
                expected_plan.SerializeAsString());
            EXPECT_EQ(
                projects.conclusion(expected_conclusion.declaration().id()).SerializeAsString(),
                expected_conclusion.SerializeAsString());
            EXPECT_EQ(projects.conclusions(expected_plan.project_id(), "", 10).size(), 1U);
            EXPECT_FALSE(expected_conclusion.retrospective());
            if (!rolling) {
                asterion::research::Variables variables(restored, owner());
                EXPECT_EQ(
                    variables.definition(expected_variable.id()).SerializeAsString(),
                    expected_variable.SerializeAsString());
                EXPECT_EQ(
                    restored.describe("factor-rows").variable_definition().SerializeAsString(),
                    protocol::variable_object(expected_variable).SerializeAsString());
                EXPECT_EQ(
                    restored.get("factor-rows").factor().variable_definition().SerializeAsString(),
                    expected_variable.SerializeAsString());
            }
            EXPECT_EQ(
                restored.describe("factor-rows").research_plan().definition_digest(),
                expected_plan.definition_digest());
            EXPECT_EQ(restored.describe("factor-rows").program_artifact(), artifact);
            EXPECT_TRUE(restored.get("factor-rows").factor().retain_observations());
            const auto result = tasks::factor_result(restored, "factor-rows");
            EXPECT_EQ(result.SerializeAsString(), expected.SerializeAsString());
            if (!rolling) {
                const auto report = tasks::verified_result(restored, "statistics-rows");
                ASSERT_TRUE(report.has_statistics());
                EXPECT_EQ(
                    report.statistics().SerializeAsString(),
                    expected_statistics.SerializeAsString());
                EXPECT_EQ(report.result_task().program_artifact(), artifact);
                EXPECT_EQ(
                    report.statistics().source().version(),
                    restored.describe("factor-rows").result_digest());
                EXPECT_EQ(report.statistics().summary().valid(), holdout ? 43U : 38U);
                EXPECT_EQ(
                    restored.retirement("factor-rows", owner().node_id()).consumers_size(),
                    1);
            }
            if (rolling) {
                EXPECT_TRUE(result.observations().empty());
                ASSERT_EQ(result.folds_size(), 2);
                EXPECT_EQ(result.folds(1).observations(0).event_index(), 40U);
                EXPECT_TRUE(result.folds(0).observations(78).label_crosses_boundary());
                EXPECT_FALSE(result.folds(0).observations(78).included());
                EXPECT_FALSE(result.folds(0).observations(119).has_forward_return());
            } else {
                EXPECT_EQ(result.observations(2).value(), 0.0);
                if (holdout) {
                    EXPECT_EQ(result.partitions(0).sample_count(), 40U);
                    EXPECT_EQ(result.partitions(1).sample_count(), 42U);
                    EXPECT_EQ(result.purged_count(), 3U);
                    EXPECT_TRUE(result.observations(42).label_crosses_boundary());
                    EXPECT_FALSE(result.observations(42).included());
                    EXPECT_EQ(
                        result.observations(45).role(),
                        factor::proto::FactorObservation::HOLDOUT);
                    EXPECT_FALSE(result.observations(89).has_forward_return());
                } else
                    EXPECT_FALSE(result.observations(39).has_forward_return());
            }
        }
        if (!rolling) {
            const auto broken = folder.path / "missing-variable-copy";
            fs::copy(folder.path / "restore/owners/1", broken, fs::copy_options::recursive);
            {
                sqlite::Database database(broken / "tasks.sqlite");
                sqlite::Database::Statement remove(
                    database,
                    "DELETE FROM research_variable_definitions WHERE id=?");
                remove.bind(1, expected_variable.id()).step();
            }
            EXPECT_THROW(
                terminal_backup::cold_cut(
                    {{data_owner, folder.path / "restore/owners/0"}, {owner(), broken}}),
                Error);
        }
        {
            sqlite::Database corrupted(folder.path / "restore/owners/1/tasks.sqlite");
            sqlite::Database::Statement remove(
                corrupted,
                "DELETE FROM research_hypothesis_plans WHERE id=?");
            remove.bind(1, expected_plan.id())
                .step(); // Synthetic corruption, never a product deletion path.
        }
        EXPECT_THROW(
            terminal_backup::cold_cut(
                {{data_owner, folder.path / "restore/owners/0"},
                    {owner(), folder.path / "restore/owners/1"}}),
            Error);
    }
}

TEST(ResearchColdBackup, SharedInputsProduceIndependentBackupAndRestoreWithoutOriginalFiles)
{
    Folder folder;
    const auto root = folder.path / "source";
    seed(root, false);
    {
        tasks::Store store(asterion::research::task_domain(), root, {"research-one"});
        const auto token = store.commit(store.claim("fixed-run")).token();
        store.commit(tasks::finish(store, "fixed-run", token, backtest::run(input())));
        auto child = task_submission(store, "linked-run", input());
        task::proto::TaskDerivation origin;
        origin.set_source_id("fixed-run");
        origin.set_source_result_digest(store.describe("fixed-run").result_digest());
        origin.set_data_choice("fixed");
        child.set_derivation(origin);
        store.admit_submission(child);
        child.prepare_files();
        const auto prepared = store.prepared_submission_record("linked-run");
        tasks::ResearchDataRoute route{prepared.execution().data_owner(), {}, "fixture-node"};
        tasks::save_research_retention(
            root / "linked-run",
            tasks::research_retention(prepared.execution(), route, "research-one", "linked-run"));
        store.commit(store.register_submission(child));
    }
    EXPECT_TRUE(fs::equivalent(root / "fixed-run/input.pb", root / "linked-run/input.pb"));
    const auto digest = sha256_file(root / "fixed-run/input.pb");
    const auto backup = folder.path / "backup";
    {
        auto cut = terminal_backup::cold_cut({{owner(), root}});
        const auto plan = cut.preview("shared-input-backup");
        cut.save("shared-input-backup", backup, sha256_bytes(plan.dump()));
    }
    EXPECT_FALSE(
        fs::equivalent(root / "fixed-run/input.pb", backup / "owners/0/fixed-run/input.pb"));
    EXPECT_EQ(fs::hard_link_count(backup / "owners/0/fixed-run/input.pb"), 1U);
    backup_files::restore_files(backup, folder.path / "restored");
    fs::rename(root, folder.path / "original-preserved");
    const auto restored = folder.path / "restored/owners/0";
    EXPECT_FALSE(fs::equivalent(restored / "fixed-run/input.pb", restored / "linked-run/input.pb"));
    EXPECT_EQ(sha256_file(restored / "fixed-run/input.pb"), digest);
    EXPECT_EQ(sha256_file(restored / "linked-run/input.pb"), digest);
    auto verified = research::cold_backup(restored, owner());
    EXPECT_EQ(verified.references.size(), 2U);
    EXPECT_EQ(backup_files::inspect(backup).at("owners").size(), 1U);
}

TEST(ResearchColdBackup, IndependentRawSampleAndStatisticsCloseDataReferencesAndRestoreOriginalRows)
{
    Folder folder;
    const auto data_root = folder.path / "data";
    const auto research_root = folder.path / "research";
    fs::create_directory(data_root);
    fs::create_directory(research_root);
    const auto selection = test::seed_history(data_root, {100, -1, 0, 2, -2, 4}, "raw-sample-cold");
    data::proto::DataOwner source;
    source.set_node_id("fixture-node");
    source.set_service_id("historical-data");
    resource::proto::Owner data_owner;
    data_owner.set_node_id(source.node_id());
    data_owner.set_service_id(source.service_id());
    data_owner.set_domain("data");
    research::proto::SampleResult expected_sample;
    research::proto::StatisticsResult expected_statistics;
    research::proto::HypothesisPlan expected_plan;
    research::proto::ProjectConclusion expected_conclusion;
    const auto artifact = sha256_file(current_executable());
    {
        data::Store data(data_root, "historical-data");
        const auto query = protocol::encode_bar_dataset_request(
            {{"source_dataset_ids", selection.at("source_dataset_ids")},
                {"daily", selection.at("daily")},
                {"begin_day", ""},
                {"end_day", ""},
                {"contract", "SHFE/rb/2026-10"}});
        research::proto::SampleInput input;
        *input.mutable_owner() = owner();
        *input.mutable_data_owner() = source;
        auto& request = *input.mutable_request();
        *request.mutable_selection() = query;
        request.set_mode(research::proto::SampleRequest::POST_HOC);
        request.set_use_scope("Synthetic independent raw-close backup");
        request.set_development_end(4);
        *input.mutable_dataset() = data::resolve_bar_dataset(data.tables(), data.sources(query));
        tasks::Store research(asterion::research::task_domain(), research_root, {"research-one"});
        (void)programs::Files(research_root / "programs", owner())
            .preserve(current_executable(), artifact);
        research::Projects projects(research, owner(), sqlite::Database::Access::writer);
        research::proto::ProjectMutation draft;
        draft.set_id("raw-question-save");
        draft.mutable_save()->set_project_id("raw-question");
        draft.mutable_save()->mutable_definition()->set_title("Synthetic raw distribution");
        draft.mutable_save()->mutable_definition()->set_question(
            "What does the observed raw-price distribution show?");
        draft.mutable_save()->mutable_definition()->set_hypothesis(
            "Exploratory description only; prediction remains untested");
        const auto saved = projects.apply(draft).draft();
        research::proto::ProjectMutation fix;
        fix.set_id("raw-question-fix");
        fix.mutable_fix()->set_project_id(saved.id());
        fix.mutable_fix()->set_plan_id("raw-plan");
        fix.mutable_fix()->set_expected_draft_digest(saved.digest());
        expected_plan = projects.apply(fix).plan();
        research::proto::ProjectBinding binding;
        *binding.mutable_owner() = owner();
        binding.set_project_id(saved.id());
        binding.set_plan_id(expected_plan.id());
        binding.set_definition_digest(expected_plan.definition_digest());
        auto submission = task_submission(research, "raw-sample", input);
        asterion::tasks::set_research_plan(research, submission, binding);
        asterion::tasks::set_program_artifact(submission, artifact);
        research.admit_submission(submission);
        submission.prepare_files();
        tasks::ResearchDataRoute route{source, {}, "fixture-node"};
        const auto retained =
            tasks::research_retention(submission.execution(), route, "research-one", "raw-sample");
        tasks::save_research_retention(research_root / "raw-sample", retained);
        data::Retentions references(data_root, source, data);
        references.prepare(retained);
        research.commit(research.register_submission(submission));
        references.confirm(retained);
        const auto token = research.commit(research.claim("raw-sample")).token();
        expected_sample = tasks::capture_sample(input);
        task::proto::TaskFinish sampled;
        sampled.set_id("raw-sample");
        sampled.set_token(token);
        *sampled.mutable_sample() = expected_sample;
        research.commit(tasks::finish(research, std::move(sampled)));
        const auto original = tasks::verified_result(research, "raw-sample");
        research::proto::StatisticsRequest statistics;
        *statistics.mutable_source()->mutable_owner() = owner();
        statistics.mutable_source()->set_kind("sample_result");
        statistics.mutable_source()->set_id("raw-sample");
        statistics.mutable_source()->set_version(original.result_task().result_digest());
        statistics.set_use_scope("Synthetic raw-close distribution after independent sample");
        statistics.set_column(research::proto::StatisticsRequest::RAW_CLOSE);
        statistics.set_role(research::proto::StatisticsRequest::DEVELOPMENT);
        statistics.set_end_index(6);
        statistics.set_bins(3);
        statistics.set_grouping(research::proto::StatisticsRequest::TRADING_DAY);
        const auto captured = tasks::capture_statistics_input(
            owner(),
            statistics,
            original,
            sha256_file(research_root / "raw-sample/input.pb"),
            fs::file_size(research_root / "raw-sample/input.pb"));
        auto analysis = task_submission(research, "raw-statistics", captured);
        asterion::tasks::set_program_artifact(analysis, artifact);
        asterion::tasks::set_research_plan(research, analysis, binding);
        tasks::submit(research, std::move(analysis));
        const auto analysis_token = research.commit(research.claim("raw-statistics")).token();
        expected_statistics = tasks::compute_statistics(captured, research_root);
        task::proto::TaskFinish analysed;
        analysed.set_id("raw-statistics");
        analysed.set_token(analysis_token);
        *analysed.mutable_statistics() = expected_statistics;
        research.commit(tasks::finish(research, std::move(analysed)));
        research::proto::ProjectMutation conclude;
        conclude.set_id("raw-statistics-human-write");
        auto& declaration = *conclude.mutable_conclude();
        declaration.set_id("raw-statistics-human");
        *declaration.mutable_plan() = binding;
        *declaration.mutable_result()->mutable_owner() = owner();
        declaration.mutable_result()->set_kind("statistics_result");
        declaration.mutable_result()->set_id("raw-statistics");
        declaration.mutable_result()->set_version(
            research.describe("raw-statistics").result_digest());
        declaration.set_judgment(research::proto::INSUFFICIENT);
        declaration.set_reason("Human description does not establish predictive validity");
        expected_conclusion = projects.apply(conclude).conclusion();
    }
    {
        auto cut = terminal_backup::cold_cut({{data_owner, data_root}, {owner(), research_root}});
        const auto plan = cut.preview("independent-sample-statistics");
        for (const auto& dependency : plan.at("dependencies"))
            EXPECT_TRUE(dependency.at("included"));
        cut.save(
            "independent-sample-statistics",
            folder.path / "archive",
            sha256_bytes(plan.dump()));
    }
    backup_files::restore_files(folder.path / "archive", folder.path / "restore");
    fs::rename(data_root, folder.path / "held-data");
    fs::rename(research_root, folder.path / "held-research");
    const auto restored_data = folder.path / "restore/owners/0";
    const auto restored_research = folder.path / "restore/owners/1";
    {
        auto checked =
            terminal_backup::cold_cut({{data_owner, restored_data}, {owner(), restored_research}});
        const auto restored_plan = checked.preview("restored-raw-sample");
        for (const auto& dependency : restored_plan.at("dependencies"))
            EXPECT_TRUE(dependency.at("included"));
    }
    {
        tasks::Store restored(
            asterion::research::task_domain(),
            restored_research,
            {"research-one"});
        EXPECT_EQ(
            tasks::verified_result(restored, "raw-sample").sample().SerializeAsString(),
            expected_sample.SerializeAsString());
        const auto report = tasks::verified_result(restored, "raw-statistics");
        EXPECT_EQ(report.statistics().SerializeAsString(), expected_statistics.SerializeAsString());
        EXPECT_DOUBLE_EQ(report.statistics().summary().mean().value(), 25.25);
        EXPECT_DOUBLE_EQ(report.statistics().rows(2).value(), 0);
        EXPECT_FALSE(report.statistics().rows(4).has_value());
        ASSERT_EQ(report.statistics().groups_size(), 1);
        EXPECT_EQ(report.statistics().groups(0).trading_day(), "2026-09-25");
        EXPECT_DOUBLE_EQ(report.statistics().groups(0).summary().mean().value(), 25.25);
        EXPECT_EQ(restored.retirement("raw-sample", owner().node_id()).consumers_size(), 1);
        research::Projects projects(restored, owner());
        EXPECT_EQ(projects.plan("raw-plan").SerializeAsString(), expected_plan.SerializeAsString());
        EXPECT_EQ(
            projects.conclusion("raw-statistics-human").SerializeAsString(),
            expected_conclusion.SerializeAsString());
        EXPECT_EQ(report.result_task().research_plan().plan_id(), "raw-plan");
    }
    fs::rename(restored_research / "raw-sample/input.pb", folder.path / "held-sample-input.pb");
    EXPECT_THROW(
        terminal_backup::cold_cut({{data_owner, restored_data}, {owner(), restored_research}}),
        std::exception);
    fs::rename(folder.path / "held-sample-input.pb", restored_research / "raw-sample/input.pb");
}

TEST(ResearchColdBackup, StrategyDraftsAndAdmittedUseCloseOriginalEvidenceAcrossIndependentRestore)
{
    Folder folder;
    const auto data_root = folder.path / "strategy-data";
    const auto root = folder.path / "strategy-research";
    fs::create_directory(data_root);
    fs::create_directory(root);
    const auto selection =
        test::seed_history(data_root, {100, 101, 102, 99, 98, 101, 104}, "strategy-cold");
    data::proto::DataOwner source;
    source.set_node_id("fixture-node");
    source.set_service_id("historical-data");
    resource::proto::Owner data_owner;
    data_owner.set_node_id(source.node_id());
    data_owner.set_service_id(source.service_id());
    data_owner.set_domain("data");
    research::proto::StrategyPackage expected;
    research::proto::StrategyDelivery used, pending;
    research::proto::StrategyUse use;
    task::proto::TaskResponse expected_result;
    research::proto::HypothesisPlan expected_plan;
    research::proto::ProjectConclusion expected_conclusion;
    {
        data::Store data(data_root, source.service_id());
        const auto query = protocol::encode_bar_dataset_request(
            {{"source_dataset_ids", selection.at("source_dataset_ids")},
                {"daily", selection.at("daily")},
                {"begin_day", ""},
                {"end_day", ""},
                {"contract", "SHFE/rb/2026-10"}});
        const auto dataset = data::resolve_bar_dataset(data.tables(), data.sources(query));
        auto fixed = testing_support::backtest_input(dataset);
        *fixed.mutable_data_owner() = source;
        tasks::Store store(asterion::research::task_domain(), root, {"research-one"});
        const auto artifact = sha256_file(current_executable());
        (void)programs::Files(root / "programs", owner()).preserve(current_executable(), artifact);
        data::Retentions references(data_root, source, data);
        const auto submit = [&](tasks::Store::Submission submission, const std::string& id) {
            store.admit_submission(submission);
            submission.prepare_files();
            const auto prepared = store.prepared_submission_record(id);
            const tasks::ResearchDataRoute route{source, {}, owner().node_id()};
            const auto retained =
                tasks::research_retention(prepared.execution(), route, owner().service_id(), id);
            tasks::save_research_retention(root / id, retained);
            references.prepare(retained);
            store.commit(store.register_submission(submission));
            references.confirm(retained);
            const auto token = store.commit(store.claim(id)).token();
            store.commit(tasks::finish(store, id, token, backtest::run(fixed)));
        };
        auto reference = task_submission(store, "reference", fixed);
        asterion::tasks::set_program_artifact(reference, artifact);
        submit(std::move(reference), "reference");
        const auto original = tasks::verified_result(store, "reference");
        research::Strategies strategies(store, owner(), sqlite::Database::Access::writer);
        research::proto::StrategyMutation freeze;
        freeze.set_id("freeze");
        auto& definition = *freeze.mutable_freeze();
        definition.set_id("candidate");
        definition.set_name("Synthetic cold candidate");
        definition.set_purpose("Independent formal evidence restore");
        definition.set_limitations("No predictive or trading authorization");
        auto& origin = *definition.mutable_source();
        *origin.mutable_owner() = owner();
        origin.set_kind("backtest_result");
        origin.set_id("reference");
        origin.set_version(original.result_task().result_digest());
        const auto captured = research::capture_strategy_package(
            owner(),
            definition,
            original,
            sha256_file(root / "reference/input.pb"));
        expected = strategies.apply(freeze, &captured).package();
        const auto draft = [&](const std::string& id, const std::string& target) {
            research::proto::StrategyMutation request;
            request.set_id(id + "-request");
            request.mutable_deliver()->set_id(id);
            request.mutable_deliver()->set_target_task_id(target);
            *request.mutable_deliver()->mutable_package() =
                protocol::strategy_package_object(expected);
            return strategies.apply(request).delivery();
        };
        used = draft("used-delivery", "delivered");
        pending = draft("pending-delivery", "never-submitted");
        *use.mutable_package() = used.package();
        *use.mutable_delivery() = protocol::strategy_delivery_object(used);
        research::Projects projects(store, owner(), sqlite::Database::Access::writer);
        research::proto::ProjectMutation question;
        question.set_id("backtest-question-save");
        question.mutable_save()->set_project_id("backtest-question");
        auto& hypothesis = *question.mutable_save()->mutable_definition();
        hypothesis.set_title("Original synthetic backtest question");
        hypothesis.set_question("Does the fixed simulation meet the declared criteria?");
        hypothesis.set_hypothesis("Unproven simulated strategy behavior");
        hypothesis.set_support("Independent evidence is required");
        hypothesis.set_refutation("Opposite behavior or insufficient evidence");
        const auto saved_question = projects.apply(question).draft();
        research::proto::ProjectMutation fix_plan;
        fix_plan.set_id("backtest-plan-fix");
        fix_plan.mutable_fix()->set_project_id(saved_question.id());
        fix_plan.mutable_fix()->set_plan_id("backtest-plan");
        fix_plan.mutable_fix()->set_expected_draft_digest(saved_question.digest());
        expected_plan = projects.apply(fix_plan).plan();
        research::proto::ProjectBinding binding;
        *binding.mutable_owner() = owner();
        binding.set_project_id(expected_plan.project_id());
        binding.set_plan_id(expected_plan.id());
        binding.set_definition_digest(expected_plan.definition_digest());
        auto calculation = task_submission(store, "delivered", fixed);
        asterion::tasks::set_research_plan(store, calculation, binding);
        asterion::tasks::set_program_artifact(calculation, artifact);
        asterion::tasks::set_strategy_use(store, calculation, use, expected.definition());
        task::proto::TaskDerivation derivation;
        derivation.set_source_id("reference");
        derivation.set_source_result_digest(origin.version());
        derivation.set_data_choice("fixed");
        calculation.set_derivation(derivation);
        submit(std::move(calculation), "delivered");
        expected_result = tasks::verified_result(store, "delivered");
        research::proto::ProjectMutation review;
        review.set_id("backtest-human-review-command");
        auto& declaration = *review.mutable_conclude();
        declaration.set_id("backtest-human-review");
        *declaration.mutable_plan() = binding;
        *declaration.mutable_result()->mutable_owner() = owner();
        declaration.mutable_result()->set_kind("backtest_result");
        declaration.mutable_result()->set_id("delivered");
        declaration.mutable_result()->set_version(expected_result.result_task().result_digest());
        declaration.set_judgment(research::proto::INSUFFICIENT);
        declaration.set_reason("Independent human judgment: synthetic ledger is not market proof");
        expected_conclusion = projects.apply(review).conclusion();
        EXPECT_FALSE(expected_conclusion.retrospective());
        research::proto::StrategyMutation deprecate;
        deprecate.set_id("deprecate");
        *deprecate.mutable_retire()->mutable_package() = used.package();
        deprecate.mutable_retire()->set_reason("No new admission; preserve formal evidence");
        expected = strategies.apply(deprecate).package();
    }
    {
        auto cut = terminal_backup::cold_cut({{data_owner, data_root}, {owner(), root}});
        const auto preview = cut.preview("strategy-cut");
        for (const auto& dependency : preview.at("dependencies"))
            EXPECT_TRUE(dependency.at("included"));
        cut.save("strategy-cut", folder.path / "archive", sha256_bytes(preview.dump()));
    }
    backup_files::restore_files(folder.path / "archive", folder.path / "restored");
    fs::rename(data_root, folder.path / "held-data");
    fs::rename(root, folder.path / "held-research");
    const auto restored_data = folder.path / "restored/owners/0";
    const auto restored = folder.path / "restored/owners/1";
    {
        auto cut = terminal_backup::cold_cut({{data_owner, restored_data}, {owner(), restored}});
        const auto restored_preview = cut.preview("restored-strategies");
        for (const auto& dependency : restored_preview.at("dependencies"))
            EXPECT_TRUE(dependency.at("included"));
    }
    {
        tasks::Store store(asterion::research::task_domain(), restored, {"research-one"});
        research::Strategies strategies(store, owner());
        research::Projects projects(store, owner());
        EXPECT_EQ(
            projects.plan(expected_plan.id()).SerializeAsString(),
            expected_plan.SerializeAsString());
        EXPECT_EQ(
            projects.conclusion(expected_conclusion.declaration().id()).SerializeAsString(),
            expected_conclusion.SerializeAsString());
        EXPECT_EQ(store.describe("delivered").research_plan().plan_id(), expected_plan.id());
        EXPECT_EQ(
            strategies.package(expected.id()).SerializeAsString(),
            expected.SerializeAsString());
        EXPECT_EQ(
            strategies.delivery(pending.id()).SerializeAsString(),
            pending.SerializeAsString());
        EXPECT_FALSE(strategies.delivery_status(pending.id()).target_accepted());
        EXPECT_TRUE(strategies.delivery_status(used.id()).target_accepted());
        EXPECT_EQ(
            tasks::verified_result(store, "delivered").SerializeAsString(),
            expected_result.SerializeAsString());
    }
    {
        auto checked = research::cold_backup(restored, owner());
        EXPECT_EQ(
            std::ranges::count(checked.objects, "strategy_package", &resource::proto::Object::kind),
            1);
        EXPECT_EQ(
            std::ranges::count(
                checked.objects,
                "strategy_delivery",
                &resource::proto::Object::kind),
            2);
    }
    const auto module = risk_providers::Module::filename(restored / "reference");
    fs::rename(module, folder.path / "held-module");
    EXPECT_THROW(research::cold_backup(restored, owner()), std::exception);
    fs::rename(folder.path / "held-module", module);
    {
        sqlite::Database database(restored / "tasks.sqlite", sqlite::Database::Access::writer);
        sqlite::Database::Statement erase(
            database,
            "DELETE FROM research_strategy_deliveries WHERE id=?");
        erase.bind(1, used.id()).step();
    }
    EXPECT_THROW(research::cold_backup(restored, owner()), std::exception);
}

TEST(ResearchBackup, IndependentLogicExpectationsAndActualTrialRestoreWithoutData)
{
    Folder folder;
    const auto root = folder.path / "research";
    ASSERT_TRUE(fs::create_directory(root));
    task::proto::TaskResponse original;
    research::proto::LogicTrialInput fixed;
    research::proto::StrategyPackage expected_package;
    {
        tasks::Store store(asterion::research::task_domain(), root, {"research-one"});
        research::Logic logic(store, owner(), sqlite::Database::Access::writer);
        research::proto::LogicMutation registration;
        registration.set_id("register-rule");
        auto& rule = *registration.mutable_registration();
        rule.set_id("means");
        rule.set_name("Fictional means");
        rule.set_purpose("No return claim");
        rule.set_fast(1);
        rule.set_slow(2);
        rule.set_input(research::proto::LogicDefinition::COMPLETED_CLOSE);
        rule.set_output(research::proto::LogicDefinition::TARGET_SIGN);
        const auto definition = logic.apply(registration).definition();
        research::proto::LogicMutation save;
        save.set_id("save-expectations");
        auto& example = *save.mutable_example();
        example.set_id("prior-expectations");
        example.set_name("Prior fictional rising observations");
        example.set_use_scope("Synthetic pure logic only");
        example.set_fictional(true);
        *example.mutable_definition() = protocol::logic_object(definition);
        for (int i = 0; i < 3; ++i) {
            auto& row = *example.add_observations();
            row.set_closed_at_ns((i + 1) * 60000000000LL);
            row.set_available_at_ns(row.closed_at_ns());
            row.mutable_close()->set_units(Decimal::parse(std::to_string(i + 10)).raw());
            if (i == 0)
                row.set_warmup(true);
            else
                row.set_target_sign(1);
        }
        fixed = logic.input(protocol::logic_object(logic.apply(save).example()), true);
        const auto artifact = sha256_file(current_executable());
        (void)programs::Files(root / "programs", owner()).preserve(current_executable(), artifact);
        auto submission = task_submission(store, "logic-run", fixed);
        asterion::tasks::set_program_artifact(submission, artifact);
        tasks::submit(store, std::move(submission));
        const auto token = store.commit(store.claim("logic-run")).token();
        task::proto::TaskFinish finish;
        finish.set_id("logic-run");
        finish.set_token(token);
        *finish.mutable_logic_trial() = tasks::compute_logic_trial(fixed);
        store.commit(tasks::finish(store, std::move(finish)));
        original = tasks::verified_result(store, "logic-run");
        const auto module = risk_providers::Module::selected();
        (void)programs::Files(root / "programs", owner())
            .preserve(module.path(), module.artifact());
        research::proto::StrategyMutation assemble;
        assemble.set_id("assemble-logic");
        auto& request = *assemble.mutable_logic();
        request.set_id("logic-package");
        request.set_name("Checked fictional means");
        request.set_purpose("Pending explicit research backtest");
        request.set_limitations("No market performance or live authorization");
        auto& reference = *request.mutable_source();
        *reference.mutable_owner() = owner();
        reference.set_kind("logic_trial_result");
        reference.set_id("logic-run");
        reference.set_version(original.result_task().result_digest());
        *request.mutable_contract() =
            test::dataset({test::flat("2026-09-25", 1790298000000000000LL, "100")}).contract();
        request.set_interval_minutes(1);
        *request.mutable_strategy() = protocol::encode_strategy(
            {{"quantity", "1"},
                {"sides", "both"},
                {"rule", {{"kind", "moving_average"}, {"fast", 1}, {"slow", 2}}}});
        const auto captured = research::capture_logic_strategy_package(
            owner(),
            request,
            original,
            sha256_file(root / "logic-run/input.pb"),
            module.artifact());
        research::Strategies strategies(store, owner(), sqlite::Database::Access::writer);
        expected_package = strategies.apply(assemble, &captured).package();
        research::proto::StrategyMutation deliver;
        deliver.set_id("save-pending-draft");
        deliver.mutable_deliver()->set_id("pending-draft");
        deliver.mutable_deliver()->set_target_task_id("not-yet-admitted");
        *deliver.mutable_deliver()->mutable_package() =
            protocol::strategy_package_object(expected_package);
        strategies.apply(deliver);
    }
    {
        auto cut = terminal_backup::cold_cut({{owner(), root}});
        const auto preview = cut.preview("logic-cut");
        for (const auto& dependency : preview.at("dependencies"))
            EXPECT_TRUE(dependency.at("included"));
        cut.save("logic-cut", folder.path / "archive", sha256_bytes(preview.dump()));
    }
    backup_files::restore_files(folder.path / "archive", folder.path / "restored");
    fs::rename(root, folder.path / "held-original");
    const auto restored = folder.path / "restored/owners/0";
    {
        auto cut = terminal_backup::cold_cut({{owner(), restored}});
        const auto preview = cut.preview("check");
        for (const auto& dependency : preview.at("dependencies"))
            EXPECT_TRUE(dependency.at("included"));
    }
    {
        tasks::Store store(asterion::research::task_domain(), restored, {"research-one"});
        EXPECT_EQ(
            tasks::verified_result(store, "logic-run").SerializeAsString(),
            original.SerializeAsString());
        research::Logic logic(store, owner());
        logic.verify_input(fixed, false);
        research::Strategies strategies(store, owner());
        EXPECT_EQ(
            strategies.package(expected_package.id()).SerializeAsString(),
            expected_package.SerializeAsString());
        EXPECT_FALSE(strategies.delivery_status("pending-draft").target_accepted());
    }
    const auto risk = restored / "programs" / expected_package.definition().risk_artifact();
    fs::rename(risk, folder.path / "held-risk");
    EXPECT_THROW(research::cold_backup(restored, owner()), std::invalid_argument);
    fs::rename(folder.path / "held-risk", risk);
    fs::rename(
        restored / "programs" / original.result_task().program_artifact(),
        folder.path / "held-program");
    EXPECT_THROW(research::cold_backup(restored, owner()), std::invalid_argument);
}

TEST(ResearchColdBackup, RollingWindowsRestoreOriginalTradingDays)
{
    Folder folder;
    const auto data_root = folder.path / "rolling-data";
    const auto root = folder.path / "rolling-research";
    fs::create_directory(data_root);
    fs::create_directory(root);
    std::vector<int> prices;
    std::vector<std::string> days;
    auto at = std::chrono::sys_days{std::chrono::year{2026} / std::chrono::September / 1};
    for (int i = 0; prices.size() < 33; at += std::chrono::days{1}) {
        const auto weekday = std::chrono::weekday{at};
        if (weekday == std::chrono::Saturday || weekday == std::chrono::Sunday)
            continue;
        prices.push_back(100 + 2 * i - (i % 3 == 2 ? 3 : 0));
        days.push_back(format_trading_date(std::chrono::year_month_day{at}));
        ++i;
    }
    const auto selection = test::seed_history(data_root, prices, "rolling-cold", days);
    data::proto::DataOwner source;
    source.set_node_id("fixture-node");
    source.set_service_id("historical-data");
    auto data_owner = owner();
    data_owner.set_domain("data");
    data_owner.set_service_id(source.service_id());
    task::proto::TaskResponse original, original_evaluation;
    research::proto::ProjectConclusion evaluation_conclusion;
    research::proto::EvaluationInput evaluation_input;
    {
        data::Store data(data_root, source.service_id());
        const auto query = protocol::encode_bar_dataset_request(
            {{"source_dataset_ids", selection.at("source_dataset_ids")},
                {"daily", selection.at("daily")},
                {"begin_day", ""},
                {"end_day", ""},
                {"contract", "SHFE/rb/2026-10"}});
        auto fixed = testing_support::backtest_input(
            data::resolve_bar_dataset(data.tables(), data.sources(query)));
        *fixed.mutable_data_owner() = source;
        *fixed.mutable_strategies(0) = protocol::encode_strategy(
            {{"quantity", "1"},
                {"sides", "both"},
                {"rule", {{"kind", "moving_average"}, {"fast", 1}, {"slow", 2}}}});
        *fixed.add_strategies() = protocol::encode_strategy(
            {{"quantity", "1"},
                {"sides", "both"},
                {"rule", {{"kind", "moving_average"}, {"fast", 1}, {"slow", 3}}}});
        // This synthetic comparison explicitly chooses its scoring method.
        fixed.mutable_performance_assumptions()->set_annual_periods(252);
        fixed.mutable_performance_assumptions()->set_risk_free(
            backtest::proto::PerformanceAssumptions::EXPLICIT_ZERO);
        fixed.mutable_walk_forward()->set_training_days(20);
        fixed.mutable_walk_forward()->set_validation_days(5);
        tasks::Store store(asterion::research::task_domain(), root, {"research-one"});
        const auto artifact = sha256_file(current_executable());
        (void)programs::Files(root / "programs", owner()).preserve(current_executable(), artifact);
        auto submission = task_submission(store, "rolling-run", fixed);
        asterion::tasks::set_program_artifact(submission, artifact);
        store.admit_submission(submission);
        submission.prepare_files();
        tasks::ResearchDataRoute route{source, {}, owner().node_id()};
        const auto retained = tasks::research_retention(
            submission.execution(),
            route,
            owner().service_id(),
            "rolling-run");
        tasks::save_research_retention(root / "rolling-run", retained);
        data::Retentions references(data_root, source, data);
        references.prepare(retained);
        store.commit(store.register_submission(submission));
        references.confirm(retained);
        const auto token = store.commit(store.claim("rolling-run")).token();
        const auto result = backtest::run(fixed);
        ASSERT_EQ(result.folds_size(), 3);
        EXPECT_EQ(result.folds(2).training_first_day(), days[10]);
        EXPECT_EQ(result.folds(2).training_last_day(), days[29]);
        EXPECT_EQ(result.folds(2).first_day(), days[30]);
        EXPECT_EQ(result.folds(2).last_day(), days[32]);
        EXPECT_EQ(result.folds(2).validation_days(), 3U);
        store.commit(tasks::finish(store, "rolling-run", token, result));
        original = tasks::verified_result(store, "rolling-run");
        research::proto::EvaluationRequest evaluate;
        *evaluate.mutable_source()->mutable_owner() = owner();
        evaluate.mutable_source()->set_kind("backtest_result");
        evaluate.mutable_source()->set_id("rolling-run");
        evaluate.mutable_source()->set_version(original.result_task().result_digest());
        evaluate.set_question(
            "Fixed original rolling validation account; no independent conclusion");
        evaluate.set_scope(research::proto::EvaluationRequest::OUT_OF_SAMPLE);
        evaluate.set_include_daily_drawdown(true);
        evaluate.set_include_exposure(true);
        auto& scenario = *evaluate.mutable_static_stress();
        scenario.set_purpose("Synthetic fixed rolling account funding stress");
        scenario.mutable_price()->set_units(Decimal::parse("80").raw());
        scenario.mutable_margin_rate()->set_units(Decimal::parse("0.4").raw());
        scenario.set_funds_model(
            research::proto::EvaluationStressScenario::PAPER_BALANCE_EQUITY_MINIMUM);
        evaluate.mutable_method()->set_annual_periods(250);
        evaluate.mutable_method()->set_risk_free(
            backtest::proto::PerformanceAssumptions::EXPLICIT_ZERO);
        auto source_read = store.prepare_result("rolling-run");
        const auto source_digest = source_read.input_digest();
        const auto source_bytes = source_read.input_bytes();
        source_read.verify();
        evaluation_input = tasks::capture_evaluation_input(
            owner(),
            evaluate,
            store.confirm_result(std::move(source_read)),
            source_digest,
            source_bytes);
        auto report = task_submission(store, "rolling-evaluation", evaluation_input);
        asterion::tasks::set_program_artifact(report, artifact);
        tasks::submit(store, std::move(report));
        task::proto::TaskFinish finish;
        finish.set_id("rolling-evaluation");
        finish.set_token(store.commit(store.claim(finish.id())).token());
        *finish.mutable_evaluation() = tasks::compute_evaluation(evaluation_input, root);
        EXPECT_EQ(finish.evaluation().trading_days(), 13U);
        ASSERT_TRUE(finish.evaluation().has_daily_drawdown());
        ASSERT_TRUE(finish.evaluation().has_static_stress());
        EXPECT_EQ(finish.evaluation().daily_drawdown().points_size(), 13);
        ASSERT_TRUE(finish.evaluation().has_exposure());
        EXPECT_EQ(finish.evaluation().exposure().source_index(), 32U);
        store.commit(tasks::finish(store, finish));
        (void)store.commit(store.retire(store.retirement(finish.id(), owner().node_id()).object()));
        original_evaluation = tasks::verified_result(store, finish.id());
        research::Projects projects(store, owner(), sqlite::Database::Access::writer);
        research::proto::ProjectMutation save;
        save.set_id("evaluation-interpretation-save");
        auto* draft = save.mutable_save();
        draft->set_project_id("evaluation-interpretation");
        auto* definition = draft->mutable_definition();
        definition->set_title("Synthetic rolling account interpretation");
        definition->set_question("What can this fixed performance establish?");
        definition->set_hypothesis("No automatic investment verdict");
        definition->set_support("Independent evidence would be required");
        definition->set_refutation("Synthetic account results alone are insufficient");
        const auto saved_draft = projects.apply(save).draft();
        research::proto::ProjectMutation fix;
        fix.set_id("evaluation-interpretation-fix");
        fix.mutable_fix()->set_project_id(saved_draft.id());
        fix.mutable_fix()->set_plan_id("evaluation-interpretation-plan");
        fix.mutable_fix()->set_expected_draft_digest(saved_draft.digest());
        const auto plan = projects.apply(fix).plan();
        research::proto::ProjectMutation conclude;
        conclude.set_id("evaluation-interpretation-human");
        auto* human = conclude.mutable_conclude();
        human->set_id("evaluation-interpretation-human");
        auto* binding = human->mutable_plan();
        *binding->mutable_owner() = owner();
        binding->set_project_id(plan.project_id());
        binding->set_plan_id(plan.id());
        binding->set_definition_digest(plan.definition_digest());
        *human->mutable_result()->mutable_owner() = owner();
        human->mutable_result()->set_kind("performance_evaluation_result");
        human->mutable_result()->set_id(finish.id());
        human->mutable_result()->set_version(original_evaluation.result_task().result_digest());
        human->set_judgment(research::proto::INSUFFICIENT);
        human->set_reason("Synthetic posthoc review, not independent market evidence");
        evaluation_conclusion = projects.apply(conclude).conclusion();
        EXPECT_TRUE(evaluation_conclusion.retrospective());

        auto pending = task_submission(store, "rolling-evaluation-prepared", evaluation_input);
        asterion::tasks::set_program_artifact(pending, artifact);
        store.admit_submission(pending);
        pending.prepare_files();
        EXPECT_TRUE(store.abandon_submission(pending));
    }
    {
        auto cut = terminal_backup::cold_cut({{data_owner, data_root}, {owner(), root}});
        const auto preview = cut.preview("rolling-cut");
        for (const auto& dependency : preview.at("dependencies"))
            EXPECT_TRUE(dependency.at("included"));
        cut.save("rolling-cut", folder.path / "archive", sha256_bytes(preview.dump()));
    }
    backup_files::restore_files(folder.path / "archive", folder.path / "restored");
    fs::rename(data_root, folder.path / "held-data");
    fs::rename(root, folder.path / "held-research");
    const auto restored_root = folder.path / "restored/owners/1";
    {
        auto checked = terminal_backup::cold_cut(
            {{data_owner, folder.path / "restored/owners/0"}, {owner(), restored_root}});
        (void)checked.preview("restored-rolling");
    }
    tasks::Store restored(asterion::research::task_domain(), restored_root, {"research-one"});
    {
        research::Projects projects(restored, owner());
        EXPECT_EQ(
            projects.conclusion(evaluation_conclusion.declaration().id()).SerializeAsString(),
            evaluation_conclusion.SerializeAsString());
    }
    EXPECT_EQ(
        tasks::verified_result(restored, "rolling-run").SerializeAsString(),
        original.SerializeAsString());
    EXPECT_EQ(
        tasks::verified_result(restored, "rolling-evaluation").SerializeAsString(),
        original_evaluation.SerializeAsString());
    EXPECT_EQ(
        restored.prepared_submission_record("rolling-evaluation-prepared")
            .execution()
            .evaluation()
            .SerializeAsString(),
        evaluation_input.SerializeAsString());
    auto resumed = restored.resume_submission("rolling-evaluation-prepared");
    restored.admit_submission(resumed);
    restored.commit(restored.register_submission(resumed));
    task::proto::TaskFinish resumed_finish;
    resumed_finish.set_id("rolling-evaluation-prepared");
    resumed_finish.set_token(restored.commit(restored.claim(resumed_finish.id())).token());
    *resumed_finish.mutable_evaluation() =
        tasks::compute_evaluation(evaluation_input, restored_root);
    restored.commit(tasks::finish(restored, resumed_finish));
    EXPECT_EQ(
        tasks::verified_result(restored, resumed_finish.id()).evaluation().SerializeAsString(),
        original_evaluation.evaluation().SerializeAsString());
}
