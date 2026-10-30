#include "table_parquet.hpp"
#include "parquet_engine.hpp"
#include <stdexcept>
#include <unordered_map>

namespace asterion::parquet {
namespace {

duckdb::LogicalType logical(const TableColumn& column)
{
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
std::string quoted(std::string_view name)
{
    return '"' + std::string(name) + '"';
}

std::string key_order(const HistoryTable& table)
{
    std::string order;
    for (const auto& column : table.columns)
        if (column.key)
            order += (order.empty() ? "" : ", ") + quoted(column.name);
    return order;
}

duckdb::Value value(const TableColumn& column, const TableCell& cell)
{
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

// Text is data, never SQL: quote it as a string literal.
std::string text_literal(std::string_view value)
{
    std::string quoted = "'";
    for (const auto c : value) {
        quoted += c;
        if (c == '\'')
            quoted += '\'';
    }
    return quoted + "'";
}

// One cell of a flat result vector; a missing value stays missing.
TableCell cell(const TableColumn& column, duckdb::Vector& vector, duckdb::idx_t row)
{
    if (!duckdb::FlatVector::Validity(vector).RowIsValid(row)) {
        if (column.key)
            throw std::invalid_argument("Parquet table key has a missing value");
        return {};
    }
    switch (column.type) {
        case TableType::text:
        case TableType::contract:
            return duckdb::FlatVector::GetData<duckdb::string_t>(vector)[row].GetString();
        case TableType::date:
            return std::int64_t{duckdb::FlatVector::GetData<duckdb::date_t>(vector)[row].days};
        case TableType::boolean:
            return std::int64_t{duckdb::FlatVector::GetData<bool>(vector)[row]};
        case TableType::decimal:
            return duckdb::FlatVector::GetData<std::int64_t>(vector)[row];
    }
    throw std::logic_error("unknown table column type");
}

} // namespace

void write_table(const std::filesystem::path& path, const TablePartition& partition)
{
    const auto& table = *partition.table;
    Engine engine;
    std::string definition;
    for (const auto& column : table.columns)
        definition += (definition.empty() ? "" : ", ") + quoted(column.name) + " "
            + logical(column).ToString() + (column.key ? " NOT NULL" : "");
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

std::vector<std::vector<TableCell>>
read_table(const std::filesystem::path& path, const HistoryTable& table)
{
    Engine engine;
    std::string expected;
    for (const auto& column : table.columns)
        expected += (expected.empty() ? "" : ", ") + std::string(column.name) + " "
            + logical(column).ToString();
    require_schema(engine, path, expected);
    const auto result = engine.run(
        "SELECT * FROM read_parquet(" + literal(path) + ") ORDER BY " + key_order(table));
    std::vector<std::vector<TableCell>> rows;
    rows.reserve(result->RowCount());
    while (auto chunk = result->Fetch()) {
        chunk->Flatten();
        const auto first = rows.size();
        rows.resize(first + chunk->size(), std::vector<TableCell>(table.columns.size()));
        for (std::size_t c = 0; c < table.columns.size(); ++c)
            for (duckdb::idx_t r = 0; r < chunk->size(); ++r)
                rows[first + r][c] = cell(table.columns[c], chunk->data[c], r);
    }
    return rows;
}

void scan_tables(
    std::span<const std::filesystem::path> files,
    const HistoryTable& table,
    std::span<const std::size_t> columns,
    std::span<const std::string> prefixes,
    const std::function<void(std::size_t, std::vector<TableCell>&)>& visit)
{
    if (files.empty())
        return;
    std::string list, selected, wanted;
    std::unordered_map<std::string, std::size_t> index;
    for (std::size_t f = 0; f < files.size(); ++f) {
        list += (f ? ", " : "") + literal(files[f]);
        index.emplace(files[f].string(), f);
    }
    for (const auto column : columns)
        selected += ", " + quoted(table.columns[column].name);
    for (const auto& prefix : prefixes)
        wanted += (wanted.empty() ? " WHERE " : " OR ") + std::string("starts_with(")
            + quoted(table.columns[0].name) + ", " + text_literal(prefix) + ")";
    Engine engine;
    const auto result = engine.run(
        "SELECT filename" + selected + " FROM read_parquet([" + list + "], filename = true)"
        + wanted);
    std::vector<TableCell> cells(columns.size());
    while (auto chunk = result->Fetch()) {
        chunk->Flatten();
        for (duckdb::idx_t r = 0; r < chunk->size(); ++r) {
            const auto file = index.find(
                duckdb::FlatVector::GetData<duckdb::string_t>(chunk->data[0])[r].GetString());
            if (file == index.end())
                throw std::logic_error("Parquet scan returned a file it was not given");
            for (std::size_t c = 0; c < columns.size(); ++c)
                cells[c] = cell(table.columns[columns[c]], chunk->data[c + 1], r);
            visit(file->second, cells);
        }
    }
}

} // namespace asterion::parquet
