#include <asterion/domain/history_tables.hpp>
#include <asterion/domain/trading_days.hpp>
#include <asterion/domain/history_identity.hpp>
#include <asterion/kernel/process/artifact.hpp>
#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace asterion {
namespace {

using enum TableType;
// Columns hold what the source published. Everything but the key may be missing.
constexpr TableColumn contracts[] = {
    {"contract", contract, 0, true},
    {"source_instrument", text},
    {"name", text},
    {"list_date", date},
    {"delist_date", date},
    {"last_delivery_date", date},
    {"multiplier", decimal, 8},
    {"per_unit", decimal, 8},
    {"trade_unit", text},
    {"quote_unit", text},
    {"tick_description", text},
    {"delivery_description", text},
    {"trading_hours", text},
};
constexpr TableColumn calendar[] = {
    {"day", date, 0, true},
    {"open", boolean},
    {"previous_trading_day", date},
};
// Prices as quoted, quantities in contracts, amount in the quote currency.
constexpr TableColumn daily[] = {
    {"contract", contract, 0, true},
    {"open", decimal, 8},
    {"high", decimal, 8},
    {"low", decimal, 8},
    {"close", decimal, 8},
    {"settlement", decimal, 8},
    {"previous_close", decimal, 8},
    {"previous_settlement", decimal, 8},
    {"volume", decimal, 0},
    {"amount", decimal, 2},
    {"open_interest", decimal, 0},
    {"open_interest_change", decimal, 0},
    {"delivery_settlement", decimal, 8},
};
// The settlement parameters of a contract on a day: what the exchange charges
// and requires, in the units the source publishes. They differ by exchange: a
// fee is a share of turnover or an amount per lot, a margin rate a fraction
// or a percentage. The settlement price itself is in futures.daily.
constexpr TableColumn settlement[] = {
    {"contract", contract, 0, true},
    {"trading_fee_rate", decimal, 8},
    {"trading_fee", decimal, 8},
    {"close_today_fee", decimal, 8},
    {"delivery_fee", decimal, 8},
    {"long_margin_rate", decimal, 8},
    {"short_margin_rate", decimal, 8},
    {"buy_hedging_margin_rate", decimal, 8},
    {"sell_hedging_margin_rate", decimal, 8},
};
// The lines of an exchange's daily report of registered warehouse receipts,
// in the order published. What tells two lines of a product apart differs by
// exchange and product (a warehouse, a region, a crop year, a grade, a
// brand), and some lines nothing published tells apart: a line is known by
// its place among the lines of its product, on that day only.
constexpr TableColumn receipts[] = {
    {"product", text, 0, true},
    {"line", decimal, 0, true},
    {"product_name", text},
    {"warehouse", text},
    {"warehouse_id", text},
    {"area", text},
    {"year", text},
    {"grade", text},
    {"brand", text},
    {"place", text},
    {"converted", text},
    {"previous_volume", decimal, 0},
    {"volume", decimal, 0},
    {"change", decimal, 0},
    {"unit", text},
    {"premium", decimal, 8},
};
// What each member traded and holds in a contract or a product, as the
// exchange ranks it. The instrument is a contract (SHFE/cu/2024-03) or a
// product (SHFE/cu).
constexpr TableColumn rankings[] = {
    {"instrument", text, 0, true},
    {"member", text, 0, true},
    {"volume", decimal, 0},
    {"volume_change", decimal, 0},
    {"long", decimal, 0},
    {"long_change", decimal, 0},
    {"short", decimal, 0},
    {"short_change", decimal, 0},
};
constexpr TableColumn margin[] = {
    {"contract", contract, 0, true},
    {"phase", text, 0, true},
    {"long_margin_rate", decimal, 8},
    {"short_margin_rate", decimal, 8},
};
constexpr HistoryTable tables[] = {
    {"futures.margin", 1, TablePeriod::day, margin},
    {"futures.contracts", 1, TablePeriod::all, contracts},
    {"futures.calendar", 1, TablePeriod::year, calendar},
    {"futures.daily", 1, TablePeriod::day, daily},
    {"futures.settlement", 1, TablePeriod::day, settlement},
    {"futures.receipts", 1, TablePeriod::day, receipts},
    {"futures.rankings", 1, TablePeriod::day, rankings},
};
constexpr std::size_t max_text_bytes = 1024;
constexpr std::int64_t powers[] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000};

