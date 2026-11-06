#include <asterion/protocol/history.hpp>
#include <stdexcept>
#include <string>

namespace asterion::protocol {

std::string table_name(::history::Table table)
{
    switch (table) {
        case ::history::TABLE_CALENDAR:
            return "calendar";
        case ::history::TABLE_CONTRACTS:
            return "contracts";
        case ::history::TABLE_DAILY:
            return "daily";
        case ::history::TABLE_MINUTES:
            return "minutes";
        case ::history::TABLE_SETTLEMENT:
            return "settlement";
        case ::history::TABLE_PRICE_TICKS:
            return "price_ticks";
        default:
            throw std::invalid_argument("missing table");
    }
}

::history::Table table_of(std::string_view name)
{
    for (int value = ::history::TABLE_CALENDAR; value <= ::history::TABLE_PRICE_TICKS; ++value)
        if (table_name(static_cast<::history::Table>(value)) == name)
            return static_cast<::history::Table>(value);
    throw std::invalid_argument("unknown table: " + std::string(name));
}

} // namespace asterion::protocol
