#include "freshness.hpp"
#include "parquet_engine.hpp"
#include <chrono>
#include <format>
#include <set>

namespace asterion::warehouse {

namespace {

// The files of a version's partitions under `prefix`, as a Parquet list.
std::string files(const Store& store, const Version& version, const std::string& prefix)
{
    std::string result;
    for (auto at = version.partitions.lower_bound(prefix);
        at != version.partitions.end() && at->first.starts_with(prefix);
        ++at)
        result += (result.empty() ? "" : ",") + parquet::literal(store.file(version, at->first));
    return result;
}

} // namespace

std::string yesterday()
{
    const auto today = std::chrono::floor<std::chrono::days>(
        std::chrono::system_clock::now() + std::chrono::hours(8));
    return std::format("{:%F}", today - std::chrono::days{1});
}

OpenDays open_days(const Store& store, const Version& version, const std::string& through)
{
    OpenDays result;
    const auto calendar = files(store, version, "calendar/");
    if (calendar.empty())
        return result;
    parquet::Engine engine;
    // `through` is a YYYY-MM-DD this service formatted.
    const auto rows = engine.run(
        "SELECT exchange, strftime(day, '%Y-%m-%d') FROM read_parquet([" + calendar
        + "]) WHERE open AND day <= DATE '" + through + "' ORDER BY 1, 2");
    for (duckdb::idx_t row = 0; row < rows->RowCount(); ++row)
        result[rows->GetValue(0, row).ToString()].push_back(rows->GetValue(1, row).ToString());
    return result;
}

std::map<std::string, Holding>
holdings(const Store& store, const Version& version, const std::string& table, const OpenDays& open)
{
    // table/EXCHANGE/day, and /product/delivery for minutes.
    std::map<std::string, std::set<std::string>> days;
    const auto prefix = table + '/';
    for (auto at = version.partitions.lower_bound(prefix);
        at != version.partitions.end() && at->first.starts_with(prefix);
        ++at) {
        const auto& key = at->first;
        const auto exchange = key.find('/', prefix.size());
        const auto day = key.find('/', exchange + 1);
        auto holder = key.substr(prefix.size(), exchange - prefix.size());
        if (day != std::string::npos)
            holder += key.substr(day);
        days[holder].insert(key.substr(exchange + 1, day - exchange - 1));
    }
    std::map<std::string, std::string> delisted;
    if (table == "minutes" && !days.empty()) {
        const auto contracts = files(store, version, "contracts/");
        if (!contracts.empty()) {
            parquet::Engine engine;
            const auto rows = engine.run(
                "SELECT contract, strftime(delist_date, '%Y-%m-%d') FROM read_parquet([" + contracts
                + "]) WHERE delist_date IS NOT NULL");
            for (duckdb::idx_t row = 0; row < rows->RowCount(); ++row)
                delisted[rows->GetValue(0, row).ToString()] = rows->GetValue(1, row).ToString();
        }
    }
    std::map<std::string, Holding> result;
    for (const auto& [holder, held] : days) {
        auto& out = result[holder];
        out.last = *held.rbegin();
        const auto calendar = open.find(holder.substr(0, holder.find('/')));
        if (calendar == open.end())
            continue;
        out.through = calendar->second.back();
        if (const auto at = delisted.find(holder); at != delisted.end())
            out.through = std::min(out.through, at->second);
        for (const auto& day : calendar->second)
            if (day >= *held.begin() && day <= out.through && !held.contains(day))
                ++out.missing;
    }
    return result;
}

} // namespace asterion::warehouse
