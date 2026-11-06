#pragma once
#include <asterion/futures/intraday_bars.hpp>
#include <duckdb.hpp>
#include <filesystem>
#include <memory>

namespace asterion::recording {

// Reads the files the recorder wrote. It has its own DuckDB database, so a
// long read never holds the lane the recorder's writer uses. Days are YYYYMMDD.
class Reader
{
public:
    struct Segment
    {
        std::string instance;
        std::uint64_t number = 0;
        std::int64_t first_received_ms = 0, last_received_ms = 0;
        std::uint64_t ticks = 0;
        std::string end;
    };

    explicit Reader(std::filesystem::path recordings);
    std::vector<Segment> segments(const std::string& trading_day);
    // The trading days from `first` to `last` (YYYYMMDD) that have segments.
    std::vector<std::string> days(const std::string& first, const std::string& last) const;
    std::vector<MinuteBar> minutes(const std::string& trading_day, const InstrumentId& id);
    // The instruments with recorded minutes on a trading day (YYYYMMDD).
    std::vector<InstrumentId> instruments(const std::string& trading_day);

private:
    const std::filesystem::path root_;
    std::unique_ptr<duckdb::DuckDB> database_;
};

} // namespace asterion::recording
