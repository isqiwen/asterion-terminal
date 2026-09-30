#include "bar_parquet.hpp"
#include <asterion/foundation/time.hpp>
#include <asterion/kernel/durable_file.hpp>
#include <duckdb.hpp>
#include <array>
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
// DECIMAL(18,8) is stored as an int64 and DECIMAL(38,8) as a 128-bit integer,
// both scaled by 10^8 exactly like Decimal::raw(): columns move without arithmetic.
const duckdb::LogicalType price_type = duckdb::LogicalType::DECIMAL(18, 8);
const duckdb::LogicalType quantity_type = duckdb::LogicalType::DECIMAL(38, 8);
std::int64_t narrow(const duckdb::hugeint_t& value) {
  std::int64_t result = 0;
  if (!duckdb::Hugeint::TryCast(value, result))
    throw std::invalid_argument("Parquet bar value exceeds the Decimal range");
  return result;
}
void write_minute_bars(const fs::path& path, const std::vector<HistoricalBar>& bars) {
  Engine engine;
  engine.run("CREATE TABLE bars(timestamp_ns BIGINT NOT NULL, open DECIMAL(18,8) NOT NULL,"
             " high DECIMAL(18,8) NOT NULL, low DECIMAL(18,8) NOT NULL,"
             " close DECIMAL(18,8) NOT NULL, volume DECIMAL(38,8) NOT NULL,"
             " amount DECIMAL(38,8) NOT NULL, open_interest DECIMAL(38,8) NOT NULL,"
             " trading_day VARCHAR NOT NULL)");
  {
    duckdb::Appender appender(*engine.connection, "bars");
    const duckdb::vector<duckdb::LogicalType> types{duckdb::LogicalType::BIGINT,
                                                    price_type,
                                                    price_type,
                                                    price_type,
                                                    price_type,
                                                    quantity_type,
                                                    quantity_type,
                                                    quantity_type,
                                                    duckdb::LogicalType::VARCHAR};
    duckdb::DataChunk chunk;
    chunk.Initialize(duckdb::Allocator::DefaultAllocator(), types);
    for (std::size_t offset = 0; offset < bars.size(); offset += STANDARD_VECTOR_SIZE) {
      const auto count = std::min<std::size_t>(STANDARD_VECTOR_SIZE, bars.size() - offset);
      chunk.Reset();
      auto* times = duckdb::FlatVector::GetData<std::int64_t>(chunk.data[0]);
      std::array<std::int64_t*, 4> prices{};
      std::array<duckdb::hugeint_t*, 3> quantities{};
      for (std::size_t column = 0; column < 4; ++column)
        prices[column] = duckdb::FlatVector::GetData<std::int64_t>(chunk.data[column + 1]);
      for (std::size_t column = 0; column < 3; ++column)
        quantities[column] = duckdb::FlatVector::GetData<duckdb::hugeint_t>(chunk.data[column + 5]);
      auto* days = duckdb::FlatVector::GetData<duckdb::string_t>(chunk.data[8]);
      for (std::size_t row = 0; row < count; ++row) {
        const auto& bar = bars[offset + row];
        bar.validate();
        times[row] = bar.timestamp_ns;
        prices[0][row] = bar.open.raw();
        prices[1][row] = bar.high.raw();
        prices[2][row] = bar.low.raw();
        prices[3][row] = bar.close.raw();
        quantities[0][row] = duckdb::hugeint_t(bar.volume.raw());
        quantities[1][row] = duckdb::hugeint_t(bar.amount.raw());
        quantities[2][row] = duckdb::hugeint_t(bar.open_interest.raw());
        days[row] = duckdb::StringVector::AddString(chunk.data[8], bar.trading_day);
      }
      chunk.SetCardinality(count);
      appender.AppendDataChunk(chunk);
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
  const auto result =
      engine.run("SELECT * FROM read_parquet(" + literal(path) + ") ORDER BY timestamp_ns");
  std::vector<HistoricalBar> bars;
  bars.reserve(result->RowCount());
  while (auto chunk = result->Fetch()) {
    chunk->Flatten();
    const auto rows = chunk->size();
    for (std::size_t column = 0; column < 9; ++column)
      if (!duckdb::FlatVector::Validity(chunk->data[column]).CheckAllValid(rows))
        throw std::invalid_argument("Parquet bar has a missing value");
    const auto* times = duckdb::FlatVector::GetData<std::int64_t>(chunk->data[0]);
    const auto price = [&](std::size_t column, duckdb::idx_t row) {
      return Decimal::from_raw(duckdb::FlatVector::GetData<std::int64_t>(chunk->data[column])[row]);
    };
    const auto quantity = [&](std::size_t column, duckdb::idx_t row) {
      return Decimal::from_raw(
          narrow(duckdb::FlatVector::GetData<duckdb::hugeint_t>(chunk->data[column])[row]));
    };
    const auto* days = duckdb::FlatVector::GetData<duckdb::string_t>(chunk->data[8]);
    for (duckdb::idx_t row = 0; row < rows; ++row) {
      HistoricalBar bar{times[row],       price(1, row),    price(2, row),
                        price(3, row),    price(4, row),    quantity(5, row),
                        quantity(6, row), quantity(7, row), days[row].GetString()};
      bar.validate();
      bars.push_back(std::move(bar));
    }
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
std::vector<std::string> trading_days(const std::vector<fs::path>& files, bool minute) {
  if (files.empty())
    return {};
  Engine engine;
  std::string list = "[";
  for (const auto& file : files) {
    require_schema(engine, file,
                   minute ? "timestamp_ns BIGINT, open DECIMAL(18,8), high DECIMAL(18,8),"
                            " low DECIMAL(18,8), close DECIMAL(18,8), volume DECIMAL(38,8),"
                            " amount DECIMAL(38,8), open_interest DECIMAL(38,8),"
                            " trading_day VARCHAR"
                          : "trading_day DATE, open DECIMAL(18,8), high DECIMAL(18,8),"
                            " low DECIMAL(18,8), close DECIMAL(18,8), volume DECIMAL(38,8),"
                            " amount DECIMAL(38,8), open_interest DECIMAL(38,8),"
                            " previous_close DECIMAL(18,8), previous_settlement DECIMAL(18,8),"
                            " settlement DECIMAL(18,8)");
    list += (list.size() > 1 ? ", " : "") + literal(file);
  }
  list += "]";
  const auto day = minute ? std::string("trading_day") : "strftime(trading_day, '%Y-%m-%d')";
  const auto result = engine.run("SELECT DISTINCT " + day + " AS day FROM read_parquet(" + list +
                                 ") WHERE " + day + " <> '' ORDER BY day");
  std::vector<std::string> days;
  while (auto chunk = result->Fetch()) {
    chunk->Flatten();
    const auto* values = duckdb::FlatVector::GetData<duckdb::string_t>(chunk->data[0]);
    for (duckdb::idx_t row = 0; row < chunk->size(); ++row) {
      auto text = values[row].GetString();
      (void)parse_trading_date(text);
      days.push_back(std::move(text));
    }
  }
  return days;
}
} // namespace asterion::parquet
