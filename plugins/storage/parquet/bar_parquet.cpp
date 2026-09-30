#include "bar_parquet.hpp"
#include <asterion/foundation/time.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <duckdb.hpp>
#include <stdexcept>
namespace asterion::parquet {
namespace {
namespace fs = std::filesystem;
constexpr auto scale = "100000000";
// A private in-memory engine per call: no catalog, no extension downloads.
struct Engine {
  duckdb::DBConfig config;
  std::unique_ptr<duckdb::DuckDB> database;
  std::unique_ptr<duckdb::Connection> connection;
  Engine() {
    config.SetOptionByName("autoinstall_known_extensions", duckdb::Value::BOOLEAN(false));
    config.SetOptionByName("autoload_known_extensions", duckdb::Value::BOOLEAN(false));
    config.SetOptionByName("allow_community_extensions", duckdb::Value::BOOLEAN(false));
    config.SetOptionByName("threads", duckdb::Value::BIGINT(1));
    database = std::make_unique<duckdb::DuckDB>(nullptr, &config);
    connection = std::make_unique<duckdb::Connection>(*database);
  }
  std::unique_ptr<duckdb::MaterializedQueryResult> run(const std::string& sql) {
    auto result = connection->Query(sql);
    if (result->HasError())
      throw std::runtime_error("Parquet storage operation failed");
    return result;
  }
};
// Paths are data, never SQL: quote them as string literals.
std::string literal(const fs::path& path) {
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
duckdb::Value price(const Decimal& value) {
  return duckdb::Value::DECIMAL(value.raw(), 18, 8);
}
duckdb::Value quantity(const Decimal& value) {
  return duckdb::Value::DECIMAL(value.raw(), 38, 8);
}
duckdb::Value optional_price(const std::optional<Decimal>& value) {
  return value ? price(*value) : duckdb::Value(duckdb::LogicalType::DECIMAL(18, 8));
}
std::string raw(const char* column) {
  return std::string("CAST(CAST(") + column + " AS DECIMAL(38,8)) * " + scale + " AS BIGINT)";
}
void publish(Engine& engine, const std::string& table, const fs::path& path) {
  if (fs::is_symlink(path))
    throw std::invalid_argument("Parquet storage refuses symbolic links");
  auto temporary = path;
  temporary += ".tmp";
  if (fs::is_symlink(temporary))
    throw std::invalid_argument("Parquet storage refuses symbolic links");
  engine.run("COPY " + table + " TO " + literal(temporary) + " (FORMAT parquet, COMPRESSION zstd)");
  publish_file_durably(temporary, path);
}
// Rejects files whose columns are not exactly the expected names and types.
void require_schema(Engine& engine, const fs::path& path, const std::string& expected) {
  const auto result =
      engine.run("SELECT string_agg(column_name || ' ' || column_type, ', ' ORDER BY rowid)"
                 " FROM (SELECT *, row_number() OVER () AS rowid FROM (DESCRIBE SELECT * FROM "
                 "read_parquet(" +
                 literal(path) + ")))");
  if (result->RowCount() != 1 || result->GetValue(0, 0).ToString() != expected)
    throw std::invalid_argument("unsupported Parquet bar schema");
}
Decimal decimal(const duckdb::Value& value) {
  return Decimal::from_raw(value.GetValue<std::int64_t>());
}
} // namespace
void write_minute_bars(const fs::path& path, const std::vector<HistoricalBar>& bars) {
  Engine engine;
  engine.run("CREATE TABLE bars(timestamp_ns BIGINT NOT NULL, open DECIMAL(18,8) NOT NULL,"
             " high DECIMAL(18,8) NOT NULL, low DECIMAL(18,8) NOT NULL,"
             " close DECIMAL(18,8) NOT NULL, volume DECIMAL(38,8) NOT NULL,"
             " amount DECIMAL(38,8) NOT NULL, open_interest DECIMAL(38,8) NOT NULL,"
             " trading_day VARCHAR NOT NULL)");
  {
    duckdb::Appender appender(*engine.connection, "bars");
    for (const auto& bar : bars) {
      bar.validate();
      appender.BeginRow();
      appender.Append<std::int64_t>(bar.timestamp_ns);
      for (const auto* value : {&bar.open, &bar.high, &bar.low, &bar.close})
        appender.Append(price(*value));
      for (const auto* value : {&bar.volume, &bar.amount, &bar.open_interest})
        appender.Append(quantity(*value));
      appender.Append(duckdb::Value(bar.trading_day));
      appender.EndRow();
    }
    appender.Close();
  }
  publish(engine, "(SELECT * FROM bars ORDER BY timestamp_ns)", path);
}
std::vector<HistoricalBar> read_minute_bars(const fs::path& path) {
  Engine engine;
  require_schema(engine, path,
                 "timestamp_ns BIGINT, open DECIMAL(18,8), high DECIMAL(18,8), low DECIMAL(18,8),"
                 " close DECIMAL(18,8), volume DECIMAL(38,8), amount DECIMAL(38,8),"
                 " open_interest DECIMAL(38,8), trading_day VARCHAR");
  const auto result = engine.run(
      "SELECT timestamp_ns, " + raw("open") + ", " + raw("high") + ", " + raw("low") + ", " +
      raw("close") + ", " + raw("volume") + ", " + raw("amount") + ", " + raw("open_interest") +
      ", trading_day FROM read_parquet(" + literal(path) + ") ORDER BY timestamp_ns");
  std::vector<HistoricalBar> bars;
  bars.reserve(result->RowCount());
  for (duckdb::idx_t row = 0; row < result->RowCount(); ++row) {
    for (duckdb::idx_t column = 0; column < 9; ++column)
      if (result->GetValue(column, row).IsNull())
        throw std::invalid_argument("Parquet bar has a missing value");
    HistoricalBar bar{result->GetValue(0, row).GetValue<std::int64_t>(),
                      decimal(result->GetValue(1, row)),
                      decimal(result->GetValue(2, row)),
                      decimal(result->GetValue(3, row)),
                      decimal(result->GetValue(4, row)),
                      decimal(result->GetValue(5, row)),
                      decimal(result->GetValue(6, row)),
                      decimal(result->GetValue(7, row)),
                      result->GetValue(8, row).GetValue<std::string>()};
    bar.validate();
    bars.push_back(std::move(bar));
  }
  return bars;
}
void write_daily_bars(const fs::path& path, const std::vector<HistoricalDailyBar>& bars) {
  Engine engine;
  engine.run("CREATE TABLE bars(trading_day DATE NOT NULL, open DECIMAL(18,8) NOT NULL,"
             " high DECIMAL(18,8) NOT NULL, low DECIMAL(18,8) NOT NULL,"
             " close DECIMAL(18,8) NOT NULL, volume DECIMAL(38,8) NOT NULL,"
             " amount DECIMAL(38,8) NOT NULL, open_interest DECIMAL(38,8) NOT NULL,"
             " previous_close DECIMAL(18,8), previous_settlement DECIMAL(18,8),"
             " settlement DECIMAL(18,8))");
  {
    duckdb::Appender appender(*engine.connection, "bars");
    for (const auto& bar : bars) {
      bar.validate();
      appender.BeginRow();
      appender.Append(duckdb::Value::DATE(static_cast<int>(bar.trading_day.year()),
                                          static_cast<unsigned>(bar.trading_day.month()),
                                          static_cast<unsigned>(bar.trading_day.day())));
      for (const auto* value : {&bar.open, &bar.high, &bar.low, &bar.close})
        appender.Append(price(*value));
      for (const auto* value : {&bar.volume, &bar.amount, &bar.open_interest})
        appender.Append(quantity(*value));
      for (const auto* value : {&bar.previous_close, &bar.previous_settlement, &bar.settlement})
        appender.Append(optional_price(*value));
      appender.EndRow();
    }
    appender.Close();
  }
  publish(engine, "(SELECT * FROM bars ORDER BY trading_day)", path);
}
std::vector<HistoricalDailyBar> read_daily_bars(const fs::path& path) {
  Engine engine;
  require_schema(engine, path,
                 "trading_day DATE, open DECIMAL(18,8), high DECIMAL(18,8), low DECIMAL(18,8),"
                 " close DECIMAL(18,8), volume DECIMAL(38,8), amount DECIMAL(38,8),"
                 " open_interest DECIMAL(38,8), previous_close DECIMAL(18,8),"
                 " previous_settlement DECIMAL(18,8), settlement DECIMAL(18,8)");
  const auto result = engine.run(
      "SELECT strftime(trading_day, '%Y-%m-%d'), " + raw("open") + ", " + raw("high") + ", " +
      raw("low") + ", " + raw("close") + ", " + raw("volume") + ", " + raw("amount") + ", " +
      raw("open_interest") + ", " + raw("previous_close") + ", " + raw("previous_settlement") +
      ", " + raw("settlement") + " FROM read_parquet(" + literal(path) + ") ORDER BY trading_day");
  std::vector<HistoricalDailyBar> bars;
  bars.reserve(result->RowCount());
  for (duckdb::idx_t row = 0; row < result->RowCount(); ++row) {
    for (duckdb::idx_t column = 0; column < 8; ++column)
      if (result->GetValue(column, row).IsNull())
        throw std::invalid_argument("Parquet bar has a missing value");
    const auto optional = [&](duckdb::idx_t column) -> std::optional<Decimal> {
      const auto value = result->GetValue(column, row);
      return value.IsNull() ? std::nullopt : std::optional(decimal(value));
    };
    HistoricalDailyBar bar{parse_trading_date(result->GetValue(0, row).GetValue<std::string>()),
                           decimal(result->GetValue(1, row)),
                           decimal(result->GetValue(2, row)),
                           decimal(result->GetValue(3, row)),
                           decimal(result->GetValue(4, row)),
                           decimal(result->GetValue(5, row)),
                           decimal(result->GetValue(6, row)),
                           decimal(result->GetValue(7, row)),
                           optional(8),
                           optional(9),
                           optional(10)};
    bar.validate();
    bars.push_back(std::move(bar));
  }
  return bars;
}
} // namespace asterion::parquet