bool digits(std::string_view value)
{
    return !value.empty() && std::ranges::all_of(value, [](char c) {
        return c >= '0' && c <= '9';
    });
}

// Plain decimal notation only; a source expands exponents itself. Places past
// the scale must be zeros: nothing is rounded.
std::int64_t exact_units(std::string_view value, unsigned scale)
{
    const bool negative = value.starts_with('-');
    if (negative)
        value.remove_prefix(1);
    const auto dot = value.find('.');
    const auto integer = value.substr(0, dot);
    auto fraction = dot == std::string_view::npos ? std::string_view{} : value.substr(dot + 1);
    if (!digits(integer) || (dot != std::string_view::npos && !digits(fraction)))
        throw std::invalid_argument("is not a plain decimal number");
    if (fraction.size() > scale) {
        if (fraction.find_first_not_of('0', scale) != std::string_view::npos)
            throw std::invalid_argument(
                "has more than " + std::to_string(scale) + " decimal places");
        fraction = fraction.substr(0, scale);
    }
    // Eighteen digits: what a DECIMAL(18, scale) column holds.
    constexpr std::int64_t limit = 999'999'999'999'999'999;
    std::int64_t units = 0;
    const auto append = [&](char digit) {
        if (units > (limit - (digit - '0')) / 10)
            throw std::invalid_argument("is out of range");
        units = units * 10 + (digit - '0');
    };
    for (const char digit : integer)
        append(digit);
    for (const char digit : fraction)
        append(digit);
    for (auto place = fraction.size(); place < scale; ++place)
        append('0');
    return negative ? -units : units;
}

std::int64_t day_number(std::string_view value)
{
    return std::chrono::sys_days(parse_trading_date(value)).time_since_epoch().count();
}

std::string shown(std::string_view value)
{
    constexpr std::size_t limit = 64;
    return '"' + std::string(value.substr(0, limit)) + (value.size() > limit ? "..." : "") + '"';
}

} // namespace

TableCell table_cell(const TableColumn& column, std::string_view exchange, const std::string& value)
{
    switch (column.type) {
        case text:
            if (value.empty() || value.size() > max_text_bytes
                || std::ranges::any_of(value, [](unsigned char c) {
                       return c < 0x20 || c == 0x7f;
                   }))
                throw std::invalid_argument("is empty, too long or has control characters");
            return value;
        case contract:
            try {
                if (HistoryIdentity::parse(value).venue != exchange)
                    throw std::invalid_argument("another exchange");
            } catch (const std::invalid_argument&) {
                throw std::invalid_argument("is not a contract of " + std::string(exchange));
            }
            return value;
        case date:
            try {
                return day_number(value);
            } catch (const std::invalid_argument&) {
                throw std::invalid_argument("is not a date");
            }
        case boolean:
            if (value != "0" && value != "1")
                throw std::invalid_argument("is not 0 or 1");
            return std::int64_t{value == "1"};
        case decimal:
            return exact_units(value, column.scale);
    }
    throw std::logic_error("unknown table column type");
}

std::span<const HistoryTable> history_tables()
{
    return tables;
}

const HistoryTable& history_table(std::string_view id)
{
    for (const auto& table : tables)
        if (table.id == id)
            return table;
    throw std::invalid_argument("unknown history table");
}

std::size_t table_column(const HistoryTable& table, std::string_view name)
{
    const auto found = std::ranges::find(table.columns, name, &TableColumn::name);
    if (found == table.columns.end())
        throw std::logic_error("history table has no such column");
    return static_cast<std::size_t>(found - table.columns.begin());
}

void validate_table_period(const HistoryTable& table, std::string_view period)
{
    switch (table.period) {
        case TablePeriod::all:
            if (period == "all")
                return;
            break;
        case TablePeriod::year:
            if (period.size() == 4 && digits(period) && period >= "1990" && period <= "2100")
                return;
            break;
        case TablePeriod::day:
            (void)parse_trading_date(period);
            return;
    }
    throw std::invalid_argument("invalid history table period");
}

