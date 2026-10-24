#include "history_tables.hpp"
#include "sqlite_database.hpp"
#include "table_parquet.hpp"
#include "table_update.hpp"
#include <asterion/protocol/data.hpp>
#include <asterion/kernel/process/child.hpp>
#include <fstream>
#include <asterion/kernel/process/artifact.hpp>
#include <gtest/gtest.h>
using namespace asterion;
using history_files::TableSeries;
using history_files::TableStore;

namespace {

struct Folder
{
    std::filesystem::path path =
        std::filesystem::temp_directory_path() / ("asterion-tables-" + unique_process_id());

    Folder()
    {
        std::filesystem::create_directory(path);
    }

    ~Folder()
    {
        std::filesystem::remove_all(path);
    }
};

const HistoryTable& daily = history_table("futures.daily");

// A daily row with only the cells a test cares about; the rest are missing.
TableSourceRow row(const char* contract, const char* close, const char* volume, const char* amount)
{
    TableSourceRow cells(daily.columns.size());
    cells[0] = contract;
    if (close)
        cells[4] = close;
    if (volume)
        cells[8] = volume;
    if (amount)
        cells[9] = amount;
    return cells;
}

std::string rejection(const std::vector<TableSourceRow>& rows, const char* exchange = "SHFE")
{
    try {
        (void)table_partition(daily, exchange, "2025-07-25", rows);
    } catch (const std::invalid_argument& error) {
        return error.what();
    }
    return {};
}

const TableSeries series{"futures.daily", 1, "fixture.daily.v1"};
const std::string artifact(64, 'a');

} // namespace

TEST(HistoryTables, KeepWhatTheSourcePublishedExactly)
{
    // A day's turnover above what an eight-place int64 holds, and a contract
    // that did not trade: both are ordinary rows of a real exchange day.
    const auto partition = table_partition(
        daily,
        "SHFE",
        "2025-07-25",
        {row("SHFE/rb/2025-10", "3356", "3501234.0", "117501413040.50"),
            row("SHFE/rb/2025-06", nullptr, "0", "0")});
    ASSERT_EQ(partition.rows.size(), 2U);
    EXPECT_EQ(std::get<std::string>(partition.rows[0][0]), "SHFE/rb/2025-06") << "ordered by key";
    EXPECT_TRUE(std::holds_alternative<std::monostate>(partition.rows[0][4]));
    EXPECT_EQ(table_cell_text(daily.columns[4], partition.rows[0][4]), std::nullopt);
    EXPECT_EQ(std::get<std::int64_t>(partition.rows[1][8]), 3501234);
    EXPECT_EQ(std::get<std::int64_t>(partition.rows[1][9]), 11750141304050);
    EXPECT_EQ(table_cell_text(daily.columns[9], partition.rows[1][9]), "117501413040.5");
    EXPECT_EQ(table_cell_text(daily.columns[4], partition.rows[1][4]), "3356");
    // The digest names the rows, not the order they arrived in.
    EXPECT_EQ(
        partition.content,
        table_partition(
            daily,
            "SHFE",
            "2025-07-25",
            {row("SHFE/rb/2025-06", nullptr, "0.00", "0"),
                row("SHFE/rb/2025-10", "3356.0", "3501234", "117501413040.5")})
            .content);
    EXPECT_NE(
        partition.content,
        table_partition(
            daily,
            "SHFE",
            "2025-07-28",
            {row("SHFE/rb/2025-10", "3356", "3501234.0", "117501413040.50"),
                row("SHFE/rb/2025-06", nullptr, "0", "0")})
            .content);
}

