#include "research_evaluation.hpp"
#include "research_submission.hpp"
#include "data_submission.hpp"
#include "support/data_discovery.hpp"
#include "support/task_wait.hpp"
#include <asterion/kernel/ipc/local_channel.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <asterion/research.pb.h>
#include "engine.hpp"
#include "task_store.hpp"
#include "tasks/task_store_support.hpp"
#include "tasks/paper_input.hpp"
#include "program_files.hpp"
#include <asterion/protocol/research_evaluation.hpp>
#include <asterion/protocol/research_project.hpp>
#include <asterion/protocol/backtest.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/child.hpp>
#include <asterion/domain/trading_days.hpp>
#include <asterion/foundation/error.hpp>
#include <gtest/gtest.h>
#include <map>
#include <filesystem>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
using namespace asterion;
namespace fs = std::filesystem;
namespace r = asterion::research::proto;

namespace {

struct EvaluationEvidence : testing::Test
{
    fs::path root =
        fs::temp_directory_path() / ("asterion-evaluation-fixture-" + unique_process_id());
    resource::proto::Owner owner;
    std::unique_ptr<tasks::Store> store;
    backtest::proto::BacktestInput original;
    task::proto::TaskResponse evidence;
    r::EvaluationRequest request;
    std::string artifact, input_digest;
    std::uint64_t input_bytes = 0;
    fs::path runtime, captured_log;

    void SetUp() override
    {
        ASSERT_TRUE(fs::create_directory(root));
        root = fs::canonical(root);
        owner.set_node_id("fixture-node");
        owner.set_service_id("research-one");
        owner.set_domain("research");
        store = std::make_unique<tasks::Store>(root, tasks::Identity{"research-one"});
        artifact = sha256_file(current_executable());
        (void)programs::Files(root / "programs", owner).preserve(current_executable(), artifact);
    }

    void TearDown() override
    {
        store.reset();
        if (HasFailure() && !runtime.empty()) {
            std::ifstream diagnostics(captured_log);
            if (diagnostics)
                std::cerr << "Isolated Research startup output:\n" << diagnostics.rdbuf();
        }
        std::error_code error;
        fs::remove_all(root, error);
        if (!runtime.empty())
            fs::remove_all(runtime, error);
    }

    void publish(
        bool comparison = false,
        bool record_funds = true,
        bool daily = false,
        bool portfolio = false)
    {
        using namespace std::chrono;
        std::vector<MarketBar> bars;
        auto day = sys_days{year{2026} / August / 17};
        while (bars.size() < 30) {
            const weekday wd{day};
            if (wd != Saturday && wd != Sunday) {
                const auto i = static_cast<int>(bars.size());
                bars.push_back(
                    test::flat(
                        format_trading_date(year_month_day{day}),
                        duration_cast<nanoseconds>((day + hours{9}).time_since_epoch()).count(),
                        std::to_string(100 + 2 * i - (i % 3 == 2 ? 3 : 0)).c_str()));
            }
            day += days{1};
        }
        auto dataset = test::dataset(bars);
        if (daily) {
            dataset.set_interval_minutes(0);
            dataset.clear_source_dataset_ids();
            dataset.clear_history_evidence();
            dataset.set_source(dataset.daily().daily_source());
            dataset.set_revision(protocol::bar_dataset_revision(dataset));
        }
        original = testing_support::backtest_input(dataset);
        if (portfolio) {
            auto* second = original.mutable_paper()->add_contracts();
            *second = original.paper().contracts(0);
            *second->mutable_dataset()->mutable_contract() =
                test::contract("SHFE", "hc2610", "hc", "2026-10");
            second->mutable_dataset()->set_revision(
                protocol::bar_dataset_revision(second->dataset()));
            original.set_dataset_revision(protocol::dataset_revision(original.paper()));
        }
        if (comparison) {
            *original.add_strategies() = testing_support::moving_average(1, 2);
            original.set_holdout_day(bars[25].trading_day);
            original.mutable_performance_assumptions()->set_annual_periods(252);
            original.mutable_performance_assumptions()->set_risk_free(
                backtest::proto::PerformanceAssumptions::EXPLICIT_ZERO);
        }
        auto submission = task_submission(*store, "source-account", original);
        submission.set_program_artifact(artifact);
        tasks::submit(*store, std::move(submission));
        const auto token = store->commit(store->claim("source-account")).token();
        auto computed = backtest::run(original);
        // This synthetic producer explicitly omits optional observations before
        // its first publication; no existing formal evidence is rewritten.
        if (!record_funds)
            for (auto& day : *computed.mutable_settlements()) {
                day.clear_occupancy();
                for (auto& contract : *day.mutable_contracts()) {
                    contract.clear_gross_quantity();
                    contract.clear_margin();
                }
            }
        store->commit(tasks::finish(*store, "source-account", token, computed));
        auto read = store->prepare_result("source-account");
        read.verify();
        input_digest = read.input_digest();
        input_bytes = read.input_bytes();
        evidence = store->confirm_result(std::move(read));
        *request.mutable_source()->mutable_owner() = owner;
        request.mutable_source()->set_kind("backtest_result");
        request.mutable_source()->set_id("source-account");
        request.mutable_source()->set_version(evidence.result_task().result_digest());
        request.set_question("Fixed synthetic account performance, without a trading conclusion");
        request.set_scope(r::EvaluationRequest::FULL_ACCOUNT);
    }

    void stress()
    {
        request.set_include_exposure(true);
        auto& scenario = *request.mutable_static_stress();
        scenario.set_purpose("Explicit synthetic fixed-holdings funding stress");
        scenario.mutable_price()->set_units(Decimal::parse("80").raw());
        scenario.mutable_margin_rate()->set_units(Decimal::parse("0.4").raw());
        scenario.set_funds_model(r::EvaluationStressScenario::PAPER_BALANCE_EQUITY_MINIMUM);
    }

    r::EvaluationInput capture()
    {
        return tasks::capture_evaluation_input(owner, request, evidence, input_digest, input_bytes);
    }
};

} // namespace

