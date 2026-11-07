#include "tushare.hpp"
#include "values.hpp"
#include <httplib.h>
#include <algorithm>
#include <regex>
#include <set>
#include <sstream>

namespace asterion::warehouse::acquisition {
namespace {

using Json = nlohmann::json;
using warehouse::Cell;
using values::response_limit;

using values::numeric;
using values::number;

std::string text(const Json& value)
{
    return value.get<std::string>();
}

std::string dashed(const Json& value)
{
    auto day = text(value);
    if (day.size() != 8)
        throw std::invalid_argument("expected compact date");
    day.insert(6, "-");
    day.insert(4, "-");
    day_of(day);
    return day;
}

Cell optional_date(const Json& value)
{
    return value.is_null() || value == "" ? Cell{} : Cell{dashed(value)};
}

Cell label(const Json& value)
{
    if (value.is_null())
        return {};
    auto out = text(value);
    for (auto& c : out)
        if (static_cast<unsigned char>(c) < 32 || c == 127)
            c = ' ';
    const auto first = out.find_first_not_of(' '), last = out.find_last_not_of(' ');
    return first == std::string::npos ? Cell{} : Cell{out.substr(first, last - first + 1)};
}

const std::string& suffix(const std::string& exchange)
{
    static const std::map<std::string, std::string> values{
        {"SHFE", ".SHF"},
        {"DCE", ".DCE"},
        {"CZCE", ".ZCE"},
        {"CFFEX", ".CFX"},
        {"INE", ".INE"},
        {"GFEX", ".GFE"}
    };
    require_exchange(exchange);
    return values.at(exchange);
}

bool dated(const std::string& code, const std::string& exchange)
{
    static const std::regex pattern("[A-Z]{1,3}[0-9]{3,4}");
    const auto& ending = suffix(exchange);
    return code.ends_with(ending)
        && std::regex_match(code.substr(0, code.size() - ending.size()), pattern);
}

std::string czce_code(const std::string& code)
{
    const auto at = code.find_first_of("0123456789");
    return code.substr(0, at) + code.substr(code.size() - 7, 3);
}

} // namespace

Budget::Budget(unsigned per_minute)
    : capacity_(per_minute)
{
    if (!per_minute || per_minute > 500)
        throw std::invalid_argument("requests per minute must be 1 to 500");
}

void Budget::acquire(std::stop_token stop)
{
    std::unique_lock lock(mutex_);
    for (;;) {
        check_stop(stop);
        const auto now = std::chrono::steady_clock::now();
        while (!sent_.empty() && now - sent_.front() >= std::chrono::seconds(60))
            sent_.pop_front();
        if (sent_.size() < capacity_) {
            sent_.push_back(now);
            return;
        }
        changed_.wait_until(lock, stop, sent_.front() + std::chrono::seconds(60), [] {
            return false;
        });
    }
}

std::string tushare_post(const std::string& body, std::stop_token stop)
{
    check_stop(stop);
    httplib::Client client("https://api.tushare.pro");
    client.enable_server_certificate_verification(true);
    client.set_follow_location(false);
    client.set_connection_timeout(10);
    client.set_read_timeout(30);
    client.set_write_timeout(10);
    client.set_max_timeout(std::chrono::seconds(60));
    client.set_payload_max_length(response_limit);
    std::stop_callback cancel(stop, [&] {
        client.stop();
    });
    auto response = client.Post("/", body, "application/json");
    check_stop(stop);
    if (!response || response->status != 200)
        throw SourceError("unavailable", "Tushare HTTPS request failed");
    return std::move(response->body);
}

Tushare::Tushare(std::string token, std::shared_ptr<Budget> budget, std::stop_token stop, Post post)
    : token_(std::move(token)),
      budget_(std::move(budget)),
      stop_(stop),
      post_(std::move(post))
{
    if (token_.empty() || token_.size() > 256
        || std::any_of(token_.begin(), token_.end(), [](unsigned char c) {
               return c <= 32 || c == 127;
           }))
        throw SourceError("invalid_request", "Tushare token is required without whitespace");
    if (!budget_ || !post_)
        throw std::invalid_argument("missing Tushare transport or budget");
}

struct Tushare::Table
{
    std::map<std::string, std::size_t> fields;
    Json rows;

