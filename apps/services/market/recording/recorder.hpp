#pragma once
#include <asterion/futures/intraday_bars.hpp>
#include <asterion/system/thread_pool.hpp>
#include <chrono>
#include <deque>
#include <filesystem>
#include <future>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace asterion::recording {

// Records every quote the feed delivers as Parquet, partitioned by the
// provider's trading day:
//
//   recordings/ticks/trading_day=<YYYYMMDD>/<instance>-<part>.parquet
//   recordings/minutes/trading_day=<YYYYMMDD>/<instance>-<part>.parquet
//   recordings/segments/trading_day=<YYYYMMDD>/<instance>.parquet
//
// Minutes are the bars the market service aggregates for strategies (the same
// code), written once a later bar or the end of their trading day completes
// them; a bar observation may have missed part of is marked partial, as is the
// bar forming at a stop.
// A segment is one stretch of uninterrupted observation by this process. It
// ends with the reason observation stopped; a segment without an end belongs
// to a process that stopped before it could say why. The time between two
// segments is a gap: nothing is filled in. A segment counts the quotes it
// observed, so fewer rows in its files means a write failed. Files are
// written on a worker. When four writes are behind, an interval's ticks are
// dropped and its segment ends with `write_backlog`: the market thread never
// waits for the disk. Minutes wait in memory until a write succeeds.
// Whether a trading day (YYYYMMDD) has ended at `now`: days before today in
// Beijing, and today from 15:30 Beijing time on. A night session belongs to
// the next trading day, so it never ends a day early.
bool trading_ended(const std::string& trading_day, std::chrono::system_clock::time_point now);

// Test hook: writes wait while held, as on a stalled disk.
void hold_writes_for_testing(bool hold);

class Recorder
{
public:
    struct Status
    {
        std::string trading_day, error;
        std::uint64_t rows = 0, files = 0, bytes = 0, lost = 0;
        std::int64_t written_ms = 0;
    };

    Recorder(std::filesystem::path root, std::string instance);
    ~Recorder();

    // Called before `minutes` observes the same quote.
    void observe(const MarketQuote& quote, bool out_of_order, const IntradayBars& minutes);
    // Ends the current segment if it observed anything.
    void interrupt(std::string_view reason);
    // Hands buffered quotes and completed minutes to the writer every
    // interval; collects results.
    void poll(std::int64_t now_ms, const IntradayBars& minutes);
    // Ends the segment, writes everything buffered and waits for the writer.
    void close(std::string_view reason, std::int64_t now_ms, const IntradayBars& minutes);

    const Status& status() const
    {
        return status_;
    }

    struct Row
    {
        std::uint64_t segment;
        bool out_of_order;
        MarketQuote quote;
    };

    struct Minute
    {
        InstrumentId instrument;
        MinuteBar bar;
    };

    struct Segment
    {
        std::uint64_t number;
        std::int64_t first_ms = 0, last_ms = 0;
        std::uint64_t ticks = 0;
        std::string end;
    };

private:
    struct Written
    {
        std::string trading_day, error;
        std::uint64_t rows = 0, bytes = 0, files = 0, lost = 0;
        // Minutes a failed write did not reach the disk with.
        std::vector<Minute> bars = {};
    };

    // What each instrument's minutes have reached in the files.
    struct MinuteMark
    {
        std::string trading_day;
        std::int64_t start_ms = 0;
        std::optional<MinuteBar> forming;
    };

    void take(const InstrumentId& id, const IntradaySeries& series);
    // Writes the last bar of every trading day that has ended at `now_ms`.
    void finish_ended_days(std::int64_t now_ms);
    void take_minutes(const IntradayBars& minutes, bool final);
    void flush(std::int64_t now_ms);
    // Waits for the oldest write and takes its result.
    void collect();

    const std::filesystem::path root_;
    const std::string instance_;
    std::vector<Row> rows_;
    std::map<std::string, std::vector<Minute>> minutes_;
    std::map<InstrumentId, MinuteMark> written_;
    std::uint64_t minutes_revision_ = 0;
    // Segments by trading day; a flush rewrites the days that changed.
    std::map<std::string, std::vector<Segment>> segments_;
    std::set<std::string> changed_;
    std::uint64_t segment_ = 1, part_ = 0;
    bool observed_ = false, due_ = false;
    std::int64_t flushed_ms_ = 0;
    Status status_;
    std::deque<std::future<Written>> pending_;
    ThreadPool writer_{1, 8};
};

} // namespace asterion::recording
