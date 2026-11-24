#include <asterion/futures/trading_days.hpp>
#include <charconv>
#include <stdexcept>

namespace asterion {

std::chrono::year_month_day parse_trading_date(std::string_view value)
{
    if (value.size() != 10 || value[4] != '-' || value[7] != '-')
        throw std::invalid_argument("invalid historical daily trading date");
    const auto number = [&](unsigned offset, unsigned size) {
        unsigned result = 0;
        const auto part = value.substr(offset, size);
        if (part.find_first_not_of("0123456789") != std::string_view::npos)
            throw std::invalid_argument("invalid historical daily trading date");
        const auto [end, error] = std::from_chars(part.data(), part.data() + part.size(), result);
        if (error != std::errc{} || end != part.data() + part.size())
            throw std::invalid_argument("invalid historical daily trading date");
        return result;
    };
    const auto day = std::chrono::year(int(number(0, 4))) / number(5, 2) / number(8, 2);
    if (!day.ok() || day.year() < std::chrono::year(1970) || day.year() > std::chrono::year(9999))
        throw std::invalid_argument("invalid historical daily trading date");
    return day;
}

std::string format_trading_date(std::chrono::year_month_day day)
{
    if (!day.ok() || day.year() < std::chrono::year(1970) || day.year() > std::chrono::year(9999))
        throw std::invalid_argument("invalid historical daily trading date");
    const unsigned y = static_cast<unsigned>(int(day.year()));
    const unsigned m = static_cast<unsigned>(day.month());
    const unsigned d = static_cast<unsigned>(day.day());
    char text[10]{
        static_cast<char>('0' + y / 1000 % 10),
        static_cast<char>('0' + y / 100 % 10),
        static_cast<char>('0' + y / 10 % 10),
        static_cast<char>('0' + y % 10),
        '-',
        static_cast<char>('0' + m / 10),
        static_cast<char>('0' + m % 10),
        '-',
        static_cast<char>('0' + d / 10),
        static_cast<char>('0' + d % 10)
    };
    return std::string(text, sizeof(text));
}

std::int64_t trading_day_close_ns(std::string_view day)
{
    using namespace std::chrono;
    // 15:00 Asia/Shanghai = 07:00 UTC.
    return duration_cast<nanoseconds>(
        (sys_days(parse_trading_date(day)) + hours(7)).time_since_epoch())
        .count();
}

} // namespace asterion