TEST_F(EvaluationEvidence, ExplicitNewMethodDoesNotBackfillOriginalAssumptionsOrRewriteAccountFacts)
{
    publish();
    const auto before = evidence.SerializeAsString();
    const auto unknown = capture();
    const auto result = tasks::compute_evaluation(unknown, root);
    EXPECT_EQ(result.trading_days(), 30U);
    EXPECT_EQ(result.complete_return_periods(), 29U);
    EXPECT_EQ(
        result.annual_volatility().state(),
        r::EvaluationMetric::ANNUAL_PERIODS_NOT_CONFIGURED);
    EXPECT_EQ(result.sharpe().state(), r::EvaluationMetric::ANNUAL_PERIODS_NOT_CONFIGURED);
    EXPECT_FALSE(result.sharpe().has_value());
    EXPECT_TRUE(result.annual_return().has_value());
    EXPECT_EQ(result.opening_equity().units(), original.paper().deposit().units());
    EXPECT_EQ(
        result.closing_equity().units(),
        evidence.backtest().settlements(29).equity().units());
    EXPECT_EQ(result.fees().units(), evidence.backtest().settlements(29).fees().units());
    EXPECT_EQ(result.limitations_size(), 6);
    request.mutable_method()->set_annual_periods(252);
    auto only_periods = tasks::compute_evaluation(capture(), root);
    EXPECT_TRUE(only_periods.annual_volatility().has_value());
    EXPECT_EQ(only_periods.sharpe().state(), r::EvaluationMetric::RISK_FREE_NOT_CONFIGURED);
    request.mutable_method()->set_risk_free(backtest::proto::PerformanceAssumptions::EXPLICIT_ZERO);
    const auto chosen = capture();
    const auto explicit_result = tasks::compute_evaluation(chosen, root);
    EXPECT_TRUE(explicit_result.sharpe().has_value());
    EXPECT_EQ(chosen.source().program_artifact(), evidence.result_task().program_artifact());
    EXPECT_FALSE(evidence.result_task().input().has_performance_assumptions());
    EXPECT_EQ(tasks::verified_result(*store, "source-account").SerializeAsString(), before);
    const auto decoded = protocol::decode_evaluation_result(chosen, explicit_result);
    EXPECT_TRUE(decoded.at("begin_ns").is_string());
    auto malformed = explicit_result;
    malformed.mutable_sharpe()->set_state(r::EvaluationMetric::RISK_FREE_NOT_CONFIGURED);
    EXPECT_THROW(protocol::validate_evaluation_result(chosen, malformed), std::invalid_argument);
}

TEST_F(
    EvaluationEvidence,
    OriginalHoldoutWindowsMatchPublishedScoresAndKeepIndependentMethodChangesExplicit)
{
    publish(true);
    const auto source_before = evidence.SerializeAsString();
    request.set_scope(r::EvaluationRequest::DEVELOPMENT);
    request.mutable_method()->set_annual_periods(252);
    request.mutable_method()->set_risk_free(backtest::proto::PerformanceAssumptions::EXPLICIT_ZERO);
    const auto development = tasks::compute_evaluation(capture(), root);
    EXPECT_EQ(development.trading_days(), 25U);
    EXPECT_EQ(development.complete_return_periods(), 24U);
    ASSERT_TRUE(development.sharpe().has_value());
    EXPECT_NEAR(
        development.sharpe().value(),
        evidence.backtest().candidates(evidence.backtest().selected()).sharpe(),
        1e-12);
    request.mutable_method()->set_annual_periods(250);
    const auto changed = tasks::compute_evaluation(capture(), root);
    EXPECT_NEAR(
        changed.sharpe().value(),
        development.sharpe().value() * std::sqrt(250.0 / 252.0),
        1e-12);
    request.set_scope(r::EvaluationRequest::HOLDOUT);
    const auto held = tasks::compute_evaluation(capture(), root);
    EXPECT_EQ(held.trading_days(), 5U);
    EXPECT_EQ(held.complete_return_periods(), 5U);
    EXPECT_TRUE(held.opening_is_full_period());
    EXPECT_EQ(held.begin_ns(), evidence.backtest().settlements(24).timestamp_ns());
    EXPECT_EQ(held.opening_equity().units(), evidence.backtest().settlements(24).equity().units());
    EXPECT_EQ(tasks::verified_result(*store, "source-account").SerializeAsString(), source_before);
}

TEST_F(EvaluationEvidence, ChangedAuthorityMissingSourceAndCancellationCannotFabricateAReport)
{
    publish();
    auto wrong = request;
    wrong.mutable_source()->mutable_owner()->set_service_id("another-research");
    EXPECT_THROW(
        tasks::capture_evaluation_input(owner, wrong, evidence, input_digest, input_bytes),
        Error);
    request.set_scope(r::EvaluationRequest::HOLDOUT);
    EXPECT_THROW(capture(), std::invalid_argument);
    request.set_scope(r::EvaluationRequest::FULL_ACCOUNT);
    const auto fixed = capture();
    std::stop_source stopped;
    stopped.request_stop();
    EXPECT_THROW(tasks::compute_evaluation(fixed, root, stopped.get_token()), std::runtime_error);
    const auto result_file = root / "source-account/results"
        / (std::to_string(evidence.result_task().attempt()) + ".pb");
    const auto retained = root / "retained-original.pb";
    fs::rename(result_file, retained);
    EXPECT_THROW(tasks::compute_evaluation(fixed, root), std::exception);
    EXPECT_EQ(sha256_file(retained), request.source().version());
    EXPECT_FALSE(fs::exists(result_file));
}

