#pragma once
#include <duckdb.hpp>
#include <memory>
#include <string>

namespace asterion::recording {

// A DuckDB database of its own, so reading or merging files never holds the
// lane the recorder's writer uses. No extensions are loaded or installed.
std::unique_ptr<duckdb::DuckDB> open_database(const std::string& memory_limit);

std::unique_ptr<duckdb::MaterializedQueryResult> query(
    duckdb::Connection& connection,
    const std::string& sql,
    duckdb::vector<duckdb::Value> values = {});

} // namespace asterion::recording
