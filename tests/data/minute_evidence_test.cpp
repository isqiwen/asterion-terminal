#include "data/history_fixture.hpp"
#include "history_coverage.hpp"
#include "history_update.hpp"
#include <asterion/kernel/process/child.hpp>
#include <asterion/protocol/data.hpp>
#include <fstream>
#include <gtest/gtest.h>
using namespace asterion;

namespace {

struct Folder
{
    std::filesystem::path path = std::filesystem::temp_directory_path() / unique_process_id();

    Folder()
    {
        std::filesystem::create_directory(path);
    }

    ~Folder()
    {
        std::filesystem::remove_all(path);
    }
};

class Minutes final : public HistoricalBarPort
{
public:
    std::vector<HistoricalBar> bars;

    HistorySemantics semantics() const override
    {
        return {"tushare.ft_mins", "fixture.evidence.v1", "Asia/Shanghai", "bar_end"};
    }

    std::vector<HistoricalBar> read(const HistoricalBarRange& range, std::stop_token) override
    {
        std::vector<HistoricalBar> selected;
        for (const auto& bar : bars)
            if (bar.timestamp_ns >= range.begin_ns && bar.timestamp_ns <= range.end_ns)
                selected.push_back(bar);
        return selected;
    }

    void add(const char* at, const char* day)
    {
        const auto price = Decimal::parse("100");
        bars.push_back(
            {parse_shanghai_time(at),
                price,
                price,
                price,
                price,
                Decimal::parse("1"),
                price,
                Decimal::parse("2"),
                day});
    }
};

data::proto::HistoryRecord publish(const std::filesystem::path& root, Minutes& source)
{
    data::proto::MinuteDownload input;
    input.set_contract_id("SHFE/rb/2026-10");
    input.set_source("tushare.ft_mins");
    input.set_source_instrument("RB2610.SHF");
    input.set_interval_minutes(1);
    // The night and day bars of September 7 lie in different storage segments.
    input.set_begin_ns(parse_shanghai_time("2026-08-06 00:00:00"));
    input.set_end_ns(parse_shanghai_time("2026-09-09 23:59:59"));
    input.set_requests_per_minute(60);
    data::Store store(root, "historical-data", "task");
    return test::publish_download(store, "evidence", input, source);
}

data::proto::MinuteEvidenceQuery
query(const data::proto::HistoryRecord& record, unsigned limit = 50)
{
    return protocol::encode_minute_evidence_query(
        {{"dataset_id", record.minute_result().manifest_sha256()},
            {"offset", 0},
            {"limit", limit}});
}

} // namespace

TEST(MinuteEvidence, GroupsNightAndDayAcrossSegmentsAndPreservesUnknownLabels)
{
    Folder folder;
    Minutes source;
    source.add("2026-09-04 21:01:00", "2026-09-07");
    source.add("2026-09-07 09:01:00", "2026-09-07");
    source.add("2026-09-08 09:01:00", "");
    source.add("2026-09-09 09:01:00", "2026-09-09");
    const auto record = publish(folder.path, source);
    history_files::Archive archive(folder.path / "history");
    auto request = query(record, 1);
    const auto first = data::minute_evidence(archive, request);
    EXPECT_EQ(first.total_rows(), 4);
    EXPECT_EQ(first.unknown_rows(), 1);
    EXPECT_EQ(first.total_days(), 2);
    EXPECT_EQ(first.begin_ns(), record.minutes().begin_ns());
    EXPECT_EQ(first.end_ns(), record.minutes().end_ns());
    ASSERT_EQ(first.days_size(), 1);
    EXPECT_EQ(first.days(0).trading_day(), "2026-09-07");
    EXPECT_EQ(first.days(0).rows(), 2);
    EXPECT_EQ(first.days(0).first_ns(), source.bars[0].timestamp_ns);
    EXPECT_EQ(first.days(0).last_ns(), source.bars[1].timestamp_ns);
    EXPECT_EQ(protocol::decode_minute_evidence(first).at("days").size(), 1);
    request.set_offset(1);
    const auto last = data::minute_evidence(archive, request);
    ASSERT_EQ(last.days_size(), 1);
    EXPECT_EQ(last.days(0).trading_day(), "2026-09-09");
    EXPECT_EQ(last.offset(), 1);
    EXPECT_NO_THROW(protocol::decode_minute_evidence(last));

    const auto daily = test::seed_daily_table(
        folder.path,
        "SHFE/rb/2026-10",
        std::vector<std::string>{"2026-09-07", "2026-09-08", "2026-09-09"},
        100,
        2);
    history_files::TableStore tables(folder.path / "tables");
    data::proto::HistoryCoverageQuery coverage;
    *coverage.mutable_daily() = protocol::encode_daily_table(daily);
    const auto result = data::history_coverage(archive, tables, coverage);
    ASSERT_EQ(result.items_size(), 1);
    EXPECT_EQ(result.items(0).unknown_rows(), 1);
    EXPECT_TRUE(result.items(0).compared_first().empty());
    EXPECT_EQ(result.items(0).uncovered(), 0) << "Unlabeled September 8 is not a proven gap";
    auto repair = protocol::encode_history_update_query(
        {{"dataset_id", request.dataset_id()},
            {"daily", daily},
            {"mode", "repair"},
            {"end_day", ""},
            {"requests_per_minute", 60}});
    EXPECT_THROW(
        data::history_update_plan(
            archive,
            tables,
            repair,
            parse_shanghai_time("2026-10-01 00:00:00")),
        std::invalid_argument);
}

