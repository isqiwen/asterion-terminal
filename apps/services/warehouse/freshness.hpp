#pragma once
#include "store.hpp"
#include <map>
#include <string>
#include <vector>

namespace asterion::warehouse {

// Yesterday in Beijing: providers only hold finished trading days.
std::string yesterday();

// Open days through `through`, sorted, by exchange, from the version's calendar.
using OpenDays = std::map<std::string, std::vector<std::string>>;
OpenDays open_days(const Store&, const Version&, const std::string& through);

// What one exchange holds of a day-partitioned table (daily, settlement,
// price_ticks), or one contract (EXCHANGE/product/YYYY-MM) of minutes.
struct Holding
{
    std::string last;
    // The last day it should hold: its exchange's last open day, or the
    // contract's delisting; empty without a calendar for the exchange.
    std::string through;
    // Open days from its first day through `through` that it lacks.
    std::uint64_t missing = 0;
};

std::map<std::string, Holding>
holdings(const Store&, const Version&, const std::string& table, const OpenDays&);

} // namespace asterion::warehouse