TEST(HistoryTables, RejectionsNameTheRowAndColumn)
{
    EXPECT_EQ(
        rejection(
            {row("SHFE/rb/2025-10", "3356", "1", "1"),
                row("SHFE/cu/2025-10", "78000.123456789", "1", "1")}),
        "futures.daily SHFE 2025-07-25: row 2 \"SHFE/cu/2025-10\" column close: " "\"78000.123456789\" has more than 8 decimal places");
    EXPECT_EQ(
        rejection({row("SHFE/rb/2025-10", "3356", "1.5", "1")}),
        "futures.daily SHFE 2025-07-25: row 1 \"SHFE/rb/2025-10\" column volume: " "\"1.5\" has more than 0 decimal places");
    EXPECT_EQ(
        rejection({row("SHFE/rb/2025-10", "3.356e3", "1", "1")}),
        "futures.daily SHFE 2025-07-25: row 1 \"SHFE/rb/2025-10\" column close: " "\"3.356e3\" is not a plain decimal number");
    EXPECT_EQ(
        rejection({row("SHFE/rb/2025-10", "1", "1", "12345678901234567.89")}),
        "futures.daily SHFE 2025-07-25: row 1 \"SHFE/rb/2025-10\" column amount: " "\"12345678901234567.89\" is out of range");
    EXPECT_EQ(
        rejection({row("DCE/i/2025-09", "1", "1", "1")}),
        "futures.daily SHFE 2025-07-25: row 1 \"DCE/i/2025-09\" column contract: " "\"DCE/i/2025-09\" is not a contract of SHFE");
    EXPECT_EQ(
        rejection({row("SHFE/rb/2025-10", "1", "1", "1"), row("SHFE/rb/2025-10", "2", "1", "1")}),
        "futures.daily SHFE 2025-07-25: key \"SHFE/rb/2025-10\" appears more than once");
    TableSourceRow keyless(daily.columns.size());
    EXPECT_EQ(
        rejection({keyless}),
        "futures.daily SHFE 2025-07-25: row 1 column contract is missing");
    EXPECT_EQ(
        rejection({{std::string("SHFE/rb/2025-10")}}),
        "futures.daily SHFE 2025-07-25: row 1 \"SHFE/rb/2025-10\" has 1 cells, expected 13");
    EXPECT_THROW((void)table_partition(daily, "NYMEX", "2025-07-25", {}), std::invalid_argument);
    EXPECT_THROW((void)table_partition(daily, "SHFE", "2025", {}), std::invalid_argument);
    // A year of the calendar holds that year's days only.
    const auto& calendar = history_table("futures.calendar");
    EXPECT_NO_THROW((void)table_partition(
        calendar,
        "SHFE",
        "2026",
        {{std::string("2026-10-08"), std::string("1"), std::string("2026-09-30")}}));
    EXPECT_THROW(
        (void)table_partition(
            calendar,
            "SHFE",
            "2026",
            {{std::string("2025-12-31"), std::string("1"), std::nullopt}}),
        std::invalid_argument);
    EXPECT_THROW(
        (void)table_partition(
            calendar,
            "SHFE",
            "2026",
            {{std::string("2026-10-08"), std::string("yes"), std::nullopt}}),
        std::invalid_argument);
}

TEST(HistoryTables, APartitionReadsBackFromItsFileAsItWasWritten)
{
    Folder folder;
    const auto& contracts = history_table("futures.contracts");
    TableSourceRow listing(contracts.columns.size());
    listing[0] = "CFFEX/if/2026-11";
    listing[1] = "IF2611.CFX";
    listing[2] = "IF2611";
    listing[3] = "2026-09-21";
    listing[4] = "2026-11-20";
    listing[6] = "300";
    listing[10] = "0.2指数点";
    const auto partition = table_partition(contracts, "CFFEX", "all", {listing});
    const auto file = folder.path / "contracts.parquet";
    parquet::write_table(file, partition);
    EXPECT_EQ(parquet::read_table(file, contracts), partition.rows);
    const auto bars = table_partition(
        daily,
        "SHFE",
        "2025-07-25",
        {row("SHFE/rb/2025-10", "3356.5", "3501234", "117501413040.5"),
            row("SHFE/rb/2025-06", nullptr, nullptr, nullptr)});
    parquet::write_table(folder.path / "daily.parquet", bars);
    EXPECT_EQ(parquet::read_table(folder.path / "daily.parquet", daily), bars.rows);
    EXPECT_THROW(parquet::read_table(file, daily), std::invalid_argument) << "another table's file";
}

