#pragma once
#include <duckdb.hpp>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>

// The embedded DuckDB every Parquet reader and writer in this directory shares.
namespace asterion::parquet {

// In-memory engines (no catalog, no extension downloads), one per lane;
// starting DuckDB costs far more than a query. Each lane runs one query at a
// time, so concurrent reads cannot multiply its working set. Recording has a
// lane of its own: a long warehouse read never holds back its writes. Each
// call still owns a connection and its temporary tables.
enum class Lane
{
    warehouse,
    recording
};

struct Engine
{
    explicit Engine(Lane lane = Lane::warehouse);
    std::unique_ptr<duckdb::MaterializedQueryResult> run(const std::string& sql);
    // Declaration order keeps the connection's destruction inside the lease.
    std::unique_lock<std::mutex> lease;
    std::unique_ptr<duckdb::Connection> connection;
};

// Paths are data, never SQL: quote them as string literals.
std::string literal(const std::filesystem::path& path);
// Writes `table` to a sibling temporary file, syncs it and renames it over `path`.
void publish(Engine& engine, const std::string& table, const std::filesystem::path& path);
// Rejects files whose columns are not exactly the expected names and types.
void require_schema(Engine& engine, const std::filesystem::path& path, const std::string& expected);

} // namespace asterion::parquet
