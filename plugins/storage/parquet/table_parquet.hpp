#pragma once
#include <asterion/domain/history_tables.hpp>
#include <filesystem>
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
} // namespace asterion::parquet