TEST(HistoryTableStore, ARevisionNeverReplacesWhatAnEarlierSnapshotHeld)
{
    Folder folder;
    const auto first = table_partition(
        daily,
        "SHFE",
        "2025-07-25",
        {row("SHFE/rb/2025-10", "3356", "100", "3356000")});
    const auto corrected = table_partition(
        daily,
        "SHFE",
        "2025-07-25",
        {row("SHFE/rb/2025-10", "3357", "100", "3357000")});
    const auto next = table_partition(
        daily,
        "SHFE",
        "2025-07-28",
        {row("SHFE/rb/2025-10", "3360", "90", "3024000")});
    std::string snapshot;
    history_files::TableRevision original;
    {
        TableStore store(folder.path / "tables");
        EXPECT_EQ(store.head(), "");
        EXPECT_FALSE(store.current(series, "SHFE", "2025-07-25"));
        const auto recorded = store.record(first, series, store.write(first), artifact, 1000);
        EXPECT_FALSE(recorded.unchanged);
        EXPECT_EQ(recorded.revision.sequence, 1U);
        original = recorded.revision;
        snapshot = store.head();
        EXPECT_EQ(snapshot, original.snapshot);
        // The same content again is the same revision: nothing is appended.
        const auto again = store.record(first, series, store.write(first), artifact, 2000);
        EXPECT_TRUE(again.unchanged);
        EXPECT_EQ(again.revision.sequence, 1U);
        EXPECT_EQ(again.revision.acquired_at_ns, 1000);
        EXPECT_EQ(store.head(), snapshot);
        EXPECT_EQ(
            store.record(next, series, store.write(next), artifact, 3000).revision.sequence,
            2U);
        const auto revised =
            store.record(corrected, series, store.write(corrected), artifact, 4000);
        EXPECT_FALSE(revised.unchanged);
        EXPECT_EQ(revised.revision.sequence, 3U);
        EXPECT_NE(store.head(), snapshot);
    }
    // Another process later: the ledger and every revision's rows are still there.
    TableStore store(folder.path / "tables");
    const auto current = store.current(series, "SHFE", "2025-07-25");
    ASSERT_TRUE(current);
    EXPECT_EQ(current->sequence, 3U);
    EXPECT_EQ(store.rows(*current), corrected.rows);
    EXPECT_EQ(store.rows(original), first.rows) << "the superseded revision stays readable";
    EXPECT_EQ(
        store.periods(series, "SHFE", "2025-07-01", "2025-07-31"),
        (std::vector<std::string>{"2025-07-25", "2025-07-28"}));
    EXPECT_TRUE(store.periods(series, "DCE", "2025-07-01", "2025-07-31").empty());
    const auto coverage = store.coverage();
    ASSERT_EQ(coverage.size(), 1U);
    EXPECT_EQ(coverage[0].series, series);
    EXPECT_EQ(coverage[0].exchange, "SHFE");
    EXPECT_EQ(coverage[0].partitions, 2U);
    EXPECT_EQ(coverage[0].rows, 2U);
    EXPECT_EQ(coverage[0].first_period, "2025-07-25");
    EXPECT_EQ(coverage[0].last_period, "2025-07-28");
    EXPECT_EQ(coverage[0].acquired_at_ns, 4000);
    // A snapshot keeps holding what it held: the first one has one day, as first read.
    ASSERT_EQ(store.sequence(snapshot), 1U);
    EXPECT_FALSE(store.sequence(std::string(64, 'a'))) << "never a snapshot of this ledger";
    const auto then = store.revisions(series, "SHFE", "2025-07-01", "2025-07-31", 1);
    ASSERT_EQ(then.size(), 1U);
    EXPECT_EQ(store.rows(then[0]), first.rows);
    const auto now = store.revisions(series, "SHFE", "2025-07-01", "2025-07-31", 3);
    ASSERT_EQ(now.size(), 2U);
    EXPECT_EQ(store.rows(now[0]), corrected.rows);
    EXPECT_EQ(now[1].period, "2025-07-28");
    EXPECT_TRUE(store.revisions(series, "SHFE", "2025-07-26", "2025-07-27", 3).empty());
    // Both days read together: the closes of one product, each with its day.
    std::map<std::string, std::string> closes;
    const std::vector<std::size_t> wanted{
        table_column(daily, "contract"),
        table_column(daily, "close")
    };
    const std::vector<std::string> products{"SHFE/rb/"}, others{"SHFE/cu/"};
    store.scan(now, wanted, products, [&](std::size_t revision, std::vector<TableCell>& cells) {
        EXPECT_EQ(std::get<std::string>(cells[0]), "SHFE/rb/2025-10");
        closes[now[revision].period] = *table_cell_text(daily.columns[wanted[1]], cells[1]);
    });
    EXPECT_EQ(
        closes,
        (std::map<std::string, std::string>{{"2025-07-25", "3357"}, {"2025-07-28", "3360"}}));
    store.scan(now, wanted, others, [](std::size_t, std::vector<TableCell>&) {
        FAIL() << "no row is of another product";
    });
    // A file that no longer has the recorded bytes is refused, not read.
    const auto object = folder.path / "tables" / "objects" / current->object.substr(0, 2)
        / (current->object + ".parquet");
    std::ofstream(object, std::ios::app) << "x";
    EXPECT_THROW(store.rows(*current), std::invalid_argument);
    EXPECT_THROW(
        store.current({"futures.daily", 2, "fixture.daily.v1"}, "SHFE", "2025-07-25"),
        std::invalid_argument);
}

