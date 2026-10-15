#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace asterion {

// Market-wide tables kept by the data service. A table's columns are the
// contract between the host and every data source: a source delivers text
// cells in this order, and the host alone types and validates them.
enum class TableType
{
    text,
    contract,
    date,
    boolean,
    decimal
};

struct TableColumn
{
    std::string_view name;
    TableType type;
    unsigned scale = 0; // decimal places of a decimal column, 0..8
    bool key = false; // part of the row identity; never missing
};
// What one partition of an exchange covers: the whole table, a calendar
// year (YYYY) or a trading day (YYYY-MM-DD).
enum class TablePeriod
{
    all,
    year,
    day
};

struct HistoryTable
{
    std::string_view id;
    unsigned schema; // changes with the columns; partitions of another schema are not read
    TablePeriod period;
    std::span<const TableColumn> columns;
};

std::span<const HistoryTable> history_tables();
const HistoryTable& history_table(std::string_view id);
// Where a column is among a table's columns; the name must be one of them.
std::size_t table_column(const HistoryTable&, std::string_view name);
void validate_table_period(const HistoryTable&, std::string_view period);

// A typed cell. Text and contract columns hold text; the others hold units:
// decimal units at the column's scale, days since 1970-01-01, or 0 and 1.
using TableCell = std::variant<std::monostate, std::int64_t, std::string>;
// Type one nonmissing source cell exactly; never round. Used by partition typing
// and read-only source/partition reconciliation.
TableCell table_cell(const TableColumn&, std::string_view exchange, const std::string& value);
// The cell as readers are shown it; missing cells have no text.
std::optional<std::string> table_cell_text(const TableColumn&, const TableCell&);

struct TablePartition
{
    const HistoryTable* table = nullptr;
    std::string exchange, period;
    std::vector<std::vector<TableCell>> rows; // ascending by key, keys unique
    std::string content; // SHA-256 of the typed rows, independent of file encoding
};

inline constexpr std::size_t max_table_partition_rows = 100000;
using TableSourceRow = std::vector<std::optional<std::string>>;
// Types the cells a data source delivered for one partition, in the table's
// column order. Nothing is rounded, filled in or dropped: a cell that is not
// exactly representable rejects the partition, naming its row and column.
TablePartition table_partition(
    const HistoryTable&,
    std::string_view exchange,
    std::string_view period,
    const std::vector<TableSourceRow>& rows);

} // namespace asterion
