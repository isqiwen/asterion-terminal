#include "research_sample.hpp"
#include "research_submission.hpp"
#include "data_submission.hpp"
#include "research_statistics.hpp"
#include "task_store.hpp"
#include "tasks/task_store_support.hpp"
#include "program_files.hpp"
#include <asterion/system/process/artifact.hpp>
#include <asterion/system/process/child.hpp>
#include <asterion/protocol/task_execution.hpp>
#include <filesystem>
#include "descriptive_statistics.hpp"
#include "data/bar_fixture.hpp"
#include <asterion/protocol/research_sample.hpp>
#include <asterion/protocol/research_statistics.hpp>
#include <array>
#include <asterion/protocol/data.hpp>
#include <asterion/base/error.hpp>
#include <gtest/gtest.h>
using namespace asterion;

namespace {

research::proto::SampleInput sample_input()
{
    research::proto::SampleInput input;
    input.mutable_owner()->set_node_id("fixture-node");
    input.mutable_owner()->set_service_id("research-one");
    input.mutable_owner()->set_domain("research");
    input.mutable_data_owner()->set_node_id("fixture-node");
    input.mutable_data_owner()->set_service_id("synthetic-data");
    std::vector<MarketBar> values;
    int index = 0;
    for (const auto* close : {"100", "-1", "0", "2", "-2", "4"})
        values.push_back(
            test::flat(
                "2026-09-25",
                1790298000000000123LL + static_cast<std::int64_t>(index++) * 60000000000LL,
                close));
    *input.mutable_dataset() = test::dataset(values);
    auto& request = *input.mutable_request();
    request.set_mode(research::proto::SampleRequest::POST_HOC);
    request.set_use_scope("Synthetic raw close sample; historical availability remains unknown");
    request.set_development_end(4);
    auto& selection = *request.mutable_selection();
    *selection.mutable_daily() = input.dataset().daily();
    *selection.mutable_source_dataset_ids() = input.dataset().source_dataset_ids();
    selection.set_contract(protocol::history_identity(input.dataset().contract()).key());
    return input;
}

} // namespace

TEST(ResearchSample, OriginalZeroAndNegativePricesNeedNoFactorAndHeldValuesAreNotExposed)
{
    const auto input = sample_input();
    const auto original = input.SerializeAsString();
    std::size_t completed = 0, total = 0;
    const auto result = tasks::capture_sample(input, {}, [&](auto done, auto work) {
        completed = done;
        total = work;
    });
    EXPECT_EQ(result.rows_size(), 6);
    EXPECT_EQ(completed, 12U);
    EXPECT_EQ(total, 12U);
    EXPECT_EQ(result.rows(3).role(), research::proto::SampleRow::DEVELOPMENT);
    EXPECT_EQ(result.rows(4).role(), research::proto::SampleRow::HOLDOUT);
    EXPECT_EQ(result.rows(0).observed_ns(), 1790298000000000123LL);
    const auto json = protocol::decode_sample_result(input, result);
    EXPECT_EQ(json.at("rows").at(1).at("value"), "-1");
    EXPECT_EQ(json.at("rows").at(2).at("value"), "0");
    EXPECT_TRUE(json.at("rows").at(4).at("value").is_null());
    EXPECT_EQ(json.at("historical_availability"), "unknown");
    EXPECT_EQ(json.at("complete_history"), "unproven");
    EXPECT_TRUE(json.at("rows").at(0).at("observed").get<std::string>().ends_with(".000000123Z"));
    EXPECT_EQ(input.SerializeAsString(), original);
}

