#pragma once
#include <asterion/domain/history_tables.hpp>
#include <filesystem>
#include <functional>
#include <span>
#include <vector>
// One partition of a history table as an immutable Parquet file holding
// exactly the table's columns: text and contracts as VARCHAR, dates as DATE,
// flags as BOOLEAN and decimals as DECIMAL(18, scale), whose stored integer is
// the cell's units. Callers verify file digests.
namespace asterion::parquet {
// Writes a sibling temporary file, syncs it and renames it over `path`.
void write_table(const std::filesystem::path& path, const TablePartition& partition);
// The typed rows in key order. Rejects a file with any other column layout.
std::vector<std::vector<TableCell>> read_table(const std::filesystem::path& path,
                                               const HistoryTable& table);
// Several partition files of one table read together: of each row whose first
// column starts with one of `prefixes` (every row when there are none), the
// cells of `columns`, which index the table's columns. `visit` is given the
// row's file as an index into `files`; rows come in no particular order.
// The files must hold exactly the table's columns.
void scan_tables(std::span<const std::filesystem::path> files, const HistoryTable& table,
                 std::span<const std::size_t> columns, std::span<const std::string> prefixes,
                 const std::function<void(std::size_t, std::vector<TableCell>&)>& visit);
} // namespace asterion::parquet
