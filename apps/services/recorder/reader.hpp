#pragma once
#include <asterion/recorder.pb.h>
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
    recorder::proto::Days days();
    recorder::proto::Minutes minutes(const recorder::proto::MinutesQuery& query);

private:
    const std::filesystem::path root_;
    std::unique_ptr<duckdb::DuckDB> database_;
};

} // namespace asterion::recording
