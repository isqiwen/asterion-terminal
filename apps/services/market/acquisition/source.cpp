#include "source.hpp"
#include <array>
#include <algorithm>
#include <cstdio>
#include <regex>

namespace asterion::market_data::acquisition {

void check_stop(std::stop_token stop)
{
    if (stop.stop_requested())
        throw SourceError("cancelled", "acquisition cancelled");
}

std::chrono::sys_days day_of(std::string_view text)
{
    const std::string value(text);
    static const std::regex syntax("[0-9]{4}-[0-9]{2}-[0-9]{2}");
    if (!std::regex_match(value, syntax))
        throw std::invalid_argument("date must be YYYY-MM-DD");
    const std::chrono::year_month_day day{
        std::chrono::year(std::stoi(value.substr(0, 4))),
        std::chrono::month(std::stoi(value.substr(5, 2))),
        std::chrono::day(std::stoi(value.substr(8, 2)))
    };
    if (!day.ok() || day.year() == std::chrono::year(0))
        throw std::invalid_argument("invalid calendar date");
    return std::chrono::sys_days(day);
}

std::string day_text(std::chrono::sys_days value)
{
    const std::chrono::year_month_day day(value);
    std::array<char, 16> text{};
    std::snprintf(
        text.data(),
        text.size(),
        "%04d-%02u-%02u",
        int(day.year()),
        unsigned(day.month()),
        unsigned(day.day()));
    return text.data();
}

void require_exchange(std::string_view exchange)
{
    constexpr std::array exchanges{"SHFE", "DCE", "CZCE", "CFFEX", "INE", "GFEX"};
    if (std::find(exchanges.begin(), exchanges.end(), exchange) == exchanges.end())
        throw std::invalid_argument("unknown futures exchange");
}

void require_contract(std::string_view contract)
{
    static const std::regex syntax("([A-Z]+)/[a-z]{1,3}/[0-9]{4}-[0-9]{2}");
    const std::string value(contract);
    if (!std::regex_match(value, syntax))
        throw std::invalid_argument("contract must be EXCHANGE/product/YYYY-MM");
    require_exchange(value.substr(0, value.find('/')));
    day_of(value.substr(value.rfind('/') + 1) + "-01");
}

std::optional<std::string>
contract_of(const std::string& exchange, const std::string& code, const std::string& day)
{
    static const std::regex syntax("([A-Za-z]{1,3})([0-9]{3,4})");
    std::smatch match;
    if (!std::regex_match(code, match, syntax))
        return {};
    auto product = match[1].str(), digits = match[2].str();
    std::transform(product.begin(), product.end(), product.begin(), [](unsigned char c) {
        return std::tolower(c);
    });
    int year;
    if (digits.size() == 4)
        year = 2000 + std::stoi(digits.substr(0, 2));
    else {
        year = std::stoi(day.substr(0, 4)) - 1;
        while (year % 10 != digits[0] - '0')
            ++year;
    }
    const auto identity = exchange + "/" + product + "/" + std::to_string(year) + "-"
        + digits.substr(digits.size() - 2);
    require_contract(identity);
    return identity;
}

Rows Source::calendar(const std::string&, int)
{
    throw SourceError("unsupported", "source does not provide calendars");
}

Rows Source::contracts(const std::string&)
{
    throw SourceError("unsupported", "source does not provide contracts");
}

Rows Source::daily(const std::string&, const std::string&)
{
    throw SourceError("unsupported", "source does not provide daily bars");
}

Rows Source::minutes(const std::string&, const std::string&)
{
    throw SourceError("unsupported", "source does not provide minutes");
}

Rows Source::settlement(const std::string&, const std::string&)
{
    throw SourceError("unsupported", "source does not provide settlement parameters");
}

Rows Source::price_ticks(const std::string&, const std::string&)
{
    throw SourceError("unsupported", "source does not provide historical price ticks");
}

} // namespace asterion::market_data::acquisition