TEST_F(
    EvaluationEvidence,
    OriginalTaskOwnerPublishesOnceKeepsSourceHeldAndReadsExactReportAfterRestart)
{
    publish();
    stress();
    request.mutable_method()->set_annual_periods(252);
    request.mutable_method()->set_risk_free(backtest::proto::PerformanceAssumptions::EXPLICIT_ZERO);
    const auto fixed = capture();
    const auto submit = [&](const char* id, const r::EvaluationInput& definition) {
        auto submission = task_submission(*store, id, definition);
        submission.set_program_artifact(artifact);
        return tasks::submit(*store, std::move(submission));
    };
    const auto admitted = submit("evaluation-report", fixed);
    EXPECT_EQ(admitted.kind(), task::proto::PERFORMANCE_EVALUATION);
    EXPECT_EQ(
        submit("evaluation-report", fixed).submission_sequence(),
        admitted.submission_sequence());
    auto changed = fixed;
    changed.mutable_request()->mutable_method()->set_annual_periods(250);
    try {
        (void)submit("evaluation-report", changed);
        FAIL() << "An accepted report identity must reject changed evaluation assumptions";
    } catch (const std::invalid_argument& error) {
        EXPECT_STREQ(error.what(), "task id already belongs to different input");
    }
    EXPECT_EQ(
        store->retirement("source-account", owner.node_id()).state(),
        task::proto::TaskRetirement::BLOCKED);
    const auto source_bytes = tasks::verified_result(*store, "source-account").SerializeAsString();
    const auto token = store->commit(store->claim("evaluation-report")).token();
    auto read = store->prepare_input("evaluation-report");
    read.load_for_claim();
    auto attempt = store->confirm_attempt(std::move(read));
    attempt.set_token(token);
    EXPECT_THROW(tasks::execute(attempt, root, {}, {}, std::string(64, 'f')), Error);
    EXPECT_FALSE(fs::exists(root / "evaluation-report/results/1.pb"));
    const auto finish = tasks::execute(attempt, root, {}, {}, artifact);
    ASSERT_TRUE(finish.has_evaluation());
    store->commit(tasks::finish(*store, finish));
    const auto saved = tasks::verified_result(*store, "evaluation-report");
    auto report_read = store->prepare_result("evaluation-report");
    report_read.verify();
    ASSERT_TRUE(saved.has_evaluation());
    ASSERT_TRUE(saved.evaluation().has_static_stress());
    const auto& cut = evidence.backtest().settlements(29);
    const auto& position = cut.contracts(0);
    const auto multiplier =
        Decimal::from_raw(original.paper().contracts(0).dataset().contract().multiplier().units());
    const auto change = Decimal::from_raw(position.position_quantity().units())
        * (Decimal::parse("80") - Decimal::from_raw(position.price().units())) * multiplier;
    EXPECT_EQ(saved.evaluation().static_stress().mark_change().units(), change.raw());
    EXPECT_EQ(
        saved.evaluation().static_stress().equity().units(),
        (Decimal::from_raw(cut.equity().units()) + change).raw());
    const auto json = protocol::decode_evaluation_result(fixed, saved.evaluation());
    EXPECT_EQ(json.at("static_stress").at("mark_change"), change.str());
    auto changed_stress = fixed;
    changed_stress.mutable_request()->mutable_static_stress()->mutable_price()->set_units(
        Decimal::parse("81").raw());
    EXPECT_THROW(submit("evaluation-report", changed_stress), std::invalid_argument);
    EXPECT_EQ(saved.result_task().state(), task::proto::SUCCEEDED);
    EXPECT_EQ(saved.result_task().program_artifact(), artifact);
    resource::proto::CatalogueQuery catalogue_query;
    catalogue_query.set_kind("performance_evaluation_result");
    const auto catalogue = store->resources(owner.node_id(), catalogue_query);
    ASSERT_EQ(catalogue.items_size(), 1);
    EXPECT_EQ(catalogue.items(0).object().version(), saved.result_task().result_digest());
    EXPECT_EQ(catalogue.items(0).relative_location(), "evaluation-report/results/1.pb");
    catalogue_query.set_kind("performance_evaluation");
    const auto inputs = store->resources(owner.node_id(), catalogue_query);
    ASSERT_EQ(inputs.items_size(), 1);
    EXPECT_EQ(inputs.items(0).object().version(), report_read.input_digest());
    EXPECT_EQ(inputs.items(0).relative_location(), "evaluation-report/input.pb");

    EXPECT_EQ(
        saved.result_task().evaluation().source().program_artifact(),
        evidence.result_task().program_artifact());
    EXPECT_EQ(submit("evaluation-report", fixed).attempt(), 1U);
    store.reset();
    store = std::make_unique<tasks::Store>(root, tasks::Identity{"research-one"});
    EXPECT_EQ(
        tasks::verified_result(*store, "evaluation-report").SerializeAsString(),
        saved.SerializeAsString());
    EXPECT_EQ(tasks::verified_result(*store, "source-account").SerializeAsString(), source_bytes);
    EXPECT_EQ(
        store->retirement("source-account", owner.node_id()).state(),
        task::proto::TaskRetirement::BLOCKED);
}

TEST_F(
    EvaluationEvidence,
    PreparedReportRecoversExactlyWhileCancelledAndInterruptedWorkNeverAutoPublishes)
{
    publish();
    stress();
    const auto fixed = capture();
    {
        auto pending = task_submission(*store, "pending-evaluation", fixed);
        pending.set_program_artifact(artifact);
        store->admit_submission(pending);
        pending.prepare_files();
        EXPECT_TRUE(
            store->prepared_submission_record("pending-evaluation").execution().has_evaluation());
        EXPECT_TRUE(store->abandon_submission(pending));
    }
    store.reset();
    store = std::make_unique<tasks::Store>(root, tasks::Identity{"research-one"});
    auto resumed = store->resume_submission("pending-evaluation");
    store->admit_submission(resumed);
    store->commit(store->register_submission(resumed));
    EXPECT_EQ(
        store->get("pending-evaluation").evaluation().SerializeAsString(),
        fixed.SerializeAsString());
    store->commit(store->cancel("pending-evaluation"));
    EXPECT_EQ(store->get("pending-evaluation").state(), task::proto::CANCELLED);
    EXPECT_THROW(tasks::verified_result(*store, "pending-evaluation"), std::exception);
    auto running = task_submission(*store, "interrupted-evaluation", fixed);
    running.set_program_artifact(artifact);
    tasks::submit(*store, std::move(running));
    store->commit(store->claim("interrupted-evaluation"));
    store.reset();
    store = std::make_unique<tasks::Store>(root, tasks::Identity{"research-one"});
    EXPECT_EQ(store->get("interrupted-evaluation").state(), task::proto::INTERRUPTED);
    EXPECT_EQ(
        store->get("interrupted-evaluation").evaluation().SerializeAsString(),
        fixed.SerializeAsString());
    EXPECT_THROW(tasks::verified_result(*store, "interrupted-evaluation"), std::exception);
    EXPECT_FALSE(fs::exists(root / "interrupted-evaluation/results/1.pb"));
}