std::optional<std::string> table_cell_text(const TableColumn& column, const TableCell& cell)
{
    if (std::holds_alternative<std::monostate>(cell))
        return std::nullopt;
    if (const auto* value = std::get_if<std::string>(&cell))
        return *value;
    const auto units = std::get<std::int64_t>(cell);
    switch (column.type) {
        case date:
            return format_trading_date(
                std::chrono::year_month_day(std::chrono::sys_days(std::chrono::days(units))));
        case decimal: {
            // Magnitude first: the most negative value has no positive counterpart.
            const auto magnitude = units < 0 ? ~static_cast<std::uint64_t>(units) + 1
                                             : static_cast<std::uint64_t>(units);
            const auto power = static_cast<std::uint64_t>(powers[column.scale]);
            auto result = std::string(units < 0 ? "-" : "") + std::to_string(magnitude / power);
            if (!column.scale)
                return result;
            auto fraction = std::to_string(magnitude % power);
            fraction.insert(0, column.scale - fraction.size(), '0');
            fraction.erase(fraction.find_last_not_of('0') + 1);
            return fraction.empty() ? result : result + "." + fraction;
        }
        default:
            return std::to_string(units);
    }
}

TablePartition table_partition(
    const HistoryTable& table,
    std::string_view exchange,
    std::string_view period,
    const std::vector<TableSourceRow>& rows)
{
    validate_history_venue(exchange);
    validate_table_period(table, period);
    const auto name =
        std::string(table.id) + " " + std::string(exchange) + " " + std::string(period);
    if (rows.size() > max_table_partition_rows)
        throw std::invalid_argument(
            name + ": " + std::to_string(rows.size()) + " rows exceed the partition limit");
    TablePartition result{&table, std::string(exchange), std::string(period), {}, {}};
    result.rows.reserve(rows.size());
    for (std::size_t r = 0; r < rows.size(); ++r) {
        const auto& source = rows[r];
        const auto row = name + ": row " + std::to_string(r + 1)
            + (!source.empty() && source.front() ? " " + shown(*source.front()) : "");
        if (source.size() != table.columns.size())
            throw std::invalid_argument(
                row + " has " + std::to_string(source.size()) + " cells, expected "
                + std::to_string(table.columns.size()));
        auto& cells = result.rows.emplace_back();
        cells.reserve(source.size());
        for (std::size_t c = 0; c < source.size(); ++c) {
            const auto& column = table.columns[c];
            const auto at = row + " column " + std::string(column.name);
            if (!source[c]) {
                if (column.key)
                    throw std::invalid_argument(at + " is missing");
                cells.emplace_back();
                continue;
            }
            try {
                cells.push_back(table_cell(column, exchange, *source[c]));
            } catch (const std::invalid_argument& error) {
                throw std::invalid_argument(at + ": " + shown(*source[c]) + " " + error.what());
            }
            // A year's partition holds that year's days and no others.
            if (column.key && column.type == date && table.period == TablePeriod::year
                && source[c]->substr(0, 4) != period)
                throw std::invalid_argument(
                    at + ": " + shown(*source[c]) + " is outside " + std::string(period));
        }
    }
    const auto key = [&](const std::vector<TableCell>& row) {
        std::vector<TableCell> cells;
        for (std::size_t c = 0; c < table.columns.size(); ++c)
            if (table.columns[c].key)
                cells.push_back(row[c]);
        return cells;
    };
    std::ranges::sort(result.rows, {}, key);
    for (std::size_t r = 1; r < result.rows.size(); ++r)
        if (key(result.rows[r]) == key(result.rows[r - 1])) {
            std::string repeated;
            for (std::size_t c = 0; c < table.columns.size(); ++c)
                if (table.columns[c].key)
                    repeated += (repeated.empty() ? "" : " / ")
                        + table_cell_text(table.columns[c], result.rows[r][c]).value_or("");
            throw std::invalid_argument(
                name + ": key " + shown(repeated) + " appears more than once");
        }
    // Tab and newline cannot occur inside a cell, and no cell is empty.
    auto canonical = std::string(table.id) + "\t" + std::to_string(table.schema) + "\t"
        + result.exchange + "\t" + result.period + "\n";
    for (const auto& row : result.rows) {
        for (std::size_t c = 0; c < row.size(); ++c)
            canonical += (c ? "\t" : "") + table_cell_text(table.columns[c], row[c]).value_or("");
        canonical += "\n";
    }
    result.content = sha256_bytes(canonical);
    return result;
}

} // namespace asterion
