#include "freshness.hpp"
#include "parquet_engine.hpp"
#include <asterion/futures/trading_days.hpp>
#include <chrono>
#include <fstream>
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

// The holder of a day-partitioned key, EXCHANGE or (minutes) EXCHANGE/product/YYYY-MM,
// and its day: table/EXCHANGE/day[/product/delivery].
std::pair<std::string, std::string> holder_of(const std::string& key, std::size_t prefix)
{
    const auto exchange = key.find('/', prefix);
    const auto day = key.find('/', exchange + 1);
    auto holder = key.substr(prefix, exchange - prefix);
    if (day != std::string::npos)
        holder += key.substr(day);
    return {holder, key.substr(exchange + 1, day - exchange - 1)};
}

} // namespace

std::filesystem::path empty_file(const std::filesystem::path& directory, const std::string& table)
{
    return directory / "acquisition" / ("empty-" + table + ".txt");
}

std::set<std::string> empty_keys(const std::filesystem::path& directory, const std::string& table)
{
    std::set<std::string> result;
    std::ifstream input(empty_file(directory, table));
    for (std::string line; std::getline(input, line);)
        result.insert(line);
    return result;
}

std::string yesterday()
{
    const auto today = std::chrono::floor<std::chrono::days>(
        std::chrono::system_clock::now() + std::chrono::hours(8));
    return format_trading_date(std::chrono::year_month_day{today - std::chrono::days{1}});
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

std::map<std::string, Holding> holdings(
    const Store& store,
    const Version& version,
    const std::string& table,
    const OpenDays& open,
    const std::set<std::string>& empty)
{
    std::map<std::string, std::set<std::string>> days, answered;
    const auto prefix = table + '/';
    for (auto at = version.partitions.lower_bound(prefix);
        at != version.partitions.end() && at->first.starts_with(prefix);
        ++at) {
        const auto [holder, day] = holder_of(at->first, prefix.size());
        days[holder].insert(day);
    }
    for (const auto& key : empty) {
        const auto [holder, day] = holder_of(key, prefix.size());
        answered[holder].insert(day);
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
        const auto& none = answered[holder];
        out.last = std::max(*held.rbegin(), none.empty() ? std::string{} : *none.rbegin());
        const auto calendar = open.find(holder.substr(0, holder.find('/')));
        if (calendar == open.end())
            continue;
        out.through = calendar->second.back();
        if (const auto at = delisted.find(holder); at != delisted.end())
            out.through = std::min(out.through, at->second);
        for (const auto& day : calendar->second)
            if (day >= *held.begin() && day <= out.through && !held.contains(day)) {
                if (none.contains(day))
                    ++out.empty;
                else {
                    ++out.missing;
                    if (out.first_missing.empty())
                        out.first_missing = day;
                }
            }
    }
    return result;
}

std::map<std::string, std::string> rolled_dominant_minutes(
    const Store& store,
    const Version& version,
    const OpenDays& open,
    const std::set<std::string>& empty,
    const std::string& through)
{
    std::set<std::string> held_contracts;
    std::map<std::string, std::string> latest_contract;
    std::map<std::string, std::string> first_minute;
    for (auto at = version.partitions.lower_bound("minutes/");
        at != version.partitions.end() && at->first.starts_with("minutes/");
        ++at) {
        const auto& key = at->first;
        // minutes/<EXCHANGE>/<day>/<product>/<delivery>
        const auto ex_end = key.find('/', 8);
        const auto day_end = key.find('/', ex_end + 1);
        const auto prod_end = key.find('/', day_end + 1);
        if (ex_end == std::string::npos || day_end == std::string::npos
            || prod_end == std::string::npos)
            continue;
        const auto exchange = key.substr(8, ex_end - 8);
        const auto day = key.substr(ex_end + 1, day_end - ex_end - 1);
        const auto product = key.substr(day_end + 1, prod_end - day_end - 1);
        const auto delivery = key.substr(prod_end + 1);
        const auto contract = exchange + '/' + product + '/' + delivery;
        const auto prod_key = exchange + '/' + product;
        held_contracts.insert(contract);
        auto& latest = latest_contract[prod_key];
        if (latest.empty() || contract > latest)
            latest = contract;
        auto& earliest = first_minute[contract];
        if (earliest.empty() || day < earliest)
            earliest = day;
    }
    if (latest_contract.empty())
        return {};
    std::map<std::string, std::string> exchange_floor;
    for (const auto& [prod, contract] : latest_contract) {
        const auto exchange = prod.substr(0, prod.find('/'));
        const auto lookback = format_trading_date(
            std::chrono::year_month_day{
                std::chrono::sys_days{parse_trading_date(first_minute.at(contract))}
                - std::chrono::days{15}
            });
        auto [it, inserted] = exchange_floor.emplace(exchange, lookback);
        if (!inserted && lookback < it->second)
            it->second = lookback;
    }
    std::string daily_files, contract_files;
    for (auto at = version.partitions.lower_bound("daily/");
        at != version.partitions.end() && at->first.starts_with("daily/");
        ++at) {
        const auto slash = at->first.find('/', 6);
        const auto exchange = at->first.substr(6, slash - 6);
        const auto floor = exchange_floor.find(exchange);
        if (floor == exchange_floor.end())
            continue;
        const auto period = at->first.substr(slash + 1);
        if (period < floor->second.substr(0, period.size()))
            continue;
        daily_files +=
            (daily_files.empty() ? "" : ",") + parquet::literal(store.file(version, at->first));
    }
    if (daily_files.empty())
        return {};
    for (const auto& [exchange, _] : exchange_floor) {
        const auto key = "contracts/" + exchange;
        if (version.partitions.contains(key))
            contract_files +=
                (contract_files.empty() ? "" : ",") + parquet::literal(store.file(version, key));
    }
    parquet::Engine engine;
    std::map<std::string, std::pair<std::string, std::string>> lives;
    if (!contract_files.empty()) {
        const auto rows = engine.run(
            "SELECT contract, strftime(list_date, '%Y-%m-%d')," " strftime(delist_date, '%Y-%m-%d') FROM read_parquet(["
            + contract_files + "])");
        for (duckdb::idx_t r = 0; r < rows->RowCount(); ++r)
            lives[rows->GetValue(0, r).ToString()] = {
                rows->GetValue(1, r).IsNull() ? std::string{} : rows->GetValue(1, r).ToString(),
                rows->GetValue(2, r).IsNull() ? std::string{} : rows->GetValue(2, r).ToString()
            };
    }

    struct Observation
    {
        double open_interest = 0;
        bool settled = false;
    };

    std::map<std::string, std::map<std::string, std::map<std::string, Observation>>> by_product;
    const auto rows = engine.run(
        "SELECT exchange || '/' || product, contract, strftime(day, '%Y-%m-%d')," " CAST(open_interest AS DOUBLE), settlement IS NOT NULL FROM read_parquet(["
        + daily_files + "]) WHERE day <= DATE '" + through
        + "' AND open_interest IS NOT NULL ORDER BY 1, 3, 2");
    for (duckdb::idx_t r = 0; r < rows->RowCount(); ++r) {
        const auto prod = rows->GetValue(0, r).ToString();
        const auto latest = latest_contract.find(prod);
        if (latest == latest_contract.end())
            continue;
        const auto contract = rows->GetValue(1, r).ToString();
        if (contract < latest->second)
            continue;
        by_product[prod][rows->GetValue(2, r).ToString()][contract] = {
            rows->GetValue(3, r).GetValue<double>(),
            rows->GetValue(4, r).GetValue<bool>()
        };
    }
    std::map<std::string, std::string> rolled;
    for (const auto& [prod, days_map] : by_product) {
        const auto& anchor_contract = latest_contract.at(prod);
        const auto& start_day = first_minute.at(anchor_contract);
        std::vector<std::string> days;
        std::string previous;
        std::set<std::string> all_contracts;
        for (const auto& [day, obs] : days_map) {
            for (const auto& [c, _] : obs)
                all_contracts.insert(c);
            if (day < start_day)
                previous = day;
            else
                days.push_back(day);
        }
        if (days.empty())
            continue;
        if (!previous.empty())
            days.insert(days.begin(), previous);
        const auto alive_after = [&](const std::string& contract, const std::string& day) {
            const auto it = lives.find(contract);
            return it == lives.end() || it->second.second.empty() || it->second.second > day;
        };
        const auto has_later_alive = [&](const std::string& contract, const std::string& day) {
            for (auto it = all_contracts.upper_bound(contract); it != all_contracts.end(); ++it)
                if (alive_after(*it, day))
                    return true;
            return false;
        };
        std::string current = anchor_contract;
        for (std::size_t i = 1; i < days.size(); ++i) {
            const auto& day = days[i];
            const auto& prev = days[i - 1];
            const auto& prev_obs = days_map.at(prev);
            const auto& day_obs = days_map.at(day);
            std::string chosen;
            double best_oi = -1.0;
            for (const auto& [contract, prev_data] : prev_obs) {
                if (contract < current)
                    continue;
                const auto on_day = day_obs.find(contract);
                if (on_day == day_obs.end() || !on_day->second.settled)
                    continue;
                if (!alive_after(contract, day) && has_later_alive(contract, day))
                    continue;
                if (chosen.empty() || prev_data.open_interest > best_oi) {
                    chosen = contract;
                    best_oi = prev_data.open_interest;
                }
            }
            if (!chosen.empty() && chosen > current) {
                current = chosen;
                if (!held_contracts.contains(current))
                    rolled.try_emplace(current, prev);
            }
        }
        // Check the latest stored day too so a crossover on yesterday brings
        // the incoming dominant contract's pre-roll day immediately.
        const auto& last_day = days.back();
        if (last_day >= start_day) {
            std::string next_dom;
            double best_oi = -1.0;
            for (const auto& [contract, data] : days_map.at(last_day)) {
                if (contract < current || !alive_after(contract, last_day))
                    continue;
                if (next_dom.empty() || data.open_interest > best_oi) {
                    next_dom = contract;
                    best_oi = data.open_interest;
                }
            }
            if (!next_dom.empty() && next_dom > current && !held_contracts.contains(next_dom))
                rolled.try_emplace(next_dom, last_day);
        }
    }
    std::map<std::string, std::string> result;
    for (const auto& [contract, begin_day] : rolled) {
        const auto exchange = contract.substr(0, contract.find('/'));
        const auto cal = open.find(exchange);
        if (cal == open.end() || cal->second.empty())
            continue;
        std::string from = begin_day;
        std::string until = cal->second.back();
        if (const auto life = lives.find(contract); life != lives.end()) {
            if (!life->second.first.empty())
                from = std::max(from, life->second.first);
            if (!life->second.second.empty())
                until = std::min(until, life->second.second);
        }
        const auto suffix = contract.substr(exchange.size());
        for (auto it = std::lower_bound(cal->second.begin(), cal->second.end(), from);
            it != cal->second.end() && *it <= until;
            ++it) {
            if (!empty.contains("minutes/" + exchange + "/" + *it + suffix)) {
                result.emplace(contract, *it);
                break;
            }
        }
    }
    return result;
}

} // namespace asterion::warehouse
