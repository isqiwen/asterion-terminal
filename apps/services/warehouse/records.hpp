#pragma once
#include <asterion/base/decimal.hpp>
#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace asterion::warehouse {

enum class Kind
{
    text,
    date,
    boolean,
    integer,
    number,
    price
};

struct Column
{
    std::string_view name;
    Kind kind;
};

// The columns of a table, in the order of its record's fields below.
std::span<const Column> columns(std::string_view table);
// Storage numbers have the existing DECIMAL(28,6) range. Decimal is accepted
// for prices; exact decimal text also preserves wider provider quantities.
using Cell = std::variant<std::monostate, std::string, bool, std::int64_t, Decimal>;
using Row = std::vector<Cell>;

// One record per table. A Cell field may be missing; numbers are exact decimal text.
struct CalendarDay
{
    std::string exchange, day;
    bool open = false;
    Cell previous_trading_day;
};

struct Contract
{
    std::string exchange, contract, product, delivery_month, source_instrument;
    Cell name;
    std::string list_date, delist_date;
    Cell last_delivery_date, multiplier, per_unit, trade_unit, quote_unit, tick_description,
        delivery_description, trading_hours;
};

struct DailyBar
{
    std::string exchange, product, contract, day;
    Cell open, high, low, close, settlement, previous_close, previous_settlement, volume, amount,
        open_interest, open_interest_change, delivery_settlement;
};

struct MinuteBar
{
    std::string exchange, product, contract, day;
    std::int64_t start_ms = 0;
    bool partial = false;
    Cell open, high, low, close, volume, average_price, open_interest;
};

// Margin and fee rates are fractions; fees per lot are in yuan.
struct Settlement
{
    std::string exchange, product, contract, day;
    Cell long_margin_rate, short_margin_rate, fee_rate, fee_per_lot, close_today_fee_rate,
        close_today_fee_per_lot;
};

// A missing contract applies to every delivery month of the product.
struct PriceTick
{
    std::string exchange, product;
    Cell contract;
    std::string effective_from, effective_until;
    Decimal price_tick;
    std::string source_url;
};

Row row(const CalendarDay&);
Row row(const Contract&);
Row row(const DailyBar&);
Row row(const MinuteBar&);
Row row(const Settlement&);
Row row(const PriceTick&);

template <class Record>
std::vector<Row> rows(const std::vector<Record>& records)
{
    std::vector<Row> result;
    result.reserve(records.size());
    for (const auto& record : records)
        result.push_back(row(record));
    return result;
}

// Stored rows read back, for the tables acquisition itself consults.
CalendarDay read_calendar_day(const Row&);
Contract read_contract(const Row&);

} // namespace asterion::warehouse
