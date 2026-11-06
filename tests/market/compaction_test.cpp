#include "recorded/compaction.hpp"
#include "parquet_engine.hpp"
#include "recording/recorder.hpp"
#include <asterion/system/durable_file.hpp>
#include <asterion/system/process/child.hpp>
#include <gtest/gtest.h>

using namespace asterion;
using namespace std::chrono;
namespace fs = std::filesystem;

namespace {

constexpr std::int64_t base = 1'793'164'800'000; // 2026-10-28 13:20 Beijing, a minute boundary.

struct Folder
{
    fs::path path = fs::temp_directory_path() / ("ast-merge-" + unique_process_id());

    ~Folder()
    {
        fs::remove_all(path);
    }
};

MarketQuote quote(const char* venue, const char* symbol, std::int64_t at, const char* last)
{
    MarketQuote q;
    q.instrument = {venue, symbol};
    q.trading_day = "20261028";
    q.action_day = "20261028";
    q.update_time = "13:20:00";
    q.source_ms = at;
    q.received_ms = at + 5;
    q.last = Decimal::parse(last);
    q.volume = 10;
    return q;
}

// One instance's recording: a quote per contract per second for `seconds`.
void record(const fs::path& root, const std::string& instance, std::int64_t from, int seconds)
{
    recording::Recorder recorder(root, instance);
    IntradayBars minutes;
    for (int i = 0; i < seconds; ++i)
        for (const auto& [venue, symbol] :
            {std::pair{"SHFE", "rb2701"}, std::pair{"DCE", "i2701"}}) {
            const auto q = quote(venue, symbol, from + i * 1000, "3100");
            recorder.observe(q, false, minutes);
            minutes.observe(q);
        }
    recorder.close("stopped", from + seconds * 1000, minutes);
    for (const auto& entry : fs::recursive_directory_iterator(root))
        if (entry.is_regular_file())
            fs::last_write_time(
                entry.path(),
                file_clock::from_sys(system_clock::time_point(milliseconds(from))));
}

std::vector<std::string> names(const fs::path& folder)
{
    std::vector<std::string> out;
    for (const auto& entry : fs::directory_iterator(folder))
        out.push_back(entry.path().filename().string());
    std::sort(out.begin(), out.end());
    return out;
}

std::int64_t rows(const fs::path& folder)
{
    parquet::Engine engine;
    return engine
        .run("SELECT count(*) FROM read_parquet(" + parquet::literal(folder / "*.parquet") + ")")
        ->GetValue(0, 0)
        .GetValue<std::int64_t>();
}

system_clock::time_point beijing(int day, int hour, int minute)
{
    return sys_days{2026y / 10 / day} + hours(hour - 8) + minutes(minute);
}

} // namespace

TEST(Merge, ADayIsMergedOnlyAfterItsTradingEnded)
{
    Folder folder;
    record(folder.path, "a", base, 3);
    EXPECT_TRUE(recording::compactable_days(folder.path, beijing(28, 15, 29)).empty());
    EXPECT_EQ(
        recording::compactable_days(folder.path, beijing(28, 15, 30)),
        std::vector<std::string>{"20261028"});
    EXPECT_EQ(
        recording::compactable_days(folder.path, beijing(29, 3, 0)),
        std::vector<std::string>{"20261028"});
    // A night session's trading day is tomorrow: never early.
    EXPECT_TRUE(recording::compactable_days(folder.path, beijing(27, 22, 0)).empty());
}

TEST(Merge, ADayWaitsUntilItsFilesHaveBeenQuietForHalfAnHour)
{
    Folder folder;
    record(folder.path, "a", base, 3);
    const auto file = *fs::directory_iterator(folder.path / "ticks/trading_day=20261028");
    fs::last_write_time(file.path(), file_clock::from_sys(beijing(28, 16, 0)));
    EXPECT_TRUE(recording::compactable_days(folder.path, beijing(28, 16, 29)).empty());
    EXPECT_EQ(
        recording::compactable_days(folder.path, beijing(28, 16, 30)),
        std::vector<std::string>{"20261028"});
}