TEST(HistoryTableStore, RefusesALedgerItDidNotWrite)
{
    Folder folder;
    std::filesystem::create_directories(folder.path / "tables");
    {
        sqlite::Database foreign(folder.path / "tables" / "ledger.sqlite");
        foreign.execute("CREATE TABLE something(value INTEGER)");
    }
    EXPECT_THROW(TableStore(folder.path / "tables"), std::invalid_argument);
    EXPECT_THROW(TableStore("relative"), std::invalid_argument);
}

TEST(RawResponseStore, NewRevisionBindsExactBytesAndOldOrUnchangedPartitionsAreNotBackfilled)
{
    Folder folder;
    const TableSeries series{"futures.daily", daily.schema, "test.raw.daily.v1"};
    history_files::TableRevision old, held;
    const SourceResponse captured{
        "tushare.fut_daily.raw.v1",
        "{ \"code\":0 }\n",
        1790000000000000000LL
    };
    {
        TableStore store(folder.path);
        const auto first =
            table_partition(daily, "SHFE", "2025-07-25", {row("SHFE/rb/2025-10", "100", "1", "1")});
        old = store.record(first, series, store.write(first), std::string(64, 'a'), 100).revision;
        EXPECT_FALSE(store.response(old));
        EXPECT_TRUE(
            store.record(first, series, store.write(first), std::string(64, 'a'), 101, captured)
                .unchanged);
        EXPECT_FALSE(store.response(old));
        const auto changed =
            table_partition(daily, "SHFE", "2025-07-25", {row("SHFE/rb/2025-10", "101", "1", "1")});
        held =
            store.record(changed, series, store.write(changed), std::string(64, 'a'), 102, captured)
                .revision;
        ASSERT_TRUE(store.response(held));
        EXPECT_EQ(store.response(held)->body, captured.body);
        EXPECT_NE(held.snapshot, old.snapshot);
        EXPECT_FALSE(store.response(old));
    }
    TableStore reopened(folder.path);
    EXPECT_EQ(reopened.response(held)->body, captured.body);
    EXPECT_FALSE(reopened.response(old));
    const auto object =
        folder.path / "responses" / "objects" / (sha256_bytes(captured.body) + ".json");
    {
        std::ofstream output(object, std::ios::app);
        output << " ";
    }
    EXPECT_THROW(reopened.response(held), std::invalid_argument);
    {
        std::ofstream output(object, std::ios::trunc);
        output << captured.body;
    }
    std::filesystem::remove(folder.path / "responses" / "revisions" / (held.snapshot + ".json"));
    EXPECT_THROW(reopened.response(held), std::invalid_argument);
}

