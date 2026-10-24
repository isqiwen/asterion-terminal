#include "research_statistics.hpp"
#include "factor_engine.hpp"
#include "task_store.hpp"
#include "tasks/task_store_support.hpp"
#include "program_files.hpp"
#include "data/bar_fixture.hpp"
#include <asterion/protocol/research_statistics.hpp>
#include <asterion/protocol/factor.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <asterion/kernel/process/child.hpp>
#include <gtest/gtest.h>
#include <filesystem>
using namespace asterion;
namespace fs = std::filesystem;

namespace {

struct ResearchStatistics : testing::Test
{
    fs::path root =
        fs::temp_directory_path() / ("asterion-statistics-fixture-" + unique_process_id());
    resource::proto::Owner owner;
    std::unique_ptr<tasks::Store> store;
    factor::proto::FactorInput factor_input;
    task::proto::TaskResponse evidence;
    research::proto::StatisticsInput input;
    std::string artifact;

    void SetUp() override
    {
        ASSERT_TRUE(fs::create_directory(root));
        root = fs::canonical(root);
        owner.set_node_id("fixture-node");
        owner.set_service_id("research-one");
        owner.set_domain("research");
        store = std::make_unique<tasks::Store>(root, tasks::Identity{"research-one"});
        std::vector<MarketBar> prices;
        for (int i = 0; i < 90; ++i)
            prices.push_back(
                test::flat(
                    "2026-09-25",
                    1790298000000000123LL + static_cast<std::int64_t>(i) * 1000000000,
                    std::to_string(100 + i + i % 3).c_str(),
                    "1"));
        *factor_input.add_series()->mutable_bars() = test::dataset(prices);
        factor_input.mutable_data_owner()->set_node_id("fixture-node");
        factor_input.mutable_data_owner()->set_service_id("synthetic-data");
        factor_input.set_dataset_revision(protocol::factor_revision(factor_input.series()));
        factor_input.set_factor(protocol::proto::PRICE_MOMENTUM);
        factor_input.add_lookbacks(2);
        factor_input.set_horizon(3);
        factor_input.set_holdout_start(45);
        factor_input.set_retain_observations(true);
        artifact = sha256_file(current_executable());
        (void)programs::Files(root / "programs", owner).preserve(current_executable(), artifact);
        auto submitted = store->submission("source-factor", factor_input);
        submitted.set_program_artifact(artifact);
        tasks::submit(*store, std::move(submitted));
        const auto token = store->commit(store->claim("source-factor")).token();
        store->commit(tasks::finish(*store, "source-factor", token, factor::run(factor_input)));
        evidence = tasks::verified_result(*store, "source-factor");
        research::proto::StatisticsRequest request;
        *request.mutable_source()->mutable_owner() = owner;
        request.mutable_source()->set_kind("factor_result");
        request.mutable_source()->set_id("source-factor");
        request.mutable_source()->set_version(evidence.result_task().result_digest());
        request.set_use_scope("Synthetic exploration of original development features");
        request.set_column(research::proto::StatisticsRequest::FEATURE);
        request.set_role(research::proto::StatisticsRequest::DEVELOPMENT);
        request.set_end_index(90);
        request.set_bins(8);
        input = tasks::capture_statistics_input(
            owner,
            request,
            evidence,
            sha256_file(root / "source-factor/input.pb"),
            fs::file_size(root / "source-factor/input.pb"));
    }

    void TearDown() override
    {
        store.reset();
        std::error_code error;
        fs::remove_all(root, error);
    }
};

} // namespace