TEST(MinuteEvidence, EmptyDownloadHasNoInventedDaysAndCorruptSegmentsAreRejected)
{
    Folder folder;
    Minutes source;
    const auto record = publish(folder.path, source);
    history_files::Archive archive(folder.path / "history");
    const auto evidence = data::minute_evidence(archive, query(record));
    EXPECT_EQ(evidence.total_rows(), 0);
    EXPECT_EQ(evidence.total_days(), 0);
    EXPECT_EQ(evidence.unknown_rows(), 0);
    EXPECT_TRUE(evidence.days().empty());
    EXPECT_NO_THROW(protocol::decode_minute_evidence(evidence));
    auto invalid = query(record);
    invalid.set_limit(0);
    EXPECT_THROW(data::minute_evidence(archive, invalid), std::invalid_argument);
    const auto directory = std::filesystem::path(record.minute_result().directory());
    bool changed = false;
    for (const auto& entry : std::filesystem::directory_iterator(directory))
        if (entry.path().extension() == ".parquet") {
            std::ofstream(entry.path(), std::ios::app) << "corrupt";
            changed = true;
            break;
        }
    ASSERT_TRUE(changed);
    EXPECT_THROW(data::minute_evidence(archive, query(record)), std::invalid_argument);
}

namespace {

Json range_calendar(
    history_files::TableStore& tables,
    Json daily,
    const std::string& year,
    const std::vector<TableSourceRow>& rows)
{
    const auto partition = table_partition(history_table("futures.calendar"), "SHFE", year, rows);
    (void)tables.record(
        partition,
        {"futures.calendar", 1, daily.at("calendar_source").get<std::string>()},
        tables.write(partition),
        std::string(64, 'b'),
        1790000000000000001);
    daily["snapshot"] = tables.head();
    return daily;
}

Json range_calendar(
    const std::filesystem::path& root,
    Json daily,
    const std::string& year,
    const std::vector<TableSourceRow>& rows)
{
    history_files::TableStore tables(root / "tables");
    return range_calendar(tables, std::move(daily), year, rows);
}

data::proto::MinuteRangeQuery range_query(
    const data::proto::HistoryRecord& record,
    const Json& daily,
    const char* begin,
    const char* end)
{
    return protocol::encode_minute_range_query(
        {{"dataset_id", record.minute_result().manifest_sha256()},
            {"session_rules_version", 1},
            {"daily", daily},
            {"begin_day", begin},
            {"end_day", end}});
}

} // namespace