TEST(ResponseReview, PageQueryChecksOffPageRowsAndLeavesEveryStoredByteAndHeadUnchanged)
{
    Folder folder;
    TableStore store(folder.path);
    const TableSeries series{"futures.daily", daily.schema, "test.quality.daily.v1"};
    const auto partition = table_partition(
        daily,
        "SHFE",
        "2025-07-25",
        {row("SHFE/cu/2025-10", "100", "1", "1"), row("SHFE/rb/2025-10", "101", "1", "1")});
    const SourceResponse response{
        "tushare.fut_daily.raw.v1",
        R"({"code":0,"data":{"fields":["ts_code","trade_date","pre_close","pre_settle","open","high","low","close","settle","vol","amount","oi","oi_chg","delv_settle"],"items":[["CU2510.SHF","20250725",null,null,null,null,null,100,null,1,0.0001,null,null,null],["RB2510.SHF","20250725",null,null,null,null,null,100,null,1,0.0001,null,null,null]]}})",
        1
    };
    const auto object = store.write(partition);
    store.record(partition, series, object, std::string(64, 'a'), 1, response);
    const TableSeries old_series{"futures.daily", daily.schema, "test.quality.old.v1"};
    store.record(partition, old_series, object, std::string(64, 'a'), 1);
    const auto before = [&] {
        std::map<std::string, std::string> hashes;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(folder.path)) {
            if (!entry.is_regular_file())
                continue;
            std::ifstream file(entry.path(), std::ios::binary);
            hashes.emplace(
                entry.path().lexically_relative(folder.path).string(),
                sha256_bytes(std::string(std::istreambuf_iterator<char>(file), {})));
        }
        return hashes;
    };
    const auto hashes = before();
    const auto head = store.head();
    data::proto::TablePageQuery query;
    query.mutable_partition()->set_table("futures.daily");
    query.mutable_partition()->set_exchange("SHFE");
    query.mutable_partition()->set_period("2025-07-25");
    query.set_source(series.source);
    query.set_limit(1);
    const auto page = data::table_page(store, query);
    ASSERT_EQ(page.rows_size(), 1);
    EXPECT_EQ(page.rows(0).cells(0).text(), "SHFE/cu/2025-10");
    const auto json = protocol::decode_table_page(page);
    const auto& review = json.at("response_review");
    EXPECT_EQ(review.at("status"), "differences");
    EXPECT_EQ(review.at("normalized_rows"), 2);
    EXPECT_EQ(review.at("matched_rows"), 2);
    EXPECT_EQ(review.at("compared_cells"), 24);
    ASSERT_EQ(review.at("differences").size(), 1);
    EXPECT_EQ(
        review.at("differences")[0],
        (Json{
            {"kind", "value_mismatch"},
            {"key", "RB2510.SHF"},
            {"field", "close"},
            {"raw", "100"},
            {"expected", "100"},
            {"actual", "101"}
        }));
    query.set_source(old_series.source);
    EXPECT_EQ(data::table_page(store, query).response_review().reason(), "raw_missing");
    EXPECT_EQ(store.head(), head);
    EXPECT_EQ(before(), hashes);
}

namespace {

SourceResponse observation_body(const char* close, std::int64_t received)
{
    return {
        "tushare.fut_daily.raw.v1",
        std::string(
            R"({"code":0,"data":{"fields":["ts_code","trade_date","pre_close","pre_settle","open","high","low","close","settle","vol","amount","oi","oi_chg","delv_settle"],"items":[["RB2510.SHF","20250725",null,null,null,null,null,)")
            + close + ",null,1,0.0001,null,null,null]]}}",
        received
    };
}

} // namespace

