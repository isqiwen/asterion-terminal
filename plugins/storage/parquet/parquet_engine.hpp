#pragma once
#include <duckdb.hpp>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
// The embedded DuckDB every Parquet reader and writer in this directory shares.
namespace asterion::parquet {
// One in-memory engine per process (no catalog, no extension downloads);
// starting DuckDB costs far more than a query. Bound Parquet execution to one
// query lane so concurrent chart/read requests cannot multiply the engine's
// working set or concurrently touch its shared buffer queues. Each call still
// owns a connection and its temporary tables; hashing and indicators run outside
// this lane.
struct Engine {
  Engine();
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
