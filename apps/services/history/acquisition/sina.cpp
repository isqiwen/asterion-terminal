#include "sina.hpp"
#include <asterion/history.pb.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>

namespace asterion::history::acquisition {

namespace {

using Json = nlohmann::json;

// The service caps a minute answer here, so its oldest day may be cut short.
constexpr std::size_t minute_limit = 1023;

// SHFE/rb/2027-01 is RB2701, on every exchange.
std::string symbol(const std::string& contract)
{
    const auto first = contract.find('/'), last = contract.rfind('/');
    auto product = contract.substr(first + 1, last - first - 1);
    std::transform(product.begin(), product.end(), product.begin(), [](unsigned char c) {
        return std::toupper(c);
    });
    return product + contract.substr(last + 3, 2) + contract.substr(last + 6, 2);
}

std::string product_of(const std::string& contract)
{
    const auto first = contract.find('/'), last = contract.rfind('/');
    return contract.substr(first + 1, last - first - 1);
}

// `var _x=([...]);` after a comment line, or `(null)` for an unknown contract.
Json rows_of(const std::string& body)
{
    const auto open = body.find('('), close = body.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close < open)
        throw SourceError("invalid_data", "unreadable Sina answer");
    try {
        auto rows = Json::parse(body.substr(open + 1, close - open - 1));
        if (rows.is_null())
            return Json::array();
        if (!rows.is_array())
            throw SourceError("invalid_data", "unreadable Sina answer");
        return rows;
    } catch (const Json::exception&) {
        throw SourceError("invalid_data", "unreadable Sina answer");
    }
}

std::string text(const Json& row, const char* field)
{
    if (!row.contains(field) || !row.at(field).is_string())
        throw SourceError("invalid_data", "Sina answer lacks " + std::string(field));
    return row.at(field).get<std::string>();
}

// Night minutes belong to the next trading day. Exchanges hold no night session
// before a holiday, so the next weekday is that day.
std::string trading_day(const std::string& stamp)
{
    auto date = day_of(stamp.substr(0, 10));
    const auto hour = std::stoi(stamp.substr(11, 2));
    if (hour >= 18) {
        date += std::chrono::days(1);
    } else if (hour >= 3) {
        return day_text(date);
    }
    while (std::chrono::weekday(date).iso_encoding() >= 6)
        date += std::chrono::days(1);
    return day_text(date);
}

} // namespace

Sina::Sina(
    std::vector<std::string> exchanges,
    std::filesystem::path snapshots,
    std::shared_ptr<Budget> budget,
    std::stop_token stop,
    Fetch fetch)
    : exchanges_(std::move(exchanges)),
      snapshots_(std::move(snapshots)),
      budget_(std::move(budget)),
      stop_(stop),
      fetch_(std::move(fetch))
{}

bool Sina::supports(std::string_view table, std::string_view exchange) const
{
    return (table == "daily" || table == "minutes") && exchange != "CFFEX"
        && std::find(exchanges_.begin(), exchanges_.end(), exchange) != exchanges_.end();
}

std::string
Sina::get(const std::string& service, const std::string& contract, const std::string& suffix)
{
    budget_->acquire(stop_);
    ++requests_;
    const auto reply = fetch_(
        {"https://stock2.finance.sina.com.cn",
            "/futures/api/jsonp.php/var%20_x=/InnerFuturesNewService." + service
                + "?symbol=" + symbol(contract) + suffix,
            {},
            {}},
        stop_);
    check_stop(stop_);
    if (reply.status != 200)
        throw SourceError(
            "unavailable",
            "Sina refused (HTTP " + std::to_string(reply.status) + ")");
    return reply.body;
}

Rows Sina::daily(const std::string& exchange, const std::string& day)
{
    auto compact = day;
    std::erase(compact, '-');
    const auto file = snapshots_ / (compact + ".pb");
    if (!std::filesystem::exists(file))
        return {};
    std::ifstream input(file, std::ios::binary);
    ::history::Catalog catalog;
    if (!catalog.ParseFromIstream(&input))
        throw SourceError("invalid_data", "damaged contract snapshot: " + file.string());
    Rows rows;
    for (const auto& item : catalog.contracts()) {
        const auto& contract = item.contract();
        if (!contract.starts_with(exchange + "/"))
            continue;
        if (!daily_.contains(contract)) {
            auto& days = daily_[contract];
            for (const auto& row : rows_of(get("getDailyKLine", contract))) {
                const auto date = text(row, "d");
                days[date] = {
                    exchange,
                    product_of(contract),
                    contract,
                    date,
                    text(row, "o"),
                    text(row, "h"),
                    text(row, "l"),
                    text(row, "c"),
                    text(row, "s"),
                    {},
                    {},
                    text(row, "v"),
                    {},
                    text(row, "p"),
                    {},
                    {}
                };
            }
        }
        if (const auto at = daily_[contract].find(day); at != daily_[contract].end())
            rows.push_back(at->second);
    }
    return rows;
}

Rows Sina::minutes(const std::string& contract, const std::string& day)
{
    require_contract(contract);
    if (!minutes_.contains(contract)) {
        auto& days = minutes_[contract];
        const auto answer = rows_of(get("getFewMinLine", contract, "&type=1"));
        const auto exchange = contract.substr(0, contract.find('/'));
        for (const auto& row : answer) {
            const auto stamp = text(row, "d");
            if (stamp.size() != 19)
                throw SourceError("invalid_data", "unreadable Sina minute time");
            // Labelled by the minute's end, in Beijing time.
            const auto end = day_of(stamp.substr(0, 10))
                + std::chrono::hours(std::stoi(stamp.substr(11, 2)))
                + std::chrono::minutes(std::stoi(stamp.substr(14, 2)));
            const auto start = std::chrono::duration_cast<std::chrono::milliseconds>(
                (end - std::chrono::hours(8) - std::chrono::minutes(1)).time_since_epoch())
                                   .count();
            const auto traded = trading_day(stamp);
            days[traded].push_back(
                {exchange,
                    product_of(contract),
                    contract,
                    traded,
                    static_cast<std::int64_t>(start),
                    false,
                    text(row, "o"),
                    text(row, "h"),
                    text(row, "l"),
                    text(row, "c"),
                    text(row, "v"),
                    {},
                    text(row, "p")});
        }
        if (answer.size() >= minute_limit && !days.empty())
            for (auto& bar : days.begin()->second)
                bar[5] = true;
    }
    const auto at = minutes_[contract].find(day);
    return at == minutes_[contract].end() ? Rows{} : at->second;
}

} // namespace asterion::history::acquisition