TEST(RefreshObservation, SameContentHasSeparateImmutableEvidenceAndReplaySurvivesLaterRevision)
{
    Folder folder;
    auto owner = std::make_unique<TableStore>(folder.path);
    auto& store = *owner;
    const TableSeries series{"futures.daily", daily.schema, "tushare.fut_daily.v3"};
    const auto partition =
        table_partition(daily, "SHFE", "2025-07-25", {row("SHFE/rb/2025-10", "100", "1", "1")});
    const auto old =
        store.record(partition, series, store.write(partition), std::string(64, 'a'), 1).revision;
    const auto old_file =
        folder.path / "objects" / old.object.substr(0, 2) / (old.object + ".parquet");
    const auto old_bytes = sha256_file(old_file);
    history_files::TableObservationContext context{
        "data",
        "tasks",
        "same",
        1,
        "fixture",
        std::string(64, 'b'),
        old.snapshot,
        old.content
    };
    const auto raw = observation_body("100", 10);
    const auto first = store.record_refresh_observation(context, old, raw, 20, true);
    EXPECT_EQ(store.head(), old.snapshot);
    EXPECT_FALSE(store.response(old));
    data::proto::TablePageQuery query;
    query.mutable_partition()->set_table("futures.daily");
    query.mutable_partition()->set_exchange("SHFE");
    query.mutable_partition()->set_period("2025-07-25");
    query.set_source(series.source);
    query.set_limit(1);
    const auto page = data::table_page(store, query);
    EXPECT_FALSE(page.has_response_evidence());
    EXPECT_EQ(page.response_review().reason(), "raw_missing");
    EXPECT_EQ(page.latest_refresh_observation().id(), first.id);
    EXPECT_EQ(first.response.received_at_ns, 10);
    EXPECT_EQ(first.recorded_at_ns, 20);
    const auto replay = store.record_refresh_observation(context, old, raw, 30, false);
    EXPECT_EQ(replay.id, first.id);
    EXPECT_EQ(replay.recorded_at_ns, 20);
    EXPECT_TRUE(replay.unchanged);
    auto next_context = context;
    next_context.task_id = "same-again";
    const auto second =
        store.record_refresh_observation(next_context, old, observation_body("100", 40), 50, true);
    EXPECT_NE(second.id, first.id);
    EXPECT_EQ(store.latest_refresh_observation(series, "SHFE", "2025-07-25")->id, second.id);
    const auto changed =
        table_partition(daily, "SHFE", "2025-07-25", {row("SHFE/rb/2025-10", "101", "1", "1")});
    const auto revision = store
                              .record(
                                  changed,
                                  series,
                                  store.write(changed),
                                  std::string(64, 'b'),
                                  60,
                                  observation_body("101", 55))
                              .revision;
    auto changed_context = context;
    changed_context.task_id = "changed";
    const auto third = store.record_refresh_observation(
        changed_context,
        revision,
        observation_body("101", 55),
        65,
        false);
    EXPECT_NE(third.id, first.id);
    EXPECT_FALSE(third.unchanged);
    EXPECT_EQ(third.context.selected_snapshot, old.snapshot);
    EXPECT_NE(third.revision.content, third.context.selected_content);
    EXPECT_EQ(store.refresh_observation(context, series, "SHFE", "2025-07-25")->id, first.id);
    EXPECT_EQ(store.record_refresh_observation(context, old, raw, 70, true).id, first.id);
    EXPECT_EQ(store.head(), revision.snapshot);
    EXPECT_EQ(store.latest_refresh_observation(series, "SHFE", "2025-07-25")->id, third.id);
    EXPECT_EQ(store.rows(old), partition.rows);
    EXPECT_EQ(sha256_file(old_file), old_bytes);
    EXPECT_FALSE(store.response(old));
    owner.reset(); // release the single-writer lock before reopening
    TableStore reopened(folder.path);
    EXPECT_EQ(reopened.latest_refresh_observation(series, "SHFE", "2025-07-25")->id, third.id);
    EXPECT_EQ(reopened.refresh_observation(context, series, "SHFE", "2025-07-25")->id, first.id);
}