TEST_F(
    EvaluationEvidence,
    RealResearchRpcUsesOriginalProducerAndOwnEvaluatorWithoutDataAcrossRestartAndPreparationRecovery)
{
    using namespace std::chrono_literals;
    publish(false, true, true);
    request.set_include_ledger(true);
    request.mutable_method()->set_annual_periods(252);
    request.mutable_method()->set_risk_free(backtest::proto::PerformanceAssumptions::EXPLICIT_ZERO);
    request.set_include_daily_drawdown(true);
    request.set_include_exposure(true);
    stress();
    const auto original_source = evidence;
    ASSERT_TRUE(request.include_ledger());
    const auto private_runtime =
        fs::temp_directory_path() / ("ast-eval-rpc-" + unique_process_id().substr(0, 12));
    ASSERT_TRUE(fs::create_directory(private_runtime));
    runtime = private_runtime;
    fs::permissions(runtime, fs::perms::owner_all);
    const auto endpoint = (runtime / "rpc.sock").string();
    replace_file_durably(runtime / "research.json", R"({"task_concurrency":2})");
    store.reset();
    testing_support::DataDiscovery discovery(
        endpoint + ".discovery",
        "absent-data",
        endpoint + ".absent-data");
    std::unique_ptr<ChildProcess> service;
    const auto evaluator = sha256_file(ASTERION_RESEARCH_PATH);
    const auto raw = [&](research::proto::ResearchRequest query) {
        query.set_version(1);
        query.set_service_id(owner.service_id());
        query.set_correlation_id(unique_process_id());
        auto channel = ipc::Channel::connect(endpoint, 2s);
        channel.send(query.SerializeAsString(), 2s);
        task::proto::TaskResponse reply;
        if (!reply.ParseFromString(channel.receive(10s)))
            throw std::runtime_error("invalid evaluation fixture reply");
        protocol::validate_message(reply);
        if (reply.version() != query.version() || reply.service_id() != query.service_id()
            || reply.correlation_id() != query.correlation_id())
            throw std::runtime_error("evaluation fixture reply identity mismatch");
        return reply;
    };
    const auto call = [&](research::proto::ResearchRequest query) {
        auto reply = raw(std::move(query));
        if (reply.has_error())
            throw std::runtime_error(reply.error().message());
        return reply;
    };
    const auto start = [&] {
        fs::remove(endpoint);
        fs::remove(endpoint + ".health");
        captured_log = runtime / ("service-" + unique_process_id() + ".log");
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
                owner.service_id(),
                "--node-id",
                "fixture-node",
                "--discovery-endpoint",
                endpoint + ".discovery",
                "--file-workers",
                "1"
            },
            false,
            captured_log,
            true);
        discovery.research(owner.service_id(), service->id(), evaluator);
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        for (;;) {
            try {
                research::proto::ResearchRequest heartbeat;
                heartbeat.mutable_heartbeat();
                if (call(heartbeat).health().initialized())
                    break;
            } catch (const std::exception&) {
                if (service->exited())
                    throw;
            }
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("evaluation Research fixture did not initialize");
            std::this_thread::sleep_for(20ms);
        }
    };
    const auto completed = [&](const char* id) {
        const auto status = testing_support::wait_task(call, id, 10s);
        if (status.state() != task::proto::SUCCEEDED)
            throw std::runtime_error(status.error());
        research::proto::ResearchRequest read;
        read.mutable_result()->set_id(id);
        return call(read);
    };
    start();
    research::proto::ResearchRequest launch;
    launch.mutable_submit()->set_id("rpc-evaluation");
    *launch.mutable_submit()->mutable_evaluation() = request;
    const auto admitted = call(launch).task();
    const auto saved = completed("rpc-evaluation");
    ASSERT_TRUE(saved.has_evaluation());
    ASSERT_TRUE(saved.evaluation().has_daily_drawdown());
    ASSERT_TRUE(saved.evaluation().has_exposure());
    ASSERT_TRUE(saved.evaluation().has_static_stress());
    ASSERT_TRUE(saved.evaluation().has_ledger());
    EXPECT_TRUE(saved.evaluation().ledger().reconciled());
    EXPECT_EQ(
        saved.evaluation().ledger().days_size(),
        original_source.backtest().settlements_size());
    EXPECT_EQ(
        saved.evaluation().ledger().fees().SerializeAsString(),
        original_source.backtest().account().fees().SerializeAsString());
    EXPECT_EQ(
        saved.evaluation().ledger().days(0).recorded_balance().SerializeAsString(),
        original_source.backtest().settlements(0).balance().SerializeAsString());
    EXPECT_EQ(
        saved.evaluation().exposure().source_index(),
        static_cast<unsigned>(original_source.backtest().settlements_size() - 1));
    EXPECT_EQ(
        saved.evaluation().exposure().funds().SerializeAsString(),
        original_source.backtest()
            .settlements(original_source.backtest().settlements_size() - 1)
            .occupancy()
            .SerializeAsString());
    EXPECT_EQ(
        saved.evaluation().daily_drawdown().points_size(),
        original_source.backtest().settlements_size());
    EXPECT_EQ(
        saved.evaluation().daily_drawdown().points(0).equity().units(),
        original_source.backtest().settlements(0).equity().units());
    EXPECT_EQ(saved.result_task().program_artifact(), evaluator);
    EXPECT_EQ(
        saved.result_task().evaluation().source().program_artifact(),
        original_source.result_task().program_artifact());
    EXPECT_NE(
        saved.result_task().program_artifact(),
        saved.result_task().evaluation().source().program_artifact());
    EXPECT_EQ(call(launch).task().submission_sequence(), admitted.submission_sequence());
    auto changed = launch;
    changed.mutable_submit()->mutable_evaluation()->mutable_method()->set_annual_periods(250);
    const auto refused = raw(changed);
    ASSERT_TRUE(refused.has_error());
    EXPECT_EQ(refused.error().code(), "conflict");
    auto no_ledger = launch;
    no_ledger.mutable_submit()->mutable_evaluation()->set_include_ledger(false);
    const auto changed_section = raw(no_ledger);
    ASSERT_TRUE(changed_section.has_error());
    EXPECT_EQ(changed_section.error().code(), "conflict");
    auto wrong = launch;
    wrong.mutable_submit()->set_id("wrong-owner-evaluation");
    wrong.mutable_submit()->mutable_evaluation()->mutable_source()->mutable_owner()->set_service_id(
        "another-research");
    const auto wrong_reply = raw(wrong);
    ASSERT_TRUE(wrong_reply.has_error());
    EXPECT_EQ(wrong_reply.error().code(), "conflict");
    research::proto::ResearchRequest project_save;
    auto* project_command = project_save.mutable_project()->mutable_mutation();
    project_command->set_id("evaluation-question-save");
    auto* project_draft = project_command->mutable_save();
    project_draft->set_project_id("evaluation-question");
    auto* definition = project_draft->mutable_definition();
    definition->set_title("Synthetic evaluation interpretation");
    definition->set_question("Does this fixed evidence support the declared hypothesis?");
    definition->set_hypothesis("A question, not an automatic metric verdict");
    definition->set_support("Independent evidence would be required");
    definition->set_refutation("A fixed interpretation may record insufficient evidence");
    const auto project = call(project_save).project().draft();
    research::proto::ResearchRequest project_fix;
    auto* fix_command = project_fix.mutable_project()->mutable_mutation();
    fix_command->set_id("evaluation-question-fix");
    auto* fix = fix_command->mutable_fix();
    fix->set_project_id(project.id());
    fix->set_plan_id("evaluation-question-plan");
    fix->set_expected_draft_digest(project.digest());
    const auto plan = call(project_fix).project().plan();
    research::proto::ResearchRequest human;
    auto* human_command = human.mutable_project()->mutable_mutation();
    human_command->set_id("evaluation-human-append");
    auto* declaration = human_command->mutable_conclude();
    declaration->set_id("evaluation-human");
    auto* binding = declaration->mutable_plan();
    *binding->mutable_owner() = owner;
    binding->set_project_id(plan.project_id());
    binding->set_plan_id(plan.id());
    binding->set_definition_digest(plan.definition_digest());
    auto* result = declaration->mutable_result();
    *result->mutable_owner() = owner;
    result->set_kind("performance_evaluation_result");
    result->set_id(saved.result_task().id());
    result->set_version(saved.result_task().result_digest());
    declaration->set_judgment(research::proto::INSUFFICIENT);
    declaration->set_reason(
        "Synthetic performance is not independent evidence or trading authorization");
    const auto human_reply = call(human).project();
    const auto verdict = human_reply.conclusion();
    EXPECT_TRUE(verdict.retrospective());
    EXPECT_EQ(verdict.program_artifact(), evaluator);
    EXPECT_EQ(verdict.declaration().result().version(), saved.result_task().result_digest());
    EXPECT_EQ(verdict.declaration().judgment(), research::proto::INSUFFICIENT);
    EXPECT_EQ(
        protocol::decode_project_reply(human_reply, human.project(), owner)
            .at("conclusion")
            .at("retrospective"),
        true);
    EXPECT_EQ(call(human).project().SerializeAsString(), human_reply.SerializeAsString());
    auto edited_human = human;
    edited_human.mutable_project()->mutable_mutation()->mutable_conclude()->set_reason(
        "Changed after the original receipt");
    EXPECT_TRUE(raw(edited_human).has_error());
    auto wrong_version = human;
    wrong_version.mutable_project()->mutable_mutation()->set_id("wrong-evaluation-verdict");
    wrong_version.mutable_project()->mutable_mutation()->mutable_conclude()->set_id(
        "wrong-evaluation-verdict");
    wrong_version.mutable_project()
        ->mutable_mutation()
        ->mutable_conclude()
        ->mutable_result()
        ->set_version(std::string(64, 'f'));
    EXPECT_TRUE(raw(wrong_version).has_error());
    const auto after_interpretation = completed("rpc-evaluation");
    EXPECT_EQ(
        after_interpretation.result_task().SerializeAsString(),
        saved.result_task().SerializeAsString());
    EXPECT_EQ(
        after_interpretation.evaluation().SerializeAsString(),
        saved.evaluation().SerializeAsString());
    service.reset();
    start();
    const auto restored = completed("rpc-evaluation");
    research::proto::ResearchRequest verdict_read;
    verdict_read.mutable_project()->mutable_conclusion()->set_id("evaluation-human");
    EXPECT_EQ(
        call(verdict_read).project().conclusion().SerializeAsString(),
        verdict.SerializeAsString());

    EXPECT_EQ(restored.evaluation().SerializeAsString(), saved.evaluation().SerializeAsString());
    EXPECT_EQ(restored.result_task().SerializeAsString(), saved.result_task().SerializeAsString());
    EXPECT_EQ(call(launch).task().attempt(), 1U);
    research::proto::ResearchRequest original_read;
    original_read.mutable_result()->set_id("source-account");
    const auto unchanged = call(original_read);
    EXPECT_EQ(
        unchanged.backtest().SerializeAsString(),
        original_source.backtest().SerializeAsString());
    EXPECT_EQ(
        unchanged.result_task().SerializeAsString(),
        original_source.result_task().SerializeAsString());
    service.reset();
    {
        tasks::Store pending(root, tasks::Identity{owner.service_id()});
        auto preparation =
            task_submission(pending, "prepared-evaluation", saved.result_task().evaluation());
        preparation.set_program_artifact(evaluator);
        pending.admit_submission(preparation);
        preparation.prepare_files();
        fs::remove(root / "prepared-evaluation/input.pb");
        EXPECT_TRUE(pending.abandon_submission(preparation));
    }
    start();
    research::proto::ResearchRequest recover;
    recover.mutable_recover_submission()->set_id("prepared-evaluation");
    call(recover);
    const auto recovered = completed("prepared-evaluation");
    EXPECT_EQ(recovered.evaluation().SerializeAsString(), saved.evaluation().SerializeAsString());
    EXPECT_EQ(
        recovered.result_task().evaluation().SerializeAsString(),
        saved.result_task().evaluation().SerializeAsString());
    EXPECT_EQ(call(recover).task().attempt(), 1U);
    research::proto::ResearchRequest retire;
    retire.mutable_retirement()->set_id("rpc-evaluation");
    retire.mutable_retirement()->set_execute(true);
    const auto retired = call(retire).retirement();
    EXPECT_EQ(retired.state(), task::proto::TaskRetirement::RETIRED);
    EXPECT_TRUE(retired.source_release_confirmed());
    service.reset();
    EXPECT_TRUE(fs::exists(root / "rpc-evaluation/results/1.pb"));
    EXPECT_FALSE(fs::exists(root / "wrong-owner-evaluation"));
}

