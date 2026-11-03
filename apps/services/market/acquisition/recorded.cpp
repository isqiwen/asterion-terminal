#include "recorded.hpp"
#include "recording/reader.hpp"
#include <algorithm>
#include <set>
#include <thread>

namespace asterion::market_data::acquisition {

Recorded::Recorded(
    std::filesystem::path root,
    std::shared_ptr<std::shared_mutex> archive,
    std::stop_token stop)
    : root_(std::move(root)),
      archive_(std::move(archive)),
      stop_(stop)
{}

bool Recorded::supports(std::string_view table, std::string_view exchange) const
{
    require_exchange(exchange);
    return table == "minutes";
}

MinuteBatch Recorded::minutes(const std::string& contract, const std::string& day)
{
    require_contract(contract);
    day_of(day);
    check_stop(stop_);
    const auto first = contract.find('/'), last = contract.rfind('/');
    const auto exchange = contract.substr(0, first);
    const auto product = contract.substr(first + 1, last - first - 1);
    const auto month = contract.substr(last + 1);
    auto symbol =
        product + (exchange == "CZCE" ? month.substr(3, 1) : month.substr(2, 2)) + month.substr(5);
    if (exchange == "CZCE" || exchange == "CFFEX")
        std::transform(symbol.begin(), symbol.end(), symbol.begin(), [](unsigned char c) {
            return std::toupper(c);
        });
    // CZCE's three-digit code repeats each decade. Never label another
    // delivery's recording as the requested contract.
    if (contract_of(exchange, symbol, day) != contract)
        throw SourceError(
            "invalid_data",
            "recorded symbol does not identify this delivery on the requested day");
    auto compact = day;
    std::erase(compact, '-');
    recording::proto::MinutesQuery query;
    query.set_trading_day(compact);
    query.set_venue(exchange);
    query.set_symbol(symbol);
    MinuteBatch result;
    // Compaction replaces archive files; the live feed and recording writer
    // do not acquire this lock and continue independently of historical reads.
    std::shared_lock lock(*archive_, std::defer_lock);
    while (!lock.try_lock()) {
        check_stop(stop_);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check_stop(stop_);
    recording::Reader reader(root_);
    ++requests_;
    const auto bars = reader.minutes(query);
    if (bars.bars().empty())
        return result;
    const auto number = [](const std::string& value) -> warehouse::Cell {
        return value.empty() ? warehouse::Cell{} : warehouse::Cell{value};
    };
    for (const auto& bar : bars.bars()) {
        check_stop(stop_);
        result.rows.push_back(
            {exchange,
                product,
                contract,
                day,
                bar.start_ms(),
                bar.partial(),
                number(bar.open()),
                number(bar.high()),
                number(bar.low()),
                number(bar.close()),
                std::to_string(bar.volume()),
                number(bar.average_price()),
                number(bar.open_interest())});
    }
    // These are service observation intervals, not proof that this contract
    // traded throughout a segment. Preserve gaps and unfinished segments.
    for (const auto& segment : reader.segments(compact)) {
        check_stop(stop_);
        result.observations.push_back(
            {exchange,
                contract,
                day,
                segment.instance(),
                static_cast<std::int64_t>(segment.number()),
                segment.first_received_ms(),
                segment.last_received_ms(),
                static_cast<std::int64_t>(segment.ticks()),
                segment.end()});
    }
    return result;
}

std::vector<std::string> Recorded::minute_contracts(const std::string& day)
{
    day_of(day);
    auto compact = day;
    std::erase(compact, '-');
    std::shared_lock lock(*archive_, std::defer_lock);
    while (!lock.try_lock()) {
        check_stop(stop_);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::set<std::string> contracts;
    // A symbol that names no delivery on that day is left out, as minutes() would refuse it.
    for (const auto& id : recording::Reader(root_).instruments(compact))
        if (const auto contract = contract_of(id.venue, id.symbol, day))
            contracts.insert(*contract);
    return {contracts.begin(), contracts.end()};
}

} // namespace asterion::market_data::acquisition
