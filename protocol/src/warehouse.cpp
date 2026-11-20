#include <asterion/protocol/warehouse.hpp>
#include <stdexcept>
#include <string>

namespace asterion::protocol {

std::string table_name(::warehouse::Table table)
{
    switch (table) {
        case ::warehouse::TABLE_CALENDAR:
            return "calendar";
        case ::warehouse::TABLE_CONTRACTS:
            return "contracts";
        case ::warehouse::TABLE_DAILY:
            return "daily";
        case ::warehouse::TABLE_MINUTES:
            return "minutes";
        case ::warehouse::TABLE_SETTLEMENT:
            return "settlement";
        case ::warehouse::TABLE_PRICE_TICKS:
            return "price_ticks";
        default:
            throw std::invalid_argument("missing table");
    }
}

::warehouse::Table table_of(std::string_view name)
{
    for (int value = ::warehouse::TABLE_CALENDAR; value <= ::warehouse::TABLE_PRICE_TICKS; ++value)
        if (table_name(static_cast<::warehouse::Table>(value)) == name)
            return static_cast<::warehouse::Table>(value);
    throw std::invalid_argument("unknown table: " + std::string(name));
}

} // namespace asterion::protocol
