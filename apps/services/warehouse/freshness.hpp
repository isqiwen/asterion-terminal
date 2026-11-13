#pragma once
#include "store.hpp"
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace asterion::warehouse {

// Yesterday in Beijing: providers only hold finished trading days.
std::string yesterday();

// Open days through `through`, sorted, by exchange, from the version's calendar.
using OpenDays = std::map<std::string, std::vector<std::string>>;
OpenDays open_days(const Store&, const Version&, const std::string& through);

// Partition keys of a table that every source still answered empty after two
// more trading days of their exchange. Kept beside the acquisition tasks, not in the dataset:
// an empty partition would let research take the day as stored.
std::filesystem::path empty_file(const std::filesystem::path& directory, const std::string& table);
std::set<std::string> empty_keys(const std::filesystem::path& directory, const std::string& table);

// What one exchange holds of a day-partitioned table (daily, settlement,
// price_ticks), or one contract (EXCHANGE/product/YYYY-MM) of minutes.
struct Holding
{
    std::string last;
    // The last day it should hold: its exchange's last open day, or the
    // contract's delisting; empty without a calendar for the exchange.
    std::string through;
    // Open days from its first day through `through` that it lacks, apart
    // from the `empty` ones every source answered empty.
    std::uint64_t missing = 0, empty = 0;
};

// `last` counts the empty days too: nothing after them is due again.
std::map<std::string, Holding> holdings(
    const Store&,
    const Version&,
    const std::string& table,
    const OpenDays&,
    const std::set<std::string>& empty);

} // namespace asterion::warehouse