TEST_F(EvaluationEvidence, RequestedDailyLossPathKeepsOnlyOriginalHoldoutRowsAndSurvivesPublication)
{
    publish(true);
    request.set_scope(r::EvaluationRequest::HOLDOUT);
    request.set_include_daily_drawdown(true);
    const auto fixed = capture();
    auto submission = task_submission(*store, "daily-loss-report", fixed);
    submission.set_program_artifact(artifact);
    tasks::submit(*store, std::move(submission));
    const auto token = store->commit(store->claim("daily-loss-report")).token();
    auto prepared = store->prepare_input("daily-loss-report");
    prepared.load_for_claim();
    auto execution = store->confirm_attempt(std::move(prepared));
    execution.set_token(token);
    ASSERT_TRUE(execution.execution().evaluation().request().include_daily_drawdown());
    const auto finish = tasks::execute(execution, root, {}, {}, artifact);
    store->commit(tasks::finish(*store, finish));
    const auto saved = tasks::verified_result(*store, "daily-loss-report");
    const auto& result = saved.evaluation();
    ASSERT_TRUE(result.has_daily_drawdown());
    const auto& path = result.daily_drawdown();
    ASSERT_EQ(path.points_size(), 5);
    for (int i = 0; i < path.points_size(); ++i) {
        const auto& row = path.points(i);
        const auto index = fixed.source().first_settlement() + i;
        const auto& original = evidence.backtest().settlements(index);
        EXPECT_EQ(row.source_index(), index);
        EXPECT_EQ(row.trading_day(), original.trading_day());
        EXPECT_EQ(row.timestamp_ns(), original.timestamp_ns());
        EXPECT_EQ(row.equity().SerializeAsString(), original.equity().SerializeAsString());
        EXPECT_GE(row.peak_source_index(), fixed.source().first_settlement());
        EXPECT_LE(row.peak_source_index(), row.source_index());
    }
    EXPECT_EQ(path.points(0).peak_source_index(), fixed.source().first_settlement());
    EXPECT_EQ(result.limitations(2), r::EvaluationResult::RISK_AND_STRESS_INCOMPLETE);
    auto malformed = result;
    malformed.mutable_daily_drawdown()->mutable_points(0)->set_source_index(0);
    EXPECT_THROW(protocol::validate_evaluation_result(fixed, malformed), std::invalid_argument);
    auto unrequested = fixed;
    unrequested.mutable_request()->set_include_daily_drawdown(false);
    EXPECT_THROW(protocol::validate_evaluation_result(unrequested, result), std::invalid_argument);
    auto changed = task_submission(*store, "daily-loss-report", unrequested);
    changed.set_program_artifact(artifact);
    EXPECT_THROW(tasks::submit(*store, std::move(changed)), std::invalid_argument);
    const auto document = protocol::decode_evaluation_result(fixed, result);
    EXPECT_EQ(document.at("daily_drawdown").at("points").size(), 5U);
    store.reset();
    store = std::make_unique<tasks::Store>(root, tasks::Identity{"research-one"});
    EXPECT_EQ(
        tasks::verified_result(*store, "daily-loss-report").SerializeAsString(),
        saved.SerializeAsString());
}

