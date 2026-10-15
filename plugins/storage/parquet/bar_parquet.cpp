#include "bar_parquet.hpp"
#include <asterion/domain/trading_days.hpp>
#include "parquet_engine.hpp"
#include <asterion/foundation/time.hpp>
#include <array>
#include <stdexcept>
namespace asterion::parquet {
namespace {
namespace fs = std::filesystem;
duckdb::Value price(const Decimal& value) {
  return duckdb::Value::DECIMAL(value.raw(), 18, 8);
}
duckdb::Value quantity(const Decimal& value) {
  return duckdb::Value::DECIMAL(value.raw(), 38, 8);
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
  engine.run("CREATE TEMP TABLE bars(timestamp_ns BIGINT NOT NULL, open DECIMAL(18,8) NOT NULL,"
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
std::vector<MinuteDayEvidence> minute_evidence(const std::vector<fs::path>& files) {
  if (files.empty())
    return {};
  Engine engine;
  std::string list = "[";
  for (const auto& file : files) {
    require_schema(engine, file,
                   "timestamp_ns BIGINT, open DECIMAL(18,8), high DECIMAL(18,8),"
                   " low DECIMAL(18,8), close DECIMAL(18,8), volume DECIMAL(38,8),"
                   " amount DECIMAL(38,8), open_interest DECIMAL(38,8), trading_day VARCHAR");
    list += (list.size() > 1 ? ", " : "") + literal(file);
  }
  list += "]";
  const auto result = engine.run("SELECT trading_day, count(*)::UBIGINT, min(timestamp_ns), "
                                 "max(timestamp_ns) FROM read_parquet(" +
                                 list + ") GROUP BY trading_day ORDER BY trading_day");
  std::vector<MinuteDayEvidence> days;
  while (auto chunk = result->Fetch()) {
    chunk->Flatten();
    const auto* values = duckdb::FlatVector::GetData<duckdb::string_t>(chunk->data[0]);
    const auto* counts = duckdb::FlatVector::GetData<std::uint64_t>(chunk->data[1]);
    const auto* first = duckdb::FlatVector::GetData<std::int64_t>(chunk->data[2]);
    const auto* last = duckdb::FlatVector::GetData<std::int64_t>(chunk->data[3]);
    for (duckdb::idx_t row = 0; row < chunk->size(); ++row) {
      auto text = values[row].GetString();
      if (!text.empty())
        (void)parse_trading_date(text);
      days.push_back({std::move(text), counts[row], first[row], last[row]});
    }
  }
  return days;
}
} // namespace asterion::parquet
