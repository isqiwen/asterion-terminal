#include "table_parquet.hpp"
#include "parquet_engine.hpp"
#include <stdexcept>
namespace asterion::parquet {
namespace {
duckdb::LogicalType logical(const TableColumn& column) {
  switch (column.type) {
  case TableType::text:
  case TableType::contract:
    return duckdb::LogicalType::VARCHAR;
  case TableType::date:
    return duckdb::LogicalType::DATE;
  case TableType::boolean:
    return duckdb::LogicalType::BOOLEAN;
  case TableType::decimal:
    return duckdb::LogicalType::DECIMAL(18, static_cast<std::uint8_t>(column.scale));
  }
  throw std::logic_error("unknown table column type");
}
// Column names are identifiers of the table definitions; some are SQL words.
std::string quoted(std::string_view name) {
  return '"' + std::string(name) + '"';
}
std::string key_order(const HistoryTable& table) {
  std::string order;
  for (const auto& column : table.columns)
    if (column.key)
      order += (order.empty() ? "" : ", ") + quoted(column.name);
  return order;
}
duckdb::Value value(const TableColumn& column, const TableCell& cell) {
  if (std::holds_alternative<std::monostate>(cell))
    return duckdb::Value(logical(column));
  if (const auto* text = std::get_if<std::string>(&cell))
    return duckdb::Value(*text);
  const auto units = std::get<std::int64_t>(cell);
  switch (column.type) {
  case TableType::date:
    return duckdb::Value::DATE(duckdb::date_t(static_cast<std::int32_t>(units)));
  case TableType::boolean:
    return duckdb::Value::BOOLEAN(units != 0);
  case TableType::decimal:
    return duckdb::Value::DECIMAL(units, 18, static_cast<std::uint8_t>(column.scale));
  default:
    throw std::logic_error("table cell does not match its column");
  }
}
} // namespace
void write_table(const std::filesystem::path& path, const TablePartition& partition) {
  const auto& table = *partition.table;
  Engine engine;
  std::string definition;
  for (const auto& column : table.columns)
    definition += (definition.empty() ? "" : ", ") + quoted(column.name) + " " +
                  logical(column).ToString() + (column.key ? " NOT NULL" : "");
  engine.run("CREATE TEMP TABLE cells(" + definition + ")");
  {
    duckdb::Appender appender(*engine.connection, "cells");
    for (const auto& row : partition.rows) {
      appender.BeginRow();
      for (std::size_t c = 0; c < table.columns.size(); ++c)
        appender.Append(value(table.columns[c], row[c]));
      appender.EndRow();
    }
    appender.Close();
  }
  publish(engine, "(SELECT * FROM cells ORDER BY " + key_order(table) + ")", path);
}
std::vector<std::vector<TableCell>> read_table(const std::filesystem::path& path,
                                               const HistoryTable& table) {
  Engine engine;
  std::string expected;
  for (const auto& column : table.columns)
    expected += (expected.empty() ? "" : ", ") + std::string(column.name) + " " +
                logical(column).ToString();
  require_schema(engine, path, expected);
  const auto result =
      engine.run("SELECT * FROM read_parquet(" + literal(path) + ") ORDER BY " + key_order(table));
  std::vector<std::vector<TableCell>> rows;
  rows.reserve(result->RowCount());
  while (auto chunk = result->Fetch()) {
    chunk->Flatten();
    const auto first = rows.size();
    rows.resize(first + chunk->size(), std::vector<TableCell>(table.columns.size()));
    for (std::size_t c = 0; c < table.columns.size(); ++c) {
      auto& vector = chunk->data[c];
      const auto& validity = duckdb::FlatVector::Validity(vector);
      for (duckdb::idx_t r = 0; r < chunk->size(); ++r) {
        if (!validity.RowIsValid(r)) {
          if (table.columns[c].key)
            throw std::invalid_argument("Parquet table key has a missing value");
          continue;
        }
        auto& cell = rows[first + r][c];
        switch (table.columns[c].type) {
        case TableType::text:
        case TableType::contract:
          cell = duckdb::FlatVector::GetData<duckdb::string_t>(vector)[r].GetString();
          break;
        case TableType::date:
          cell = std::int64_t{duckdb::FlatVector::GetData<duckdb::date_t>(vector)[r].days};
          break;
        case TableType::boolean:
          cell = std::int64_t{duckdb::FlatVector::GetData<bool>(vector)[r]};
          break;
        case TableType::decimal:
          cell = duckdb::FlatVector::GetData<std::int64_t>(vector)[r];
          break;
        }
      }
    }
  }
  return rows;
}
} // namespace asterion::parquet