TEST(MinuteRange, IncludesTargetEdgesAndClosedDaysWithoutInventingZeroRowsAndPinsSnapshot)
{
    Folder folder;
    Minutes source;
    source.add("2026-09-04 21:01:00", "2026-09-07");
    source.add("2026-09-07 09:01:00", "2026-09-07");
    const auto record = publish(folder.path, source);
    auto daily = test::seed_daily_table(
        folder.path,
        "SHFE/rb/2026-10",
        std::vector<test::DailyRow>{
            {"2026-09-03", 100, 2, test::DailyTrade{100, 100, 100, 100, 1}},
            {"2026-09-04", 100, 2, test::DailyTrade{100, 100, 100, 100, 1}},
            {"2026-09-07", 100, 2, test::DailyTrade{100, 100, 100, 100, 1}},
            {"2026-09-09", 100, 2, test::DailyTrade{100, 100, 100, 100, 1}},
            {"2026-09-11", 100, 2}
        });
    const std::vector<TableSourceRow> calendar{
        {"2026-09-03", "1", std::nullopt},
        {"2026-09-04", "1", std::nullopt},
        {"2026-09-05", "0", std::nullopt},
        {"2026-09-06", "0", std::nullopt},
        {"2026-09-07", "1", std::nullopt},
        {"2026-09-08", "1", std::nullopt},
        {"2026-09-09", "1", std::nullopt},
        {"2026-09-10", "0", std::nullopt},
        {"2026-09-11", "1", std::nullopt}
    };
    daily = range_calendar(folder.path, daily, "2026", calendar);
    const auto request = range_query(record, daily, "2026-09-03", "2026-09-12");
    history_files::Archive archive(folder.path / "history");
    auto tables = std::make_unique<history_files::TableStore>(folder.path / "tables");
    const auto before = archive.get(request.dataset_id()).SerializeAsString();
    const auto result = data::minute_range_evidence(archive, *tables, request);
    ASSERT_EQ(result.days_size(), 10);
    const data::proto::MinuteRangeState expected[] = {
        data::proto::RANGE_UNCOVERED,
        data::proto::RANGE_UNCOVERED,
        data::proto::RANGE_CLOSED,
        data::proto::RANGE_CLOSED,
        data::proto::RANGE_OBSERVED,
        data::proto::RANGE_UNKNOWN_DAILY,
        data::proto::RANGE_UNCOVERED,
        data::proto::RANGE_CLOSED,
        data::proto::RANGE_NO_AUCTION,
        data::proto::RANGE_UNKNOWN_CALENDAR
    };
    for (int i = 0; i < result.days_size(); ++i) {
        EXPECT_EQ(result.days(i).state(), expected[i]) << i;
        EXPECT_EQ(result.days(i).has_observed(), i == 4);
    }
    EXPECT_EQ(result.days(4).observed().rows(), 2);
    EXPECT_EQ(result.days(4).observed().first_ns(), source.bars[0].timestamp_ns);
    EXPECT_EQ(result.request_begin_ns(), record.minutes().begin_ns());
    const auto decoded = protocol::decode_minute_range_evidence(result);
    EXPECT_TRUE(decoded.at("days").at(0).at("observed").is_null());
    EXPECT_EQ(decoded.at("request_session_coverage"), "unverified");
    auto updated = calendar;
    updated[1][1] = "0";
    updated[4][1] = "0";
    const auto next = range_calendar(*tables, daily, "2026", updated);
    EXPECT_NE(next.at("snapshot"), daily.at("snapshot"));
    EXPECT_EQ(
        data::minute_range_evidence(archive, *tables, request).SerializeAsString(),
        result.SerializeAsString());
    const auto current = data::minute_range_evidence(
        archive,
        *tables,
        range_query(record, next, "2026-09-03", "2026-09-12"));
    EXPECT_EQ(current.days(1).state(), data::proto::RANGE_CLOSED);
    EXPECT_EQ(current.days(4).state(), data::proto::RANGE_CALENDAR_CONFLICT);
    EXPECT_EQ(archive.get(request.dataset_id()).SerializeAsString(), before);
    tables.reset();
    history_files::TableStore reopened(folder.path / "tables");
    EXPECT_EQ(
        data::minute_range_evidence(archive, reopened, request).SerializeAsString(),
        result.SerializeAsString());
}

TEST(MinuteRange, UnknownLabelsAndCalendarNeverBecomeProvenGaps)
{
    Folder folder;
    Minutes source;
    source.add("2026-09-08 09:01:00", "");
    const auto record = publish(folder.path, source);
    auto daily = test::seed_daily_table(
        folder.path,
        "SHFE/rb/2026-10",
        std::vector<std::string>{"2026-09-07", "2026-09-08", "2026-09-09"},
        100,
        2);
    daily = range_calendar(
        folder.path,
        daily,
        "2026",
        {{"2026-09-07", "1", std::nullopt},
            {"2026-09-08", std::nullopt, std::nullopt},
            {"2026-09-09", "1", std::nullopt}});
    history_files::Archive archive(folder.path / "history");
    history_files::TableStore tables(folder.path / "tables");
    const auto result = data::minute_range_evidence(
        archive,
        tables,
        range_query(record, daily, "2026-09-07", "2026-09-10"));
    EXPECT_EQ(result.unknown_rows(), 1);
    EXPECT_EQ(result.days(0).state(), data::proto::RANGE_UNKNOWN_LABELS);
    EXPECT_EQ(result.days(1).state(), data::proto::RANGE_UNKNOWN_CALENDAR);
    EXPECT_EQ(result.days(2).state(), data::proto::RANGE_UNKNOWN_LABELS);
    EXPECT_EQ(result.days(3).state(), data::proto::RANGE_UNKNOWN_CALENDAR);
}

