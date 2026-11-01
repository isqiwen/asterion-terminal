#pragma once
#include <asterion/recording.pb.h>
#include <duckdb.hpp>
#include <filesystem>
#include <memory>

namespace asterion::recording {

// Answers the read-only queries from the files the recorder wrote. It has its
// own DuckDB database, so a long read never holds the lane the recorder's
// writer uses.
class Reader
{
public:
    explicit Reader(std::filesystem::path recordings);
    recording::proto::Days days();
    std::vector<recording::proto::Segment> segments(const std::string& trading_day);
    recording::proto::Minutes minutes(const recording::proto::MinutesQuery& request);

private:
    const std::filesystem::path root_;
    std::unique_ptr<duckdb::DuckDB> database_;
};

} // namespace asterion::recording