TEST_F(
    EvaluationEvidence,
    OriginalDevelopmentExposureKeepsItsOwnLastFundsAndExactContractSpecifications)
{
    publish(true);
    request.set_scope(r::EvaluationRequest::DEVELOPMENT);
    request.set_include_exposure(true);
    const auto fixed = capture();
    const auto& source = evidence.backtest().settlements(24);
    const auto result = tasks::compute_evaluation(fixed, root);
    ASSERT_FALSE(result.has_daily_drawdown());
    ASSERT_TRUE(result.has_exposure());
    const auto& slice = result.exposure();
    EXPECT_EQ(slice.source_index(), 24U);
    EXPECT_EQ(slice.trading_day(), source.trading_day());
    EXPECT_EQ(slice.timestamp_ns(), source.timestamp_ns());
    EXPECT_NE(slice.timestamp_ns(), evidence.backtest().settlements(29).timestamp_ns());
    EXPECT_EQ(slice.balance().SerializeAsString(), source.balance().SerializeAsString());
    EXPECT_EQ(slice.funds().SerializeAsString(), source.occupancy().SerializeAsString());
    ASSERT_EQ(slice.positions_size(), source.contracts_size());
    const auto& spec = original.paper().contracts(0).dataset().contract();
    EXPECT_EQ(slice.positions(0).venue(), spec.venue());
    EXPECT_EQ(slice.positions(0).symbol(), spec.symbol());
    EXPECT_EQ(slice.positions(0).product(), spec.product());
    EXPECT_EQ(slice.positions(0).delivery_month(), spec.delivery_month());
    EXPECT_EQ(
        slice.positions(0).multiplier().SerializeAsString(),
        spec.multiplier().SerializeAsString());
    EXPECT_EQ(
        slice.positions(0).price().SerializeAsString(),
        source.contracts(0).price().SerializeAsString());
    EXPECT_EQ(
        slice.positions(0).net_quantity().SerializeAsString(),
        source.contracts(0).position_quantity().SerializeAsString());
    EXPECT_EQ(
        slice.positions(0).margin().SerializeAsString(),
        source.contracts(0).margin().SerializeAsString());
    auto malformed = result;
    malformed.mutable_exposure()->set_source_index(29);
    EXPECT_THROW(protocol::validate_evaluation_result(fixed, malformed), std::invalid_argument);
    auto unrequested = fixed;
    unrequested.mutable_request()->set_include_exposure(false);
    EXPECT_THROW(protocol::validate_evaluation_result(unrequested, result), std::invalid_argument);
    auto submission = task_submission(*store, "development-exposure", fixed);
    submission.set_program_artifact(artifact);
    tasks::submit(*store, std::move(submission));
    const auto token = store->commit(store->claim("development-exposure")).token();
    task::proto::TaskFinish finish;
    finish.set_id("development-exposure");
    finish.set_token(token);
    *finish.mutable_evaluation() = result;
    store->commit(tasks::finish(*store, finish));
    const auto saved = tasks::verified_result(*store, finish.id());
    auto changed = task_submission(*store, finish.id(), unrequested);
    changed.set_program_artifact(artifact);
    EXPECT_THROW(tasks::submit(*store, std::move(changed)), std::invalid_argument);
    store.reset();
    store = std::make_unique<tasks::Store>(root, tasks::Identity{"research-one"});
    EXPECT_EQ(
        tasks::verified_result(*store, finish.id()).SerializeAsString(),
        saved.SerializeAsString());
}

TEST_F(EvaluationEvidence, SourceWithoutDailyGrossOrFundsNeverInfersThemFromNetOrAnotherSnapshot)
{
    publish(false, false);
    request.set_include_exposure(true);
    const auto fixed = capture();
    const auto result = tasks::compute_evaluation(fixed, root);
    ASSERT_TRUE(result.has_exposure());
    const auto& slice = result.exposure();
    EXPECT_FALSE(slice.has_funds());
    EXPECT_FALSE(slice.has_covered_gross_notional());
    EXPECT_EQ(slice.gross_to_equity().state(), r::EvaluationMetric::GROSS_NOT_RECORDED);
    EXPECT_EQ(slice.margin_to_equity().state(), r::EvaluationMetric::MARGIN_NOT_RECORDED);
    ASSERT_EQ(slice.positions_size(), 1);
    EXPECT_FALSE(slice.positions(0).has_gross_quantity());
    EXPECT_FALSE(slice.positions(0).has_long_quantity());
    EXPECT_FALSE(slice.positions(0).has_short_quantity());
    EXPECT_FALSE(slice.positions(0).has_margin());
    EXPECT_EQ(slice.positions(0).net_to_equity().state(), r::EvaluationMetric::AVAILABLE);
    const auto shown = protocol::decode_evaluation_result(fixed, result);
    EXPECT_TRUE(shown.at("exposure").at("funds").is_null());
    EXPECT_TRUE(shown.at("exposure").at("positions").at(0).at("gross_quantity").is_null());
}

TEST_F(EvaluationEvidence, StaticStressRequiresOriginalFundingAndExplicitScenarioRatherThanDefaults)
{
    publish(false, false);
    stress();
    EXPECT_THROW(capture(), std::invalid_argument);
    request.set_include_exposure(false);
    EXPECT_THROW(protocol::validate_evaluation_request(request), std::invalid_argument);
    request.set_include_exposure(true);
    request.mutable_static_stress()->set_funds_model(
        r::EvaluationStressScenario::FUNDS_MODEL_UNSPECIFIED);
    EXPECT_THROW(protocol::validate_evaluation_request(request), std::invalid_argument);
    request.mutable_static_stress()->set_funds_model(
        r::EvaluationStressScenario::PAPER_BALANCE_EQUITY_MINIMUM);
    request.mutable_static_stress()->mutable_margin_rate()->set_units(Decimal::parse("-0.4").raw());
    EXPECT_THROW(protocol::validate_evaluation_request(request), std::invalid_argument);
    request.clear_static_stress();
    EXPECT_NO_THROW(capture());
}

