#include "shfe_margin_fixture.hpp"
#include "historical_margins.hpp"
#include "table_update.hpp"
#include <asterion/protocol/data.hpp>
#include <asterion/system/process/child.hpp>
#include <gtest/gtest.h>
using namespace asterion;

namespace {

struct Directory
{
    std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("shfe-margin-" + unique_process_id());

    Directory()
    {
        std::filesystem::create_directory(path);
    }

    ~Directory()
    {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

data::proto::HistoricalMarginQuery query()
{
    data::proto::HistoricalMarginQuery result;
    result.set_contract(shfe::contract);
    for (const auto* day : shfe::days)
        result.add_trading_days(day);
    return result;
}

} // namespace

TEST(ShfeMargins, ReportFieldsSeparatePhasesAndRejectUnsupportedOrAmbiguousRows)
{
    auto raw = test::shfe_margin_response("2025-10-21");
    const auto reports = shfe::reports(raw, "2025-10-21");
    EXPECT_EQ(reports[0].phase, "trading_day_start");
    EXPECT_EQ(reports[0].long_rate, Decimal::parse("0.14"));
    EXPECT_EQ(reports[1].long_rate, Decimal::parse("0.16"));
    EXPECT_EQ(reports[1].short_rate, Decimal::parse("0.16"));
    EXPECT_THROW(shfe::reports(raw, "2025-10-22"), std::invalid_argument);
    EXPECT_THROW(shfe::reports(raw, "2025-10-23"), std::invalid_argument);
    auto bodies = parse_json(raw.body);
    auto settlement = parse_json(bodies.at("settlement").get<std::string>());
    settlement["Settlement"].push_back(settlement["Settlement"][0]);
    bodies["settlement"] = settlement.dump();
    raw.body = bodies.dump();
    EXPECT_THROW(shfe::reports(raw, "2025-10-21"), std::invalid_argument);
}

TEST(ShfeMargins, PinSurvivesRevisionAndRejectsMissingOrAsymmetricFacts)
{
    Directory directory;
    const auto snapshot = test::seed_shfe_margins(directory.path);
    history_files::TableStore store(directory.path / "tables");
    auto request = query();
    request.set_snapshot(snapshot);
    const auto fixed = data::historical_margins(store, request);
    EXPECT_EQ(fixed.days_size(), 3);
    data::proto::TablePageQuery page;
    page.mutable_partition()->set_table(shfe::table);
    page.mutable_partition()->set_exchange("SHFE");
    page.mutable_partition()->set_period("2025-10-21");
    page.set_source(shfe::source);
    page.set_limit(10);
    EXPECT_EQ(
        protocol::decode_table_page(data::table_page(store, page))
            .at("response_evidence")
            .at("format"),
        shfe::response_format);

    EXPECT_EQ(fixed.days(1).settlement_rate().units(), Decimal::parse("0.16").raw());
    EXPECT_FALSE(protocol::decode_historical_margins(fixed).at("publication_known"));
    auto raw = test::shfe_margin_response("2025-10-21");
    auto bodies = parse_json(raw.body);
    auto report = parse_json(bodies.at("settlement").get<std::string>());
    report["Settlement"][0]["SPEC_LONGMARGINRATIO"] = "0.18";
    bodies["settlement"] = report.dump();
    raw.body = bodies.dump();
    const auto revision = test::keep_shfe_margin(store, "2025-10-21", raw);
    EXPECT_EQ(
        data::historical_margins(store, request).SerializeAsString(),
        fixed.SerializeAsString());
    request.set_snapshot(revision);
    EXPECT_THROW(data::historical_margins(store, request), std::invalid_argument);
    Directory empty;
    history_files::TableStore missing(empty.path / "tables");
    request.clear_snapshot();
    EXPECT_THROW(data::historical_margins(missing, request), std::invalid_argument);
}

TEST(ShfeMargins, DownloadPlanUsesOnlyReviewedDatesWithoutInventingCalendar)
{
    Directory directory;
    history_files::TableStore store(directory.path / "tables");
    std::vector<data::TableOffer> offers{
        {{shfe::table, 1, shfe::source}, shfe::provider, {"SHFE"}}
    };
    data::proto::TableUpdate update;
    update.set_provider(shfe::provider);
    update.add_tables(shfe::table);
    update.set_begin_day(shfe::days.front());
    update.set_end_day(shfe::days.back());
    update.set_requests_per_minute(60);
    EXPECT_EQ(
        data::table_update_plan(
            store,
            offers,
            update,
            data::proto::TABLE_PLAN_CALENDAR,
            "2026-10-15")
            .partitions_size(),
        0);
    const auto plan =
        data::table_update_plan(store, offers, update, data::proto::TABLE_PLAN_DAYS, "2026-10-15");
    ASSERT_EQ(plan.partitions_size(), 3);
    test::keep_shfe_margin(
        store,
        shfe::days.front(),
        test::shfe_margin_response(shfe::days.front()));
    EXPECT_EQ(
        data::table_update_plan(store, offers, update, data::proto::TABLE_PLAN_DAYS, "2026-10-15")
            .partitions_size(),
        2);
    update.set_end_day("2025-10-23");
    EXPECT_THROW(
        data::table_update_plan(store, offers, update, data::proto::TABLE_PLAN_DAYS, "2026-10-15"),
        std::invalid_argument);
}
