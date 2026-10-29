#include "database.hpp"
#include <asterion/foundation/error.hpp>

namespace asterion::recording {

std::unique_ptr<duckdb::DuckDB> open_database(const std::string& memory_limit)
{
    duckdb::DBConfig config;
    config.SetOptionByName("autoinstall_known_extensions", duckdb::Value::BOOLEAN(false));
    config.SetOptionByName("autoload_known_extensions", duckdb::Value::BOOLEAN(false));
    config.SetOptionByName("allow_community_extensions", duckdb::Value::BOOLEAN(false));
    config.SetOptionByName("threads", duckdb::Value::BIGINT(1));
    config.SetOptionByName("memory_limit", duckdb::Value(memory_limit));
    config.SetOptionByName("temp_directory", duckdb::Value(""));
    return std::make_unique<duckdb::DuckDB>(nullptr, &config);
}

std::unique_ptr<duckdb::MaterializedQueryResult>
query(duckdb::Connection& connection, const std::string& sql, duckdb::vector<duckdb::Value> values)
{
    auto statement = connection.Prepare(sql);
    if (statement->HasError())
        throw Error(
            ErrorCode::operation_failed,
            "recording query failed: " + statement->GetError());
    auto result = statement->Execute(values, false);
    if (result->HasError())
        throw Error(ErrorCode::operation_failed, "recording query failed: " + result->GetError());
    return duckdb::unique_ptr_cast<duckdb::QueryResult, duckdb::MaterializedQueryResult>(
        std::move(result));
}

} // namespace asterion::recording
