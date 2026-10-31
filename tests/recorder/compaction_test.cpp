#include "compaction.hpp"
#include "parquet_engine.hpp"
#include "recorder.hpp"
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