TEST_F(ResearchStatistics, OwnedOriginalFilesProduceRoleSpecificCountsWithoutChangingTheSource)
{
    const auto input_hash = sha256_file(root / "source-factor/input.pb");
    const auto result_path = root / "source-factor/results/1.pb";
    const auto result_hash = sha256_file(result_path);
    const auto features = tasks::compute_statistics(input, root);
    EXPECT_EQ(features.summary().allowed(), 45U);
    EXPECT_EQ(features.summary().valid(), 43U);
    EXPECT_EQ(features.summary().missing(), 2U);
    EXPECT_EQ(features.rows(42).reason(), research::proto::StatisticsRow::VALID);
    EXPECT_FALSE(features.rows(45).has_value());
    EXPECT_EQ(features.rows(45).reason(), research::proto::StatisticsRow::OTHER_ROLE);
    EXPECT_TRUE(features.rows(0).observed().ends_with(".000000123Z"));
    auto returns = input;
    returns.mutable_request()->set_column(research::proto::StatisticsRequest::FORWARD_RETURN);
    const auto labels = tasks::compute_statistics(returns, root);
    EXPECT_EQ(labels.summary().valid(), 42U);
    EXPECT_EQ(labels.summary().missing(), 3U);
    EXPECT_EQ(labels.rows(0).reason(), research::proto::StatisticsRow::VALID);
    EXPECT_EQ(labels.rows(42).reason(), research::proto::StatisticsRow::CROSSED_LABEL);
    const auto json = protocol::decode_statistics_result(input, features);
    EXPECT_EQ(json.at("summary").at("valid"), 43);
    EXPECT_TRUE(json.at("rows").at(45).at("value").is_null());
    EXPECT_EQ(
        protocol::decode_statistics_input(input).at("source").at("program_artifact"),
        artifact);
    EXPECT_EQ(sha256_file(result_path), result_hash);
    EXPECT_EQ(sha256_file(root / "source-factor/input.pb"), input_hash);
}

TEST_F(ResearchStatistics, ExactOwnerVersionAndReportMembershipAreNotClientClaims)
{
    auto request = input.request();
    request.mutable_source()->mutable_owner()->set_node_id("another-node");
    EXPECT_THROW(
        tasks::capture_statistics_input(
            owner,
            request,
            evidence,
            input.source().input_sha256(),
            input.source().input_bytes()),
        Error);
    request = input.request();
    request.mutable_source()->set_version(std::string(64, 'f'));
    EXPECT_THROW(
        tasks::capture_statistics_input(
            owner,
            request,
            evidence,
            input.source().input_sha256(),
            input.source().input_bytes()),
        Error);
    auto forged = tasks::compute_statistics(input, root);
    forged.mutable_rows(45)->set_value(0);
    forged.mutable_rows(45)->set_bin(0);
    EXPECT_THROW(protocol::validate_statistics_result(input, forged), std::invalid_argument);
    forged = tasks::compute_statistics(input, root);
    forged.mutable_summary()->set_valid(40);
    EXPECT_THROW(protocol::validate_statistics_result(input, forged), std::invalid_argument);
}

TEST_F(ResearchStatistics, MissingOriginalProgramAndChangedSourceBytesNeverCauseRecomputation)
{
    programs::Files files(root / "programs", owner);
    const auto path = files.path(artifact);
    fs::rename(path, root / "held-program-fixture");
    EXPECT_THROW(tasks::compute_statistics(input, root), std::exception);
    fs::rename(root / "held-program-fixture", path);
    auto wrong = input;
    wrong.mutable_source()->set_input_sha256(std::string(64, 'f'));
    EXPECT_THROW(tasks::compute_statistics(wrong, root), std::exception);
    wrong = input;
    wrong.mutable_source()->set_required_history(4);
    EXPECT_THROW(tasks::compute_statistics(wrong, root), std::invalid_argument);
    EXPECT_EQ(tasks::compute_statistics(input, root).summary().valid(), 43U);
}

TEST_F(ResearchStatistics, EmptyEligibleRangeKeepsMissingStatesAndTheTypedRequestRejectsHoldout)
{
    auto empty = input;
    empty.mutable_request()->set_end_index(2);
    const auto result = tasks::compute_statistics(empty, root);
    EXPECT_EQ(result.summary().allowed(), 2U);
    EXPECT_EQ(result.summary().valid(), 0U);
    EXPECT_EQ(result.summary().mean().state(), research::proto::StatisticValue::NO_VALID_SAMPLES);
    EXPECT_FALSE(result.summary().mean().has_value());
    EXPECT_TRUE(result.bins().empty());
    auto query = protocol::decode_statistics_input(input).at("request");
    EXPECT_EQ(
        protocol::encode_statistics_request(query).SerializeAsString(),
        input.request().SerializeAsString());
    query["role"] = "holdout";
    EXPECT_THROW(protocol::encode_statistics_request(query), std::invalid_argument);
    query = protocol::decode_statistics_input(input).at("request");
    query["bins"] = 1.5;
    EXPECT_THROW(protocol::encode_statistics_request(query), std::invalid_argument);
}