TEST(RefreshObservation, ConflictingReplayAndCorruptObjectAreRejectedWithoutChangingRevision)
{
    Folder folder;
    TableStore store(folder.path);
    const TableSeries series{"futures.daily", daily.schema, "tushare.fut_daily.v3"};
    const auto partition =
        table_partition(daily, "SHFE", "2025-07-25", {row("SHFE/rb/2025-10", "100", "1", "1")});
    const auto old =
        store.record(partition, series, store.write(partition), std::string(64, 'a'), 1).revision;
    const history_files::TableObservationContext context{
        "data",
        "tasks",
        "one",
        1,
        "fixture",
        std::string(64, 'b'),
        old.snapshot,
        old.content
    };
    const auto held =
        store.record_refresh_observation(context, old, observation_body("100", 10), 20, true);
    EXPECT_THROW(
        store.record_refresh_observation(context, old, observation_body("101", 10), 30, true),
        std::invalid_argument);
    EXPECT_EQ(store.refresh_observation(context, series, "SHFE", "2025-07-25")->id, held.id);
    EXPECT_EQ(store.head(), old.snapshot);
    const auto object = folder.path / "observations" / "objects" / (held.id + ".json");
    std::ofstream(object, std::ios::app) << " ";
    EXPECT_THROW(
        store.refresh_observation(context, series, "SHFE", "2025-07-25"),
        std::invalid_argument);
    EXPECT_THROW(
        store.latest_refresh_observation(series, "SHFE", "2025-07-25"),
        std::invalid_argument);
    EXPECT_EQ(store.head(), old.snapshot);
}

TEST(HistoryTableStore, ColdInspectionVerifiesHistoryWithoutWritingOrPublishing)
{
    Folder folder;
    const auto root = folder.path / "tables";
    const auto partition = table_partition(
        daily,
        "SHFE",
        "2025-07-25",
        {row("SHFE/rb/2025-10", "3356", "100", "3356000")});
    history_files::TableRevision fixed;
    TableStore::Object object;
    {
        TableStore writer(root);
        object = writer.write(partition);
        fixed = writer.record(partition, series, object, artifact, 1000).revision;
    }
    const auto before = sha256_file(root / "ledger.sqlite");
    {
        TableStore reader(root, TableStore::Access::read_only);
        EXPECT_EQ(reader.head(), fixed.snapshot);
        EXPECT_NO_THROW(reader.verify_history());
        EXPECT_EQ(reader.rows(fixed).size(), 1U);
        EXPECT_THROW(reader.write(partition), std::logic_error);
        EXPECT_THROW(reader.record(partition, series, object, artifact, 2000), std::logic_error);
        EXPECT_THROW(
            reader.record_refresh_observation({}, fixed, {}, 2000, true),
            std::logic_error);
    }
    EXPECT_EQ(sha256_file(root / "ledger.sqlite"), before);
    EXPECT_FALSE(
        std::filesystem::exists(root / "objects" / ("incoming-" + partition.content + ".parquet")));
    const auto absent = folder.path / "never-created";
    EXPECT_THROW(TableStore missing(absent, TableStore::Access::read_only), std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(absent));
    {
        sqlite::Database database(root / "ledger.sqlite");
        sqlite::Database::Statement corrupt(
            database,
            "UPDATE table_revisions SET snapshot=? WHERE sequence=1");
        corrupt.bind(1, std::string(64, 'b')).step();
    }
    TableStore corrupted(root, TableStore::Access::read_only);
    EXPECT_THROW(corrupted.verify_history(), std::invalid_argument);
}