TEST(ResearchSample, ExplicitScopeAndSourceIdentityRejectUnknownOrChangedClaims)
{
    const auto input = sample_input();
    auto changed = input;
    changed.mutable_request()->clear_mode();
    EXPECT_THROW(tasks::capture_sample(changed), std::invalid_argument);
    changed = input;
    changed.mutable_request()->set_use_scope(" \t\r\n");
    EXPECT_THROW(tasks::capture_sample(changed), std::invalid_argument);
    changed = input;
    changed.mutable_request()->mutable_selection()->mutable_daily()->set_snapshot(
        std::string(64, 'f'));
    EXPECT_THROW(tasks::capture_sample(changed), std::invalid_argument);
    changed = input;
    changed.mutable_data_owner()->set_node_id("another-node");
    EXPECT_THROW(tasks::capture_sample(changed), std::invalid_argument);
    changed = input;
    changed.mutable_request()->set_development_end(6);
    EXPECT_THROW(tasks::capture_sample(changed), std::invalid_argument);
    const auto json = protocol::decode_sample_input(input).at("request");
    EXPECT_EQ(
        protocol::encode_sample_request(json).SerializeAsString(),
        input.request().SerializeAsString());
    auto fractional = json;
    fractional["evaluation"]["split_index"] = 2.5;
    EXPECT_THROW(protocol::encode_sample_request(fractional), std::invalid_argument);
}

TEST(ResearchSample, FuturePricesDoNotChangeEarlierMembershipOrMakeAvailabilityKnown)
{
    const auto original = sample_input();
    auto later = original;
    for (int i = 4; i < later.dataset().bars_size(); ++i) {
        auto& bar = *later.mutable_dataset()->mutable_bars(i);
        for (auto* value :
            {bar.mutable_open(), bar.mutable_high(), bar.mutable_low(), bar.mutable_close()})
            value->set_units(50000000000LL + i * 100000000LL);
    }
    later.mutable_dataset()->set_revision(protocol::bar_dataset_revision(later.dataset()));
    const auto before = tasks::capture_sample(original);
    const auto after = tasks::capture_sample(later);
    for (int i = 0; i < 4; ++i)
        EXPECT_EQ(before.rows(i).SerializeAsString(), after.rows(i).SerializeAsString());
    const auto left = protocol::decode_sample_result(original, before);
    const auto right = protocol::decode_sample_result(later, after);
    for (int i = 0; i < 4; ++i)
        EXPECT_EQ(left.at("rows").at(i), right.at("rows").at(i));
    EXPECT_TRUE(right.at("rows").at(4).at("value").is_null());
    EXPECT_EQ(right.at("historical_availability"), "unknown");
    auto forged = before;
    forged.mutable_rows(4)->set_role(research::proto::SampleRow::DEVELOPMENT);
    EXPECT_THROW(protocol::validate_sample_result(original, forged), std::invalid_argument);
}

TEST(ResearchSample, ActualObservationRoleSelectionStopsCooperativelyWithoutCalendarPadding)
{
    std::stop_source stop;
    EXPECT_THROW(
        observation_roles(
            200000,
            100000,
            stop.get_token(),
            [&](auto count, auto) {
                if (count >= 4096)
                    stop.request_stop();
            }),
        Error);
    auto one = sample_input();
    one.mutable_request()->set_full_sample(true);
    const auto result = tasks::capture_sample(one);
    EXPECT_EQ(result.rows_size(), one.dataset().bars_size());
    for (const auto& row : result.rows())
        EXPECT_EQ(row.role(), research::proto::SampleRow::FULL_SAMPLE);
}