TEST(MinuteRange, ValidatesInclusiveLeapAndYearBoundariesAndUnavailableSnapshots)
{
    Folder folder;
    Minutes source;
    const auto record = publish(folder.path, source);
    const auto daily = test::seed_daily_table(
        folder.path,
        "SHFE/rb/2026-10",
        std::vector<std::string>{"2026-09-07"},
        100,
        2);
    history_files::Archive archive(folder.path / "history");
    history_files::TableStore tables(folder.path / "tables");
    for (const auto& dates : std::vector<std::pair<const char*, const char*>>{
             {"2023-12-31", "2024-01-02"},
             {"2024-02-28", "2024-03-01"}
         }) {
        const auto result = data::minute_range_evidence(
            archive,
            tables,
            range_query(record, daily, dates.first, dates.second));
        ASSERT_EQ(result.days_size(), 3);
        EXPECT_EQ(result.days(0).trading_day(), dates.first);
        EXPECT_EQ(result.days(2).trading_day(), dates.second);
        for (const auto& day : result.days()) {
            EXPECT_EQ(day.state(), data::proto::RANGE_UNKNOWN_CALENDAR);
            EXPECT_FALSE(day.has_observed());
        }
        EXPECT_NO_THROW(protocol::decode_minute_range_evidence(result));
    }
    EXPECT_NO_THROW(range_query(record, daily, "2024-01-01", "2024-12-31"));
    EXPECT_THROW(range_query(record, daily, "2024-01-01", "2025-01-01"), std::invalid_argument);
    EXPECT_THROW(range_query(record, daily, "2026-09-09", "2026-09-07"), std::invalid_argument);
    EXPECT_THROW(range_query(record, daily, "2023-02-29", "2023-03-01"), std::invalid_argument);
    auto request = range_query(record, daily, "2026-09-07", "2026-09-07");
    request.mutable_daily()->set_snapshot(std::string(64, 'f'));
    EXPECT_THROW(data::minute_range_evidence(archive, tables, request), std::invalid_argument);
}

TEST(MinuteRange, OfficialClosuresCarryVersionAndSourcesWithoutGuessingOtherSessions)
{
    Folder folder;
    Minutes source;
    const auto record = publish(folder.path, source);
    const auto daily = test::seed_daily_table(
        folder.path,
        "SHFE/rb/2026-10",
        std::vector<std::string>{"2026-09-25"},
        100,
        2);
    history_files::Archive archive(folder.path / "history");
    history_files::TableStore tables(folder.path / "tables");
    auto request = range_query(record, daily, "2026-09-24", "2026-10-08");
    const auto result = data::minute_range_evidence(archive, tables, request);
    ASSERT_EQ(result.days_size(), 15);
    EXPECT_EQ(result.days(0).state(), data::proto::RANGE_UNKNOWN_CALENDAR);
    EXPECT_TRUE(result.days(0).has_evening_closure());
    EXPECT_FALSE(result.days(0).has_official_closure());
    EXPECT_EQ(result.days(1).state(), data::proto::RANGE_RULE_CONFLICT);
    EXPECT_EQ(result.days(2).state(), data::proto::RANGE_OFFICIAL_CLOSED);
    EXPECT_EQ(result.days(2).official_closure().published_day(), "2026-09-21");
    EXPECT_EQ(result.days(2).official_closure().version(), 1);
    EXPECT_NE(result.days(2).official_closure().source_url().find("833503"), std::string::npos);
    EXPECT_EQ(result.days(4).state(), data::proto::RANGE_UNKNOWN_CALENDAR);
    EXPECT_FALSE(result.days(4).has_evening_closure());
    EXPECT_TRUE(result.days(6).has_evening_closure());
    EXPECT_FALSE(result.days(6).has_official_closure());
    EXPECT_EQ(result.days(7).state(), data::proto::RANGE_OFFICIAL_CLOSED);
    EXPECT_EQ(result.days(13).state(), data::proto::RANGE_OFFICIAL_CLOSED);
    EXPECT_EQ(result.days(14).state(), data::proto::RANGE_UNKNOWN_CALENDAR);
    EXPECT_FALSE(result.days(14).has_evening_closure());
    const auto decoded = protocol::decode_minute_range_evidence(result);
    EXPECT_EQ(decoded.at("session_rules_version"), 1);
    EXPECT_TRUE(decoded.at("days").at(0).at("official_closure").is_null());
    request.set_session_rules_version(2);
    EXPECT_THROW(data::minute_range_evidence(archive, tables, request), std::invalid_argument);
}