TEST(TableRetirement, SharedPrefixesKeepObjectsAndInterruptedCleanupResumesFromEvidence)
{
    Folder folder;
    TableStore::Retirement decision;
    std::string first, middle, head, object;
    const auto a =
        table_partition(daily, "SHFE", "2025-07-25", {row("SHFE/rb/2025-10", "100", "1", "1")});
    const auto b =
        table_partition(daily, "SHFE", "2025-07-28", {row("SHFE/rb/2025-10", "200", "1", "1")});
    const auto c =
        table_partition(daily, "SHFE", "2025-07-25", {row("SHFE/rb/2025-10", "300", "1", "1")});
    {
        TableStore store(folder.path);
        const auto old = store.write(a);
        object = old.sha256;
        first = store.record(a, series, old, artifact, 1000).revision.snapshot;
        const auto second = store.write(b);
        middle = store.record(b, series, second, artifact, 2000).revision.snapshot;
        head = store.record(c, series, store.write(c), artifact, 3000).revision.snapshot;
        EXPECT_THROW(store.preview_retirement(head), Error);
        const auto shared = store.preview_retirement(first);
        EXPECT_TRUE(shared.objects.empty());
        store.begin_retirement(shared);
        store.clean_retirement(first);
        store.finish_retirement(first);
        EXPECT_FALSE(store.sequence(first));
        EXPECT_TRUE(
            std::filesystem::exists(
                folder.path / "objects" / object.substr(0, 2) / (object + ".parquet")));
        decision = store.preview_retirement(middle);
        ASSERT_EQ(decision.objects, std::vector<std::string>{object});
        store.begin_retirement(decision);
        EXPECT_THROW(store.revisions(series, "SHFE", "2025-07-25", "2025-07-28", 2), Error);
        EXPECT_THROW(store.write(a), Error);
        std::stop_source stop;
        stop.request_stop();
        EXPECT_THROW(store.clean_retirement(middle, stop.get_token()), Error);
        EXPECT_FALSE(store.retirement(middle)->complete);
    }
    TableStore restored(folder.path);
    EXPECT_FALSE(restored.sequence(middle));
    EXPECT_EQ(restored.head(), head);
    EXPECT_EQ(restored.retirement(middle)->objects, decision.objects);
    restored.clean_retirement(middle);
    restored.finish_retirement(middle);
    EXPECT_TRUE(restored.retirement(middle)->complete);
    EXPECT_EQ(restored.retirement_bytes(decision), 0U);
    EXPECT_FALSE(
        std::filesystem::exists(
            folder.path / "objects" / object.substr(0, 2) / (object + ".parquet")));
    EXPECT_NO_THROW(restored.verify_history());
    EXPECT_EQ(restored.revisions(series, "SHFE", "2025-07-25", "2025-07-28", 3).size(), 2U);
    // Same bytes can be a new, explicitly published revision after completed cleanup.
    const auto again = restored.record(a, series, restored.write(a), artifact, 4000).revision;
    EXPECT_EQ(again.object, object);
    EXPECT_NE(again.snapshot, first);
    EXPECT_NO_THROW(restored.verify_history());
    EXPECT_FALSE(restored.sequence(first));
}

TEST(TableRetirement, MissingAuthorityOrUnknownPayloadDoesNotPermitCleanup)
{
    Folder folder;
    std::string first, object;
    {
        TableStore store(folder.path);
        const auto a =
            table_partition(daily, "SHFE", "2025-07-25", {row("SHFE/rb/2025-10", "100", "1", "1")});
        const auto b =
            table_partition(daily, "SHFE", "2025-07-25", {row("SHFE/rb/2025-10", "200", "1", "1")});
        const auto file = store.write(a);
        object = file.sha256;
        first = store.record(a, series, file, artifact, 1000).revision.snapshot;
        store.record(b, series, store.write(b), artifact, 2000);
        const auto path = folder.path / "objects" / object.substr(0, 2) / (object + ".parquet");
        const auto saved = folder.path / "isolated-original.parquet";
        std::filesystem::rename(path, saved);
        EXPECT_THROW(store.preview_retirement(first), std::invalid_argument);
        EXPECT_FALSE(store.retirement(first));
        std::filesystem::rename(saved, path);
        store.begin_retirement(store.preview_retirement(first));
    }
    std::filesystem::rename(
        folder.path / "retirements.sqlite",
        folder.path / "preserved-retirements.sqlite");
    EXPECT_THROW(TableStore unavailable(folder.path), std::runtime_error);
    EXPECT_TRUE(
        std::filesystem::exists(
            folder.path / "objects" / object.substr(0, 2) / (object + ".parquet")));
    std::filesystem::rename(
        folder.path / "preserved-retirements.sqlite",
        folder.path / "retirements.sqlite");
    std::filesystem::rename(folder.path / "retirements.required", folder.path / "preserved-guard");
    EXPECT_THROW(TableStore unavailable(folder.path), std::runtime_error);
}
