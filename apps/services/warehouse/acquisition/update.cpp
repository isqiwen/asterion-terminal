#include "update.hpp"
#include "freshness.hpp"
#include "source.hpp"
#include <asterion/protocol/warehouse.hpp>
#include <set>

namespace asterion::warehouse::acquisition {
namespace wire = ::warehouse;

std::vector<wire::StartAcquisition> update_requests(
    const std::filesystem::path& directory,
    const std::string& yesterday,
    const std::string& prefix)
{
    const Store store(source_directory(directory, "research"), "research");
    const auto version = store.current();
    std::vector<wire::StartAcquisition> result;
    const auto request = [&](const std::string& table) -> wire::StartAcquisition& {
        auto& out = result.emplace_back();
        out.set_id(prefix + "-" + table);
        out.set_table(protocol::table_of(table));
        return out;
    };
    // Without yesterday's year the calendar cannot say what is due.
    std::set<std::string> calendars;
    for (auto at = version.partitions.lower_bound("calendar/");
        at != version.partitions.end() && at->first.starts_with("calendar/");
        ++at)
        calendars.insert(at->first.substr(9, at->first.find('/', 9) - 9));
    const auto current_year = yesterday.substr(0, 4);
    const auto previous_year = std::to_string(std::stoi(current_year) - 1);
    std::string first_calendar = yesterday;
    std::erase_if(calendars, [&](const auto& exchange) {
        const bool has_previous =
            version.partitions.contains("calendar/" + exchange + "/" + previous_year);
        const bool has_current =
            version.partitions.contains("calendar/" + exchange + "/" + current_year);
        if (!has_previous)
            first_calendar = previous_year + "-12-31";
        return has_current;
    });
    if (!calendars.empty()) {
        auto& calendar = request("calendar");
        calendar.set_first_day(first_calendar);
        calendar.set_last_day(yesterday);
        for (const auto& exchange : calendars)
            calendar.add_exchanges(exchange);
    }
    auto open = open_days(store, version, yesterday);
    for (const auto& exchange : calendars)
        for (auto day = day_of(current_year + "-01-01"); day <= day_of(yesterday);
            day += std::chrono::days{1})
            if (std::chrono::weekday(day).iso_encoding() < 6)
                open[exchange].push_back(day_text(day));
    for (const std::string table : {"daily", "minutes", "settlement", "price_ticks"}) {
        std::string first;
        std::vector<std::string> due;
        for (const auto& [holder, holding] :
            holdings(store, version, table, open, empty_keys(directory, table)))
            if (holding.last < holding.through) {
                due.push_back(holder);
                const auto next = day_text(day_of(holding.last) + std::chrono::days{1});
                first = first.empty() ? next : std::min(first, next);
            }
        if (due.empty())
            continue;
        auto& out = request(table);
        out.set_first_day(first);
        // A contract without a last day runs to its delisting.
        if (table == "minutes")
            for (const auto& contract : due)
                out.add_instruments(contract);
        else {
            out.set_last_day(yesterday);
            for (const auto& exchange : due)
                out.add_exchanges(exchange);
        }
    }
    return result;
}

} // namespace asterion::warehouse::acquisition
