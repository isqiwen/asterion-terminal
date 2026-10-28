#pragma once
#include <asterion/domain/intraday_bars.hpp>
#include <asterion/kernel/thread_pool.hpp>
#include <deque>
#include <filesystem>
#include <future>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace asterion::market_data {

// Records every quote the feed delivers as Parquet, partitioned by the
// provider's trading day:
//
//   recordings/ticks/trading_day=<YYYYMMDD>/<instance>-<part>.parquet
//   recordings/minutes/trading_day=<YYYYMMDD>/<instance>-<part>.parquet
//   recordings/segments/trading_day=<YYYYMMDD>/<instance>.parquet
//
// Minutes are the bars the service aggregates for strategies, written once a
// later bar or the next trading day completes them; a bar observation may
// have missed part of is marked partial, as is the bar forming at a stop.
// A segment is one stretch of uninterrupted observation by this process. It
// ends with the reason observation stopped; a segment without an end belongs
// to a process that stopped before it could say why. The time between two
// segments is a gap: nothing is filled in. A segment counts the quotes it
// observed, so fewer rows in its files means a write failed. Files are
// written on a worker; the market loop waits only when four writes are behind.
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
        std::uint64_t rows = 0, bytes = 0, files = 0;
    };

    // What each instrument's minutes have reached in the files.
    struct MinuteMark
    {
        std::string trading_day;
        std::int64_t start_ms = 0;
        std::optional<MinuteBar> forming;
    };

    void take(const InstrumentId& id, const IntradaySeries& series);
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

} // namespace asterion::market_data