TEST_F(
    EvaluationEvidence,
    RequestedFullLedgerPublishesOriginalFillsBalancesAndResidualsAcrossRestart)
{
    publish(false, true, true);
    request.set_include_ledger(true);
    const auto fixed = capture();
    const auto original_bytes = evidence.SerializeAsString();
    auto submission = task_submission(*store, "ledger-report", fixed);
    submission.set_program_artifact(artifact);
    tasks::submit(*store, std::move(submission));
    const auto token = store->commit(store->claim("ledger-report")).token();
    auto prepared = store->prepare_input("ledger-report");
    prepared.load_for_claim();
    auto execution = store->confirm_attempt(std::move(prepared));
    execution.set_token(token);
    const auto completed = tasks::execute(execution, root, {}, {}, artifact);
    store->commit(tasks::finish(*store, completed));
    const auto saved = tasks::verified_result(*store, "ledger-report");
    ASSERT_TRUE(saved.evaluation().has_ledger());
    const auto& ledger = saved.evaluation().ledger();
    EXPECT_TRUE(ledger.reconciled());
    EXPECT_EQ(ledger.residual().units(), 0);
    ASSERT_EQ(ledger.days_size(), evidence.backtest().settlements_size());
    int fills = 0;
    for (int i = 0; i < ledger.days_size(); ++i) {
        const auto& row = ledger.days(i);
        const auto& original_day = evidence.backtest().settlements(i);
        EXPECT_EQ(row.source_index(), static_cast<unsigned>(i));
        EXPECT_EQ(row.trading_day(), original_day.trading_day());
        EXPECT_EQ(row.timestamp_ns(), original_day.timestamp_ns());
        EXPECT_EQ(
            row.recorded_balance().SerializeAsString(),
            original_day.balance().SerializeAsString());
        EXPECT_EQ(
            row.recorded_equity().SerializeAsString(),
            original_day.equity().SerializeAsString());
        for (const auto& fill : row.fills()) {
            const auto& actual = evidence.backtest().account().fills(fills++);
            EXPECT_EQ(fill.id(), actual.id());
            EXPECT_EQ(fill.order_id(), actual.order_id());
            EXPECT_EQ(fill.price().SerializeAsString(), actual.price().SerializeAsString());
            EXPECT_EQ(fill.quantity().SerializeAsString(), actual.quantity().SerializeAsString());
            EXPECT_EQ(fill.fee().SerializeAsString(), actual.fee().SerializeAsString());
        }
    }
    EXPECT_EQ(fills, evidence.backtest().account().fills_size());
    auto wrong_check = saved.evaluation();
    wrong_check.mutable_ledger()->mutable_days(0)->mutable_checks(0)->set_metric("fees");
    EXPECT_THROW(protocol::validate_evaluation_result(fixed, wrong_check), std::invalid_argument);
    auto malformed = saved.evaluation();
    malformed.mutable_ledger()->mutable_residual()->set_units(1);
    EXPECT_THROW(protocol::validate_evaluation_result(fixed, malformed), std::invalid_argument);
    auto unrequested = fixed;
    unrequested.mutable_request()->set_include_ledger(false);
    EXPECT_THROW(
        protocol::validate_evaluation_result(unrequested, saved.evaluation()),
        std::invalid_argument);
    EXPECT_EQ(
        protocol::decode_evaluation_result(fixed, saved.evaluation())
            .at("ledger")
            .at("days")
            .size(),
        30U);
    EXPECT_EQ(tasks::verified_result(*store, "source-account").SerializeAsString(), original_bytes);
    store.reset();
    store = std::make_unique<tasks::Store>(root, tasks::Identity{"research-one"});
    EXPECT_EQ(
        tasks::verified_result(*store, "ledger-report").SerializeAsString(),
        saved.SerializeAsString());
}

TEST_F(EvaluationEvidence, LedgerScopeAndMissingDayEvidenceCannotBeSilentlyApproximated)
{
    publish(true, true, true);
    request.set_include_ledger(true);
    request.set_scope(r::EvaluationRequest::HOLDOUT);
    EXPECT_THROW(capture(), std::invalid_argument);
    request.set_scope(r::EvaluationRequest::FULL_ACCOUNT);
    auto missing = evidence;
    missing.mutable_backtest()->clear_fill_days();
    EXPECT_THROW(
        tasks::capture_evaluation_input(owner, request, missing, input_digest, input_bytes),
        std::invalid_argument);
}

TEST_F(EvaluationEvidence, OriginalContractContributionsPublishFixedAccountAndRowsAcrossRestart)
{
    publish(false, true, true, true);
    request.set_include_contributions(true);
    const auto fixed = capture();
    const auto source = evidence.SerializeAsString();
    auto submission = task_submission(*store, "contract-report", fixed);
    submission.set_program_artifact(artifact);
    tasks::submit(*store, std::move(submission));
    const auto token = store->commit(store->claim("contract-report")).token();
    auto prepared = store->prepare_input("contract-report");
    prepared.load_for_claim();
    auto execution = store->confirm_attempt(std::move(prepared));
    execution.set_token(token);
    const auto completed = tasks::execute(execution, root, {}, {}, artifact);
    store->commit(tasks::finish(*store, completed));
    const auto saved = tasks::verified_result(*store, "contract-report");
    ASSERT_TRUE(saved.evaluation().has_contributions());
    ASSERT_FALSE(saved.evaluation().has_ledger());
    const auto& report = saved.evaluation().contributions();
    ASSERT_TRUE(report.reconciled());
    ASSERT_EQ(report.contracts_size(), 2);
    ASSERT_EQ(report.days_size(), evidence.backtest().settlements_size());
    EXPECT_EQ(report.residual().units(), 0);
    EXPECT_EQ(report.recorded_net().units(), saved.evaluation().net_profit().units());
    std::map<std::string, const protocol::proto::Fill*> fills;
    for (const auto& f : evidence.backtest().account().fills())
        fills.emplace(f.id(), &f);
    std::size_t count = 0;
    for (const auto& c : report.contracts()) {
        EXPECT_EQ(c.venue(), "SHFE");
        for (const auto& row : c.days()) {
            const auto& original_day =
                evidence.backtest().settlements(static_cast<int>(row.source_index()));
            EXPECT_EQ(row.trading_day(), original_day.trading_day());
            for (const auto& f : row.fills()) {
                const auto it = fills.find(f.id());
                ASSERT_NE(it, fills.end());
                const auto& original_fill = *it->second;
                EXPECT_EQ(c.symbol(), original_fill.symbol());
                EXPECT_EQ(f.price().SerializeAsString(), original_fill.price().SerializeAsString());
                EXPECT_EQ(
                    f.quantity().SerializeAsString(),
                    original_fill.quantity().SerializeAsString());
                EXPECT_EQ(f.fee().SerializeAsString(), original_fill.fee().SerializeAsString());
                ++count;
            }
        }
    }
    EXPECT_EQ(count, fills.size());
    for (int i = 0; i < report.days_size(); ++i) {
        EXPECT_EQ(
            report.days(i).recorded_balance().SerializeAsString(),
            evidence.backtest().settlements(i).balance().SerializeAsString());
        EXPECT_EQ(
            report.days(i).recorded_equity().SerializeAsString(),
            evidence.backtest().settlements(i).equity().SerializeAsString());
    }
    auto wrong = saved.evaluation();
    wrong.mutable_contributions()->mutable_contracts(1)->set_symbol(report.contracts(0).symbol());
    EXPECT_THROW(protocol::validate_evaluation_result(fixed, wrong), std::invalid_argument);
    wrong = saved.evaluation();
    wrong.mutable_contributions()
        ->mutable_contracts(1)
        ->mutable_days(0)
        ->mutable_gross()
        ->set_units(1);
    EXPECT_THROW(protocol::validate_evaluation_result(fixed, wrong), std::invalid_argument);
    auto unrequested = fixed;
    unrequested.mutable_request()->set_include_contributions(false);
    EXPECT_THROW(
        protocol::validate_evaluation_result(unrequested, saved.evaluation()),
        std::invalid_argument);
    const auto json = protocol::decode_evaluation_result(fixed, saved.evaluation());
    EXPECT_EQ(json.at("contributions").at("contracts").size(), 2U);
    EXPECT_EQ(tasks::verified_result(*store, "source-account").SerializeAsString(), source);
    store.reset();
    store = std::make_unique<tasks::Store>(root, tasks::Identity{"research-one"});
    EXPECT_EQ(
        tasks::verified_result(*store, "contract-report").SerializeAsString(),
        saved.SerializeAsString());
}

