#include "parquet_engine.hpp"
#include <asterion/system/durable_file.hpp>
#include <stdexcept>

namespace asterion::parquet {
namespace {
namespace fs = std::filesystem;

struct Shared
{
    std::mutex mutex;
    std::unique_ptr<duckdb::DuckDB> database;
};

Shared& lane(Lane which)
{
    const auto open = [] {
        duckdb::DBConfig config;
        config.SetOptionByName("autoinstall_known_extensions", duckdb::Value::BOOLEAN(false));
        config.SetOptionByName("autoload_known_extensions", duckdb::Value::BOOLEAN(false));
        config.SetOptionByName("allow_community_extensions", duckdb::Value::BOOLEAN(false));
        config.SetOptionByName("threads", duckdb::Value::BIGINT(1));
        // Storage reads and writes bounded pages/segments, not arbitrary analytics.
        // Each lane has this buffer budget; it does not include caller-owned
        // bars or other process allocations.
        config.SetOptionByName("memory_limit", duckdb::Value("128 MiB"));
        config.SetOptionByName("temp_directory", duckdb::Value(""));
        return std::make_unique<duckdb::DuckDB>(nullptr, &config);
    };
    static Shared warehouse{{}, open()}, recording{{}, open()};
    return which == Lane::recording ? recording : warehouse;
}

} // namespace

Engine::Engine(Lane which)
    : lease(lane(which).mutex),
      connection(std::make_unique<duckdb::Connection>(*lane(which).database))
{}

std::unique_ptr<duckdb::MaterializedQueryResult> Engine::run(const std::string& sql)
{
    auto result = connection->Query(sql);
    if (result->HasError())
        throw std::runtime_error("Parquet storage operation failed: " + result->GetError());
    return result;
}

std::string literal(const fs::path& path)
{
    if (!path.is_absolute())
        throw std::invalid_argument("Parquet storage requires absolute paths");
    const auto text = path.string();
    if (text.find('\0') != std::string::npos)
        throw std::invalid_argument("invalid Parquet path");
    std::string quoted = "'";
    for (const auto c : text) {
        quoted += c;
        if (c == '\'')
            quoted += '\'';
    }
    return quoted + "'";
}

void publish(Engine& engine, const std::string& table, const fs::path& path)
{
    if (fs::is_symlink(path))
        throw std::invalid_argument("Parquet storage refuses symbolic links");
    auto temporary = path;
    temporary += ".tmp";
    if (fs::is_symlink(temporary))
        throw std::invalid_argument("Parquet storage refuses symbolic links");
    engine.run(
        "COPY " + table + " TO " + literal(temporary) + " (FORMAT parquet, COMPRESSION zstd)");
    publish_file_durably(temporary, path);
}

void require_schema(Engine& engine, const fs::path& path, const std::string& expected)
{
    const auto result = engine.run(
        "SELECT string_agg(column_name || ' ' || column_type, ', ' ORDER BY rowid)" " FROM (SELECT *, row_number() OVER () AS rowid FROM (DESCRIBE SELECT * FROM " "read_parquet("
        + literal(path) + ")))");
    if (result->RowCount() != 1 || result->GetValue(0, 0).ToString() != expected)
        throw std::invalid_argument("unsupported Parquet schema");
}

} // namespace asterion::parquet