    const Json& at(const Json& row, const std::string& field) const
    {
        return row.at(fields.at(field));
    }
};

Tushare::Table Tushare::ask(
    const std::string& api,
    const Json& parameters,
    const std::string& fields,
    std::size_t limit)
{
    budget_->acquire(stop_);
    ++requests_;
    std::string raw;
    try {
        raw = post_(
            Json{{"api_name", api}, {"params", parameters}, {"fields", fields}, {"token", token_}}
                .dump(),
            stop_);
    } catch (const SourceError&) {
        throw;
    } catch (...) {
        throw SourceError("unavailable", "Tushare transport failed");
    }
    check_stop(stop_);
    try {
        auto root = values::parse(raw);
        const auto code = numeric(root.at("code"));
        if (code != "0") {
            const auto message = root.value("msg", Json{});
            const auto reason = message.is_string() ? text(message) : std::string{};
            std::string failure = code == "2002" ? "permission" : "failed";
            if (reason.find("token") != std::string::npos
                && (reason.find("不正确") != std::string::npos
                    || reason.find("无效") != std::string::npos))
                failure = "invalid_credential";
            else
                for (const auto* word : {"每分钟", "频率", "每小时", "每天最多"})
                    if (reason.find(word) != std::string::npos)
                        failure = "rate_limit";
            throw SourceError(failure, "Tushare refused " + api);
        }
        auto& data = root.at("data");
        Table result{{}, data.at("items")};
        const auto& names = data.at("fields");
        if (!names.is_array() || !result.rows.is_array() || result.rows.size() >= limit)
            throw std::invalid_argument("invalid or possibly truncated table");
        for (std::size_t i = 0; i < names.size(); ++i)
            if (!result.fields.emplace(text(names[i]), i).second)
                throw std::invalid_argument("duplicate field");
        std::istringstream requested(fields);
        for (std::string field; std::getline(requested, field, ',');)
            if (!result.fields.contains(field))
                throw std::invalid_argument("missing field");
        for (const auto& row : result.rows) {
            if (!row.is_array() || row.size() != names.size())
                throw std::invalid_argument("invalid row");
            for (const auto& cell : row) {
                if (cell.is_string() && text(cell).find(token_) != std::string::npos)
                    throw std::invalid_argument("private text");
                if (cell.is_object() || cell.is_array())
                    throw std::invalid_argument("invalid cell");
            }
        }
        return result;
    } catch (const SourceError&) {
        throw;
    } catch (...) {
        throw SourceError("failed", "Tushare returned an invalid or truncated " + api + " table");
    }
}

bool Tushare::supports(std::string_view table, std::string_view exchange) const
{
    require_exchange(exchange);
    return table == "calendar" || table == "contracts" || table == "daily" || table == "minutes";
}

Rows Tushare::contracts(const std::string& exchange)
{
    require_exchange(exchange);
    check_stop(stop_);
    if (contracts_.contains(exchange))
        return contracts_.at(exchange);
    const auto table = ask(
        "fut_basic",
        {{"exchange", exchange}, {"fut_type", "1"}},
        "ts_code,exchange,name,fut_code,multiplier,trade_unit,per_unit,quote_unit,quote_unit_desc,d_mode_desc,list_date,delist_date,d_month,last_ddate,trade_time_desc",
        10000);
    Rows rows;
    std::set<std::string> identities;
    try {
        for (const auto& item : table.rows) {
            const auto at = [&](const char* field) -> const Json& {
                return table.at(item, field);
            };
            const auto code = text(at("ts_code"));
            if (at("exchange") != exchange)
                throw std::invalid_argument("foreign exchange");
            if (!dated(code, exchange))
                continue;
            const auto month = text(at("d_month"));
            if (month.size() != 6)
                throw std::invalid_argument("invalid delivery month");
            const auto delivery = month.substr(0, 4) + "-" + month.substr(4);
            day_of(delivery + "-01");
            auto product = text(at("fut_code"));
            std::transform(product.begin(), product.end(), product.begin(), [](unsigned char c) {
                return std::tolower(c);
            });
            const auto begin = code.find_first_of("0123456789");
            auto prefix = code.substr(0, begin);
            std::transform(prefix.begin(), prefix.end(), prefix.begin(), [](unsigned char c) {
                return std::tolower(c);
            });
            const auto digits = code.substr(begin, code.size() - suffix(exchange).size() - begin);
            if (prefix != product
                || digits != (digits.size() == 3 ? month.substr(3) : month.substr(2)))
                throw std::invalid_argument("inconsistent contract identity");
            const auto identity = exchange + "/" + product + "/" + delivery;
            if (!identities.insert(identity).second)
                throw std::invalid_argument("duplicate contract");
            rows.push_back(
                {exchange,
                    identity,
                    product,
                    delivery,
                    code,
                    label(at("name")),
                    dashed(at("list_date")),
                    dashed(at("delist_date")),
                    optional_date(at("last_ddate")),
                    number(at("multiplier")),
                    number(at("per_unit")),
                    label(at("trade_unit")),
                    label(at("quote_unit")),
                    label(at("quote_unit_desc")),
                    label(at("d_mode_desc")),
                    label(at("trade_time_desc"))});
        }
    } catch (...) {
        throw SourceError("failed", "Tushare returned invalid contract data");
    }
    contracts_[exchange] = rows;
    return rows;
}

Rows Tushare::calendar(const std::string& exchange, int year)
{
    require_exchange(exchange);
    if (year < 1990 || year > 9999)
        throw std::invalid_argument("invalid calendar year");
    const auto table =
        ask("fut_trade_cal",
            {{"exchange", exchange},
                {"start_date", std::to_string(year) + "0101"},
                {"end_date", std::to_string(year) + "1231"}},
            "exchange,cal_date,is_open,pretrade_date",
            2000);
    Rows rows;
    std::set<std::string> seen;
    try {
        for (const auto& item : table.rows) {
            const auto day = dashed(table.at(item, "cal_date"));
            const auto open = numeric(table.at(item, "is_open"));
            if (table.at(item, "exchange") != exchange || day.substr(0, 4) != std::to_string(year)
                || !seen.insert(day).second || (open != "0" && open != "1"))
                throw std::invalid_argument("invalid calendar row");
            rows.push_back(
                {exchange, day, open == "1", optional_date(table.at(item, "pretrade_date"))});
        }
    } catch (...) {
        throw SourceError("failed", "Tushare returned invalid calendar data");
    }
    return rows;
}

Rows Tushare::daily(const std::string& exchange, const std::string& day)
{
    require_exchange(exchange);
    day_of(day);
    const auto catalogue = contracts(exchange);
    auto compact = day;
    std::erase(compact, '-');
    const auto table = ask(
        "fut_daily",
        {{"exchange", exchange}, {"trade_date", compact}},
        "ts_code,trade_date,pre_close,pre_settle,open,high,low,close,settle,vol,amount,oi,oi_chg,delv_settle",
        2000);
    Rows rows;
    std::set<std::string> seen;
    try {
        for (const auto& item : table.rows) {
            const auto at = [&](const char* field) -> const Json& {
                return table.at(item, field);
            };
            const auto code = text(at("ts_code"));
            if (!code.ends_with(suffix(exchange)) || at("trade_date") != compact)
                throw std::invalid_argument("foreign day or exchange");
            if (!dated(code, exchange))
                continue;
            auto found = std::find_if(catalogue.begin(), catalogue.end(), [&](const auto& row) {
                return std::get<std::string>(row[4]) == code;
            });
            const auto live = [&](const auto& row) {
                return std::get<std::string>(row[6]) <= day && day <= std::get<std::string>(row[7]);
            };
            if (exchange == "CZCE" && (found == catalogue.end() || !live(*found))) {
                const auto active =
                    std::find_if(catalogue.begin(), catalogue.end(), [&](const auto& row) {
                        return live(row)
                            && czce_code(std::get<std::string>(row[4])) == czce_code(code);
                    });
                if (active != catalogue.end())
                    found = active;
            }
            if (found == catalogue.end())
                throw std::invalid_argument("unlisted contract");
            const auto identity = std::get<std::string>((*found)[1]);
            if (!seen.insert(identity).second)
                throw std::invalid_argument("duplicate daily row");
            const auto price = [&](const char* field) {
                auto cell = number(at(field));
                return cell == Cell{std::string("0")} ? Cell{} : cell;
            };
            const auto open = price("open"), high = price("high"), low = price("low");
            const auto close = std::holds_alternative<std::monostate>(open)
                    && std::holds_alternative<std::monostate>(high)
                    && std::holds_alternative<std::monostate>(low)
                ? Cell{}
                : price("close");
            rows.push_back(
                {exchange,
                    (*found)[2],
                    identity,
                    day,
                    open,
                    high,
                    low,
                    close,
                    number(at("settle")),
                    number(at("pre_close")),
                    number(at("pre_settle")),
                    number(at("vol")),
                    number(at("amount"), 4),
                    number(at("oi")),
                    number(at("oi_chg")),
                    number(at("delv_settle"))});
        }
    } catch (...) {
        throw SourceError("failed", "Tushare returned invalid daily data");
    }
    return rows;
}

warehouse::Row Tushare::calendar_day(const std::string& exchange, const std::string& day)
{
    const auto key = std::pair{exchange, std::stoi(day.substr(0, 4))};
    if (!calendars_.contains(key)) {
        auto rows = calendar(exchange, key.second);
        auto& dates = calendars_[key];
        for (auto& row : rows) {
            auto date = std::get<std::string>(row[1]);
            dates.emplace(std::move(date), std::move(row));
        }
    }
    const auto& dates = calendars_.at(key);
    if (!dates.contains(day))
        throw SourceError("invalid_data", "Tushare calendar does not cover " + day);
    return dates.at(day);
}

std::string Tushare::trading_day(const std::string& exchange, std::chrono::sys_seconds end)
{
    using namespace std::chrono;
    const auto day = floor<days>(end);
    const auto hour = duration_cast<hours>(end - day).count();
    const auto open = [&](sys_days value) {
        return std::get<bool>(calendar_day(exchange, day_text(value))[2]);
    };
    if (hour >= 6 && hour < 18) {
        if (!open(day))
            throw std::invalid_argument("minute on a closed day");
        return day_text(day);
    }
    const auto evening = hour >= 18 ? day : day - days(1);
    if (!open(evening))
        throw std::invalid_argument("night minute after a closed day");
    for (int n = 1; n <= 31; ++n)
        if (open(evening + days(n)))
            return day_text(evening + days(n));
    throw std::invalid_argument("no trading day after night session");
}

Rows Tushare::minutes(const std::string& contract, const std::string& day)
{
    using namespace std::chrono;
    require_contract(contract);
    const auto first = day_of(day);
    check_stop(stop_);
    if (window_.contract == contract && day >= window_.first && day <= window_.last) {
        const auto at = window_.rows.find(day);
        return at == window_.rows.end() ? Rows{} : at->second;
    }
    const auto exchange = contract.substr(0, contract.find('/'));
    const auto entry = calendar_day(exchange, day);
    const auto catalogue = contracts(exchange);
    const auto listing = std::find_if(catalogue.begin(), catalogue.end(), [&](const auto& row) {
        return std::get<std::string>(row[1]) == contract;
    });
    if (listing == catalogue.end())
        throw SourceError("invalid_data", "Tushare does not list the requested contract");
    if (!std::get<bool>(entry[2]) || day < std::get<std::string>((*listing)[6])
        || day > std::get<std::string>((*listing)[7]))
        return {};
    const auto last = std::min(first + days(27), day_of(std::get<std::string>((*listing)[7])));
    const auto begin = std::holds_alternative<std::monostate>(entry[3])
        ? first - days(1)
        : day_of(std::get<std::string>(entry[3]));
    const sys_seconds start = begin + hours(18),
                      finish = last + hours(15) + std::chrono::minutes(30);
    const auto code = std::get<std::string>((*listing)[4]);
    constexpr std::size_t page_size = 8000;
    Json params{
        {"ts_code", code},
        {"freq", "1min"},
        {"start_date", day_text(begin) + " 18:00:00"},
        {"end_date", day_text(last) + " 15:30:00"},
        {"limit", page_size}
    };
    Window pending{contract, day, day_text(last), {}};
    std::set<sys_seconds> seen;
    for (;;) {
        params["offset"] = seen.size();
        const auto page =
            ask("ft_mins",
                params,
                "ts_code,trade_time,open,high,low,close,vol,amount,oi",
                page_size + 1);
        try {
            for (const auto& item : page.rows) {
                const auto at = [&](const char* field) -> const Json& {
                    return page.at(item, field);
                };
                const auto label = text(at("trade_time"));
                static const std::regex clock("[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:00");
                if (at("ts_code") != code || !std::regex_match(label, clock))
                    throw std::invalid_argument("invalid minute identity or time");
                const auto hour = std::stoi(label.substr(11, 2)),
                           minute = std::stoi(label.substr(14, 2));
                if (hour > 23 || minute > 59 || (exchange != "CFFEX" && hour == 10 && minute == 30))
                    throw std::invalid_argument("invalid minute-end label");
                const sys_seconds end =
                    day_of(label.substr(0, 10)) + hours(hour) + std::chrono::minutes(minute);
                if (end < start || end > finish || !seen.insert(end).second)
                    throw std::invalid_argument("minute outside window or repeated page");
                const auto volume = numeric(at("vol"));
                if (volume.starts_with('-') || volume.find('.') != std::string::npos)
                    throw std::invalid_argument("minute volume must be nonnegative whole lots");
                const auto traded = trading_day(exchange, end);
                if (traded >= pending.first && traded <= pending.last) {
                    const auto timestamp = duration_cast<milliseconds>(
                        (end - hours(8) - std::chrono::minutes(1)).time_since_epoch())
                                               .count();
                    pending.rows[traded].push_back(
                        {exchange,
                            (*listing)[2],
                            contract,
                            traded,
                            static_cast<std::int64_t>(timestamp),
                            false,
                            number(at("open")),
                            number(at("high")),
                            number(at("low")),
                            number(at("close")),
                            volume,
                            Cell{},
                            number(at("oi"))});
                }
            }
        } catch (const SourceError&) {
            throw;
        } catch (...) {
            throw SourceError("invalid_data", "Tushare returned invalid minute data");
        }
        if (page.rows.size() < page_size)
            break;
    }
    for (auto& [date, rows] : pending.rows)
        std::sort(rows.begin(), rows.end(), [](const auto& left, const auto& right) {
            return std::get<std::int64_t>(left[4]) < std::get<std::int64_t>(right[4]);
        });
    window_ = std::move(pending);
    const auto found = window_.rows.find(day);
    return found == window_.rows.end() ? Rows{} : found->second;
}

} // namespace asterion::warehouse::acquisition