TEST_F(ResearchStatistics, PersistentStatisticsPublishAndRestartKeepTheirExactSource)
{
    auto submission = store->submission("statistics-one", input);
    submission.set_program_artifact(artifact);
    const auto accepted = tasks::submit(*store, std::move(submission));
    EXPECT_EQ(accepted.kind(), task::proto::STATISTICS);
    EXPECT_EQ(accepted.metadata().derivation().source_id(), "source-factor");
    EXPECT_EQ(
        accepted.metadata().derivation().source_result_digest(),
        evidence.result_task().result_digest());
    EXPECT_NE(sha256_file(root / "statistics-one/input.pb"), input.source().input_sha256());
    const auto token = store->commit(store->claim("statistics-one")).token();
    task::proto::TaskFinish finished;
    finished.set_id("statistics-one");
    finished.set_token(token);
    *finished.mutable_statistics() = tasks::compute_statistics(input, root);
    store->commit(tasks::finish(*store, std::move(finished)));
    const auto original = tasks::verified_result(*store, "statistics-one");
    ASSERT_TRUE(original.has_statistics());
    EXPECT_EQ(original.statistics().summary().valid(), 43U);
    EXPECT_EQ(original.result_task().program_artifact(), artifact);
    EXPECT_EQ(original.statistics().source().version(), evidence.result_task().result_digest());
    store.reset();
    store = std::make_unique<tasks::Store>(root, tasks::Identity{"research-one"});
    EXPECT_EQ(
        tasks::verified_result(*store, "statistics-one").SerializeAsString(),
        original.SerializeAsString());
    EXPECT_EQ(
        tasks::verified_result(*store, "source-factor").factor().SerializeAsString(),
        evidence.factor().SerializeAsString());
}

TEST_F(ResearchStatistics, FinalAdmissionRejectsChangedSourceCaptureAndMissingExecutionProgram)
{
    auto missing = store->submission("statistics-missing-program", input);
    EXPECT_THROW(store->admit_submission(missing), Error);
    auto changed = input;
    changed.mutable_source()->set_attempt(input.source().attempt() + 1);
    auto submission = store->submission("statistics-wrong-attempt", changed);
    submission.set_program_artifact(artifact);
    EXPECT_THROW(store->admit_submission(submission), Error);
    EXPECT_FALSE(fs::exists(root / "statistics-wrong-attempt/input.pb"));
    auto valid = store->submission("statistics-valid", input);
    valid.set_program_artifact(artifact);
    EXPECT_EQ(tasks::submit(*store, std::move(valid)).kind(), task::proto::STATISTICS);
}

TEST_F(ResearchStatistics, CancellationCannotPublishAndExplicitRetryKeepsOriginalSource)
{
    auto submission = store->submission("statistics-cancelled", input);
    submission.set_program_artifact(artifact);
    tasks::submit(*store, std::move(submission));
    const auto original_input = sha256_file(root / "statistics-cancelled/input.pb");
    const auto token = store->commit(store->claim("statistics-cancelled")).token();
    store->commit(store->cancel("statistics-cancelled"));
    std::stop_source stopped;
    stopped.request_stop();
    EXPECT_THROW(tasks::compute_statistics(input, root, stopped.get_token()), Error);
    store->commit(store->acknowledge_cancel("statistics-cancelled", token));
    EXPECT_EQ(store->describe("statistics-cancelled").state(), task::proto::CANCELLED);
    EXPECT_TRUE(store->describe("statistics-cancelled").result_digest().empty());
    EXPECT_FALSE(fs::exists(root / "statistics-cancelled/results/1.pb"));
    store->commit(store->retry("statistics-cancelled"));
    const auto retry_token = store->commit(store->claim("statistics-cancelled")).token();
    task::proto::TaskFinish completed;
    completed.set_id("statistics-cancelled");
    completed.set_token(retry_token);
    *completed.mutable_statistics() = tasks::compute_statistics(input, root);
    store->commit(tasks::finish(*store, std::move(completed)));
    const auto report = tasks::verified_result(*store, "statistics-cancelled");
    EXPECT_EQ(report.result_task().attempt(), 2U);
    EXPECT_EQ(report.result_task().program_artifact(), artifact);
    EXPECT_EQ(
        report.statistics().source().SerializeAsString(),
        input.request().source().SerializeAsString());
    EXPECT_EQ(sha256_file(root / "statistics-cancelled/input.pb"), original_input);
    EXPECT_FALSE(fs::exists(root / "statistics-cancelled/results/1.pb"));
    EXPECT_EQ(report.statistics().summary().valid(), 43U);
}