TEST_F(EvaluationEvidence, ContractContributionsRejectPartialAndMissingOriginalDates)
{
    publish(true, true, true, true);
    request.set_include_contributions(true);
    request.set_scope(r::EvaluationRequest::HOLDOUT);
    EXPECT_THROW(capture(), std::invalid_argument);
    request.set_scope(r::EvaluationRequest::FULL_ACCOUNT);
    auto missing = evidence;
    missing.mutable_backtest()->clear_fill_days();
    EXPECT_THROW(
        tasks::capture_evaluation_input(owner, request, missing, input_digest, input_bytes),
        std::invalid_argument);
}

TEST_F(
    EvaluationEvidence,
    RealResearchRpcPublishesOriginalContractContributionsAndRecoversPreparedInput)
{
    using namespace std::chrono_literals;
    publish(false, true, true, true);
    request.set_include_contributions(true);
    const auto original_source = evidence;
    runtime = fs::temp_directory_path() / ("ast-contrib-rpc-" + unique_process_id().substr(0, 12));
    ASSERT_TRUE(fs::create_directory(runtime));
    fs::permissions(runtime, fs::perms::owner_all);
    const auto endpoint = (runtime / "rpc.sock").string();
    replace_file_durably(runtime / "research.json", R"({"task_concurrency":2})");
    store.reset();
    testing_support::DataDiscovery discovery(
        endpoint + ".discovery",
        "absent-data",
        endpoint + ".absent-data");
    const auto evaluator = sha256_file(ASTERION_RESEARCH_PATH);
    std::unique_ptr<ChildProcess> service;
    const auto raw = [&](research::proto::ResearchRequest query) {
        query.set_version(1);
        query.set_service_id(owner.service_id());
        query.set_correlation_id(unique_process_id());
        auto channel = ipc::Channel::connect(endpoint, 2s);
        channel.send(query.SerializeAsString(), 2s);
        task::proto::TaskResponse reply;
        if (!reply.ParseFromString(channel.receive(10s)))
            throw std::runtime_error("invalid evaluation fixture reply");
        protocol::validate_message(reply);
        if (reply.version() != query.version() || reply.service_id() != query.service_id()
            || reply.correlation_id() != query.correlation_id())
            throw std::runtime_error("evaluation fixture reply identity mismatch");
        return reply;
    };
    const auto call = [&](research::proto::ResearchRequest query) {
        auto reply = raw(std::move(query));
        if (reply.has_error())
            throw std::runtime_error(reply.error().message());
        return reply;
    };
    const auto start = [&] {
        fs::remove(endpoint);
        fs::remove(endpoint + ".health");
        captured_log = runtime / ("service-" + unique_process_id() + ".log");
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
                owner.service_id(),
                "--node-id",
                "fixture-node",
                "--discovery-endpoint",
                endpoint + ".discovery",
                "--file-workers",
                "1"
            },
            false,
            captured_log,
            true);
        discovery.research(owner.service_id(), service->id(), evaluator);
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        for (;;) {
            try {
                research::proto::ResearchRequest q;
                q.mutable_heartbeat();
                if (call(q).health().initialized())
                    break;
            } catch (const std::exception&) {
                if (service->exited())
                    throw;
            }
            if (std::chrono::steady_clock::now() >= deadline)
                throw std::runtime_error("evaluation Research fixture did not initialize");
            std::this_thread::sleep_for(20ms);
        }
    };
    const auto completed = [&](const char* id) {
        const auto status = testing_support::wait_task(call, id, 10s);
        if (status.state() != task::proto::SUCCEEDED)
            throw std::runtime_error(status.error());
        research::proto::ResearchRequest read;
        read.mutable_result()->set_id(id);
        return call(read);
    };
    start();
    research::proto::ResearchRequest launch;
    launch.mutable_submit()->set_id("rpc-contributions");
    *launch.mutable_submit()->mutable_evaluation() = request;
    const auto admitted = call(launch).task();
    const auto saved = completed("rpc-contributions");
    ASSERT_TRUE(saved.evaluation().has_contributions());
    EXPECT_TRUE(saved.evaluation().contributions().reconciled());
    EXPECT_EQ(saved.evaluation().contributions().contracts_size(), 2);
    EXPECT_EQ(
        saved.evaluation().contributions().fees().SerializeAsString(),
        original_source.backtest().account().fees().SerializeAsString());
    EXPECT_EQ(
        saved.evaluation().contributions().days(0).recorded_balance().SerializeAsString(),
        original_source.backtest().settlements(0).balance().SerializeAsString());
    EXPECT_EQ(saved.result_task().program_artifact(), evaluator);
    EXPECT_NE(saved.result_task().program_artifact(), artifact);
    const auto replay = call(launch).task();
    EXPECT_EQ(replay.evaluation().SerializeAsString(), admitted.evaluation().SerializeAsString());
    EXPECT_EQ(replay.attempt(), 1U);
    EXPECT_EQ(replay.result_digest(), saved.result_task().result_digest());
    auto changed = launch;
    changed.mutable_submit()->mutable_evaluation()->set_include_contributions(false);
    EXPECT_TRUE(raw(changed).has_error());
    service.reset();
    start();
    const auto restored = completed("rpc-contributions");
    EXPECT_EQ(restored.evaluation().SerializeAsString(), saved.evaluation().SerializeAsString());
    EXPECT_EQ(restored.result_task().SerializeAsString(), saved.result_task().SerializeAsString());
    research::proto::ResearchRequest source;
    source.mutable_result()->set_id("source-account");
    EXPECT_EQ(
        call(source).backtest().SerializeAsString(),
        original_source.backtest().SerializeAsString());
    service.reset();
    {
        tasks::Store pending(root, tasks::Identity{"research-one"});
        auto submission =
            task_submission(pending, "prepared-contributions", saved.result_task().evaluation());
        submission.set_program_artifact(evaluator);
        pending.admit_submission(submission);
        submission.prepare_files();
        EXPECT_TRUE(pending.abandon_submission(submission));
    }
    start();
    research::proto::ResearchRequest recover;
    recover.mutable_recover_submission()->set_id("prepared-contributions");
    EXPECT_NO_THROW(call(recover));
    const auto recovered = completed("prepared-contributions");
    EXPECT_EQ(recovered.evaluation().SerializeAsString(), saved.evaluation().SerializeAsString());
}