TEST(ResearchSample, PersistentSampleOwnsFormalRowsAndExactDataReferencesAcrossRestart)
{
    namespace fs = std::filesystem;

    struct Folder
    {
        fs::path path = fs::temp_directory_path() / ("asterion-raw-sample-" + unique_process_id());

        Folder()
        {
            if (!fs::create_directory(path))
                throw std::runtime_error("sample fixture directory exists");
            path = fs::canonical(path);
        }

        ~Folder()
        {
            std::error_code error;
            fs::remove_all(path, error);
        }
    } folder;

    auto input = sample_input();
    const std::array<std::int64_t, 6> times = {
        1790254800000000123LL,
        1790254860000000123LL,
        1790298000000000123LL,
        1790514000000000123LL,
        1790557200000000123LL,
        1790557260000000123LL
    };
    const std::array<const char*, 6> prices = {"100", "-1", "0", "2", "-2", "4"};
    std::vector<MarketBar> grouped_bars;
    for (std::size_t i = 0; i < times.size(); ++i)
        grouped_bars.push_back(
            test::flat(i < 3 ? "2026-09-25" : "2026-09-28", times[i], prices[i]));
    *input.mutable_dataset() = test::dataset(grouped_bars);
    const auto artifact = sha256_file(current_executable());
    task::proto::TaskResponse original;
    task::proto::TaskResponse original_statistics;
    task::proto::TaskResponse original_grouped;
    {
        tasks::Store store(asterion::research::task_domain(), folder.path, {"research-one"});
        (void)programs::Files(folder.path / "programs", input.owner())
            .preserve(current_executable(), artifact);
        auto submission = task_submission(store, "raw-sample", input);
        asterion::tasks::set_program_artifact(submission, artifact);
        const auto admitted = tasks::submit(store, std::move(submission));
        EXPECT_EQ(admitted.kind(), task::proto::SAMPLE);
        auto read = store.prepare_input("raw-sample");
        read.load_for_claim();
        ASSERT_TRUE(read.execution().has_sample());
        EXPECT_LT(read.execution().ByteSizeLong(), input.ByteSizeLong());
        const auto references = protocol::task_data_references(read.execution());
        EXPECT_EQ(references.minutes, std::vector<std::string>{std::string(64, 'a')});
        EXPECT_EQ(references.snapshots, std::vector<std::string>{std::string(64, 'b')});
        const auto token = store.commit(store.claim(read)).token();
        const auto attempt = store.confirm_attempt(std::move(read));
        ASSERT_TRUE(attempt.task().has_sample());
        EXPECT_EQ(attempt.task().sample().SerializeAsString(), input.SerializeAsString());
        task::proto::TaskFinish finish;
        finish.set_id("raw-sample");
        finish.set_token(token);
        *finish.mutable_sample() = tasks::capture_sample(attempt.task().sample());
        store.commit(tasks::finish(store, std::move(finish)));
        original = tasks::verified_result(store, "raw-sample");
        const auto usage = store.history_usage(std::string(64, 'a'));
        ASSERT_EQ(usage.references_size(), 1);
        EXPECT_EQ(usage.references(0).kind(), data::proto::HISTORY_SAMPLE);
        EXPECT_EQ(original.result_task().program_artifact(), artifact);
        EXPECT_EQ(original.sample().rows_size(), 6);
        research::proto::StatisticsRequest request;
        *request.mutable_source()->mutable_owner() = input.owner();
        request.mutable_source()->set_kind("sample_result");
        request.mutable_source()->set_id("raw-sample");
        request.mutable_source()->set_version(original.result_task().result_digest());
        request.set_use_scope("Synthetic original raw-close distribution");
        request.set_column(research::proto::StatisticsRequest::RAW_CLOSE);
        request.set_role(research::proto::StatisticsRequest::DEVELOPMENT);
        request.set_end_index(6);
        request.set_bins(3);
        const auto captured = tasks::capture_statistics_input(
            input.owner(),
            request,
            original,
            sha256_file(folder.path / "raw-sample/input.pb"),
            fs::file_size(folder.path / "raw-sample/input.pb"));
        auto statistics = task_submission(store, "raw-statistics", captured);
        asterion::tasks::set_program_artifact(statistics, artifact);
        tasks::submit(store, std::move(statistics));
        EXPECT_EQ(store.retirement("raw-sample", "fixture-node").consumers_size(), 1);
        const auto statistics_token = store.commit(store.claim("raw-statistics")).token();
        task::proto::TaskFinish finished;
        finished.set_id("raw-statistics");
        finished.set_token(statistics_token);
        *finished.mutable_statistics() = tasks::compute_statistics(captured, folder.path);
        store.commit(tasks::finish(store, std::move(finished)));
        original_statistics = tasks::verified_result(store, "raw-statistics");
        const auto& report = original_statistics.statistics();
        EXPECT_EQ(report.summary().valid(), 4U);
        EXPECT_DOUBLE_EQ(report.summary().mean().value(), 25.25);
        EXPECT_DOUBLE_EQ(report.summary().minimum().value(), -1);
        EXPECT_DOUBLE_EQ(report.summary().median().value(), 1);
        EXPECT_DOUBLE_EQ(report.rows(2).value(), 0);
        EXPECT_FALSE(report.rows(4).has_value());
        EXPECT_EQ(report.price_power(), 1);
        EXPECT_EQ(store.history_usage(std::string(64, 'a')).references_size(), 1);
        auto grouped_input = captured;
        grouped_input.mutable_request()->set_grouping(
            research::proto::StatisticsRequest::TRADING_DAY);
        auto grouped_submission = task_submission(store, "raw-day-groups", grouped_input);
        asterion::tasks::set_program_artifact(grouped_submission, artifact);
        const auto grouped_task = tasks::submit(store, std::move(grouped_submission));
        EXPECT_EQ(grouped_task.total(), 24U);
        const auto grouped_token = store.commit(store.claim("raw-day-groups")).token();
        task::proto::TaskFinish grouped_finish;
        grouped_finish.set_id("raw-day-groups");
        grouped_finish.set_token(grouped_token);
        std::size_t completed = 0, total = 0;
        *grouped_finish.mutable_statistics() =
            tasks::compute_statistics(grouped_input, folder.path, {}, [&](auto done, auto work) {
                completed = done;
                total = work;
            });
        EXPECT_EQ(completed, 24U);
        EXPECT_EQ(total, 24U);
        store.commit(tasks::finish(store, std::move(grouped_finish)));
        original_grouped = tasks::verified_result(store, "raw-day-groups");
        const auto& days = original_grouped.statistics();
        ASSERT_EQ(days.groups_size(), 2);
        EXPECT_EQ(days.rows(0).trading_day(), "2026-09-25");
        EXPECT_TRUE(days.rows(0).observed().starts_with("2026-09-24T"));
        EXPECT_TRUE(days.rows(2).observed().starts_with("2026-09-25T"));
        EXPECT_EQ(days.groups(0).trading_day(), "2026-09-25");
        EXPECT_EQ(days.groups(0).summary().valid(), 3U);
        EXPECT_DOUBLE_EQ(days.groups(0).summary().mean().value(), 33);
        EXPECT_DOUBLE_EQ(days.groups(0).summary().median().value(), 0);
        EXPECT_EQ(days.groups(1).summary().total(), 3U);
        EXPECT_EQ(days.groups(1).summary().valid(), 1U);
        EXPECT_DOUBLE_EQ(days.groups(1).summary().mean().value(), 2);
        EXPECT_EQ(
            days.groups(1).summary().standard_deviation().state(),
            research::proto::StatisticValue::INSUFFICIENT_SAMPLES);
        EXPECT_FALSE(days.rows(4).has_value());
        auto wrong_group = days;
        wrong_group.mutable_groups(0)->set_trading_day("2026-09-24");
        EXPECT_THROW(
            protocol::validate_statistics_result(grouped_input, wrong_group),
            std::invalid_argument);
        wrong_group = days;
        wrong_group.mutable_groups(0)->set_bin_counts(0, wrong_group.groups(0).bin_counts(0) + 1);
        EXPECT_THROW(
            protocol::validate_statistics_result(grouped_input, wrong_group),
            std::invalid_argument);
    }
    tasks::Store restored(asterion::research::task_domain(), folder.path, {"research-one"});
    EXPECT_EQ(
        tasks::verified_result(restored, "raw-sample").SerializeAsString(),
        original.SerializeAsString());
    EXPECT_EQ(
        tasks::verified_result(restored, "raw-statistics").SerializeAsString(),
        original_statistics.SerializeAsString());
    EXPECT_EQ(
        tasks::verified_result(restored, "raw-day-groups").SerializeAsString(),
        original_grouped.SerializeAsString());
    EXPECT_EQ(
        protocol::decode_task_result(original, "raw-sample")
            .at("result")
            .at("rows")
            .at(2)
            .at("value"),
        "0");
}