TEST(Merge, FilesBecomeOnePerExchangeWithEveryRowAndALateQuoteIsFoldedIn)
{
    Folder folder;
    record(folder.path, "a", base, 70);
    record(folder.path, "b", base + 70'000, 70);
    const auto ticks = folder.path / "ticks/trading_day=20261028";
    const auto minutes = folder.path / "minutes/trading_day=20261028";
    const auto tick_rows = rows(ticks), minute_rows = rows(minutes);
    const auto merged = recording::compact_day(folder.path, "20261028");
    EXPECT_EQ(merged.ticks, static_cast<std::uint64_t>(tick_rows));
    EXPECT_EQ(merged.minutes, static_cast<std::uint64_t>(minute_rows));
    EXPECT_EQ(names(ticks), (std::vector<std::string>{"DCE.parquet", "SHFE.parquet"}));
    EXPECT_EQ(names(minutes), (std::vector<std::string>{"DCE.parquet", "SHFE.parquet"}));
    EXPECT_EQ(rows(ticks), tick_rows);
    EXPECT_TRUE(recording::compactable_days(folder.path, beijing(29, 3, 0)).empty());
    // Segments are kept as written.
    EXPECT_EQ(names(folder.path / "segments/trading_day=20261028").size(), 2u);

    record(folder.path, "c", base + 200'000, 2);
    EXPECT_EQ(
        recording::compactable_days(folder.path, beijing(29, 3, 0)),
        std::vector<std::string>{"20261028"});
    recording::compact_day(folder.path, "20261028");
    EXPECT_EQ(names(ticks), (std::vector<std::string>{"DCE.parquet", "SHFE.parquet"}));
    EXPECT_EQ(rows(ticks), tick_rows + 4);
}

TEST(Merge, AMergeStoppedAfterItsJournalIsCompletedAndOneStoppedBeforeLeavesTheOriginals)
{
    Folder folder;
    record(folder.path, "a", base, 3);
    const auto ticks = folder.path / "ticks/trading_day=20261028";
    // Stopped before the journal: a stray merged file is ignored and replaced.
    write_file_durably(ticks / "SHFE.parquet.merging", "partial");
    recording::finish_compactions(folder.path);
    EXPECT_EQ(rows(ticks), 6);
    recording::compact_day(folder.path, "20261028");
    EXPECT_EQ(rows(ticks), 6);

    // Stopped after the journal: the next start deletes the originals it names
    // and gives the merged file its name.
    Folder second;
    record(second.path, "a", base, 3);
    const auto folder2 = second.path / "ticks/trading_day=20261028";
    std::string journal = R"({"remove":[)";
    for (const auto& name : names(folder2))
        journal += (journal.back() == '[' ? "\"" : ",\"") + name + "\"";
    journal += R"(],"merged":["SHFE"]})";
    {
        parquet::Engine engine;
        engine.run(
            "COPY (SELECT * FROM read_parquet(" + parquet::literal(folder2 / "*.parquet")
            + ") WHERE venue = 'SHFE') TO " + parquet::literal(folder2 / "SHFE.parquet.merging")
            + " (FORMAT parquet)");
    }
    write_file_durably(folder2 / "compaction.json", journal);
    recording::finish_compactions(second.path);
    EXPECT_EQ(names(folder2), std::vector<std::string>{"SHFE.parquet"});
    EXPECT_EQ(rows(folder2), 3);
}

TEST(Merge, ASecondMergeStoppedAfterItsRenameKeepsTheMergedFile)
{
    Folder folder;
    record(folder.path, "a", base, 3);
    recording::compact_day(folder.path, "20261028");
    record(folder.path, "late", base + 200'000, 2);
    const auto ticks = folder.path / "ticks/trading_day=20261028";
    const auto total = rows(ticks);
    // The state a second merge leaves when it stops after renaming: the merged
    // files hold every row, the originals are gone and the journal names them all.
    const auto originals = names(ticks);
    {
        parquet::Engine engine;
        for (const auto* venue : {"DCE", "SHFE"})
            engine.run(
                "COPY (SELECT * FROM read_parquet(" + parquet::literal(ticks / "*.parquet")
                + ") WHERE venue = '" + venue + "') TO "
                + parquet::literal(ticks / (std::string(venue) + ".parquet.merging"))
                + " (FORMAT parquet)");
    }
    std::string journal = R"({"remove":[)";
    for (const auto& name : originals) {
        journal += (journal.back() == '[' ? "\"" : ",\"") + name + "\"";
        fs::remove(ticks / name);
    }
    journal += R"(],"merged":["DCE","SHFE"]})";
    write_file_durably(ticks / "compaction.json", journal);
    for (const auto* venue : {"DCE", "SHFE"})
        fs::rename(
            ticks / (std::string(venue) + ".parquet.merging"),
            ticks / (std::string(venue) + ".parquet"));

    recording::finish_compactions(folder.path);
    EXPECT_EQ(names(ticks), (std::vector<std::string>{"DCE.parquet", "SHFE.parquet"}));
    EXPECT_EQ(rows(ticks), total);
}

TEST(Merge, OnlyTheLatestTickDaysAreKeptAndMinutesAndSegmentsStay)
{
    Folder folder;
    for (const auto* day : {"20261026", "20261027", "20261028"})
        for (const auto* table : {"ticks", "minutes", "segments"}) {
            const auto partition = folder.path / table / (std::string("trading_day=") + day);
            fs::create_directories(partition);
            write_file_durably(partition / "SHFE.parquet", "rows");
        }
    EXPECT_TRUE(recording::prune_ticks(folder.path, 3).empty());
    EXPECT_EQ(recording::prune_ticks(folder.path, 2), std::vector<std::string>{"20261026"});
    EXPECT_FALSE(fs::exists(folder.path / "ticks/trading_day=20261026"));
    EXPECT_TRUE(fs::exists(folder.path / "ticks/trading_day=20261027/SHFE.parquet"));
    // A day whose merge keeps failing keeps its ticks.
    EXPECT_TRUE(recording::prune_ticks(folder.path, 1, {"20261027"}).empty());
    EXPECT_TRUE(fs::exists(folder.path / "ticks/trading_day=20261027/SHFE.parquet"));
    for (const auto* table : {"minutes", "segments"})
        EXPECT_TRUE(fs::exists(folder.path / table / "trading_day=20261026/SHFE.parquet"));
}
