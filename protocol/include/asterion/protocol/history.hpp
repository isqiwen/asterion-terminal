#pragma once
#include <asterion/history.pb.h>
#include <string>
#include <string_view>

namespace asterion::protocol {

// The warehouse's names: calendar, contracts, daily, minutes, settlement, price_ticks.
std::string table_name(::history::Table table);
::history::Table table_of(std::string_view name);

} // namespace asterion::protocol
