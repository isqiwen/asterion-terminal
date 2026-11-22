#include "update.hpp"
#include "freshness.hpp"
#include "source.hpp"
#include <asterion/protocol/warehouse.hpp>
#include <algorithm>
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
    std::map<std::string, std::string> market_first;
    for (const std::string market : {"daily/", "minutes/"})
        for (auto at = version.partitions.lower_bound(market);
            at != version.partitions.end() && at->first.starts_with(market);
            ++at) {
            const auto slash = at->first.find('/', market.size());
            const auto exchange = at->first.substr(market.size(), slash - market.size());
            const auto day = at->first.substr(slash + 1, 10);
            auto [it, inserted] = market_first.emplace(exchange, day);
            if (!inserted && day < it->second)
                it->second = day;
        }
    std::string daily_first;
    std::set<std::string> daily_exchanges;
    for (const std::string table :
        {"daily", "minutes", "warehouse_receipts", "member_positions", "inventory"}) {
        const auto empty_table = empty_keys(directory, table);
        std::string first;
        std::set<std::string> due;
        for (const auto& [holder, holding] : holdings(store, version, table, open, empty_table))
            if (!holding.first_missing.empty()) {
                due.insert(holder);
                first =
                    first.empty() ? holding.first_missing : std::min(first, holding.first_missing);
            }
        if (table == "minutes")
            for (const auto& [contract, day] :
                rolled_dominant_minutes(store, version, open, empty_table, yesterday)) {
                due.insert(contract);
                first = first.empty() ? day : std::min(first, day);
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
            if (table == "daily") {
                daily_first = first;
                daily_exchanges.insert(due.begin(), due.end());
            }
            out.set_last_day(yesterday);
            for (const auto& exchange : due)
                out.add_exchanges(exchange);
        }
    }
    // Settlement parameters follow every exchange that has market bars or
    // standalone rows, unless a scheduled daily update already spans them.
    {
        const std::string table = "settlement";
        std::map<std::string, std::set<std::string>> days, answered;
        const auto prefix_key = table + '/';
        for (auto at = version.partitions.lower_bound(prefix_key);
            at != version.partitions.end() && at->first.starts_with(prefix_key);
            ++at) {
            const auto slash = at->first.find('/', prefix_key.size());
            days[at->first.substr(prefix_key.size(), slash - prefix_key.size())].insert(
                at->first.substr(slash + 1, 10));
        }
        for (const auto& key : empty_keys(directory, table)) {
            const auto slash = key.find('/', prefix_key.size());
            answered[key.substr(prefix_key.size(), slash - prefix_key.size())].insert(
                key.substr(slash + 1, 10));
        }
        std::set<std::string> exchanges;
        for (const auto& [exchange, _] : days)
            exchanges.insert(exchange);
        for (const auto& [exchange, _] : market_first)
            exchanges.insert(exchange);
        std::string first;
        std::vector<std::string> due;
        for (const auto& exchange : exchanges) {
            const auto cal = open.find(exchange);
            if (cal == open.end())
                continue;
            const auto held = days.find(exchange);
            const auto market = market_first.find(exchange);
            const auto from = held != days.end() && market != market_first.end()
                ? std::min(*held->second.begin(), market->second)
                : held != days.end() ? *held->second.begin()
                                     : market->second;
            const auto& none = answered[exchange];
            std::string missing;
            for (auto it = std::lower_bound(cal->second.begin(), cal->second.end(), from);
                it != cal->second.end();
                ++it)
                if ((held == days.end() || !held->second.contains(*it)) && !none.contains(*it)) {
                    missing = *it;
                    break;
                }
            if (missing.empty())
                continue;
            if (daily_exchanges.contains(exchange) && daily_first <= missing)
                continue;
            due.push_back(exchange);
            first = first.empty() ? missing : std::min(first, missing);
        }
        if (!due.empty()) {
            auto& out = request(table);
            out.set_first_day(first);
            out.set_last_day(yesterday);
            for (const auto& exchange : due)
                out.add_exchanges(exchange);
        }
    }
    return result;
}

std::optional<wire::StartAcquisition> contracts_request(
    const std::filesystem::path& directory,
    const std::vector<std::string>& exchanges,
    const std::string& day,
    const std::string& id,
    bool missing_only)
{
    const Store store(source_directory(directory, "research"), "research");
    const auto version = store.current();
    wire::StartAcquisition out;
    for (const auto& exchange : exchanges)
        if (!missing_only || !version.partitions.contains("contracts/" + exchange))
            out.add_exchanges(exchange);
    if (out.exchanges().empty())
        return std::nullopt;
    out.set_id(id);
    out.set_table(protocol::table_of("contracts"));
    out.set_first_day(day);
    out.set_last_day(day);
    out.set_refetch(true);
    return out;
}

} // namespace asterion::warehouse::acquisition
