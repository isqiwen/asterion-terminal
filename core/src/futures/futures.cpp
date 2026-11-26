#include <asterion/futures/futures.hpp>
#include <algorithm>
#include <array>
#include <stdexcept>
#include <string_view>

namespace asterion {

bool is_dated_futures_symbol(
    std::string_view venue,
    std::string_view product,
    std::string_view symbol)
{
    if (product.empty() || !symbol.starts_with(product))
        return false;
    const auto suffix = symbol.substr(product.size());
    const std::size_t expected = venue == "CZCE" ? 3U : 4U;
    if (suffix.size() != expected || !std::ranges::all_of(suffix, [](char c) {
            return c >= '0' && c <= '9';
        }))
        return false;
    const auto month = suffix.substr(expected - 2);
    return month >= "01" && month <= "12";
}

bool prohibits_natural_person_delivery_month(std::string_view venue, std::string_view product)
{
    if (venue != "CFFEX")
        return true;
    std::string upper(product);
    for (char& c : upper)
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
    return upper == "T" || upper == "TF" || upper == "TS" || upper == "TL";
}

void FuturesContract::validate() const
{
    instrument.validate();
    constexpr std::array<std::string_view, 6> venues{"SHFE", "DCE", "CZCE", "CFFEX", "INE", "GFEX"};
    if (std::ranges::find(venues, instrument.id.venue) == venues.end()) {
        throw std::invalid_argument(
            "futures contracts currently support SHFE / DCE / CZCE / CFFEX / INE / GFEX only");
    }
    if (product.empty() || product.size() > 8 || !std::ranges::all_of(product, [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
        }))
        throw std::invalid_argument("product code must consist of Latin letters");
    if (delivery_month.empty()) {
        if (instrument.id.symbol != product)
            throw std::invalid_argument(
                "contract symbol does not match product and delivery month; " "continuous or dominant aliases are not accepted");
    } else {
        if (delivery_month.size() != 7 || delivery_month[4] != '-'
            || !std::ranges::all_of(
                delivery_month.substr(0, 4) + delivery_month.substr(5),
                [](char c) {
                    return c >= '0' && c <= '9';
                })
            || delivery_month.substr(0, 4) < "1970" || delivery_month.substr(5) < "01"
            || delivery_month.substr(5) > "12") {
            throw std::invalid_argument("delivery month must be a valid YYYY-MM");
        }
        const auto suffix = instrument.id.venue == "CZCE"
            ? delivery_month.substr(3, 1) + delivery_month.substr(5)
            : delivery_month.substr(2, 2) + delivery_month.substr(5);
        if (instrument.id.symbol != product + suffix) {
            throw std::invalid_argument(
                "contract symbol does not match product and delivery month; " "continuous or dominant aliases are not accepted");
        }
    }
    if (!instrument.quantity_increment.multiple_of(Decimal::parse("1"))
        || !instrument.multiplier.multiple_of(Decimal::parse("1"))) {
        throw std::invalid_argument(
            "futures quantity increment and multiplier must be positive integers");
    }
}

bool FuturesContract::matches(const InstrumentId& id) const
{
    if (!delivery_month.empty())
        return instrument.id == id;
    return instrument.id.venue == id.venue && is_dated_futures_symbol(id.venue, product, id.symbol);
}

Instrument FuturesContract::terms_for(const InstrumentId& id) const
{
    Instrument resolved = instrument;
    resolved.id = id;
    return resolved;
}

std::string
FuturesContract::delivery_month_for(const InstrumentId& id, std::string_view trading_day) const
{
    if (!delivery_month.empty())
        return delivery_month;
    const auto suffix = id.symbol.substr(product.size());
    if (id.venue == "CZCE") {
        const int ref_year = std::stoi(std::string(trading_day.substr(0, 4)));
        const int decade = (ref_year / 10) * 10;
        int year = decade + (suffix[0] - '0');
        if (year < ref_year - 2)
            year += 10;
        else if (year > ref_year + 7)
            year -= 10;
        return std::to_string(year) + "-" + suffix.substr(1, 2);
    }
    return std::string(trading_day.substr(0, 2)) + suffix.substr(0, 2) + "-" + suffix.substr(2, 2);
}

} // namespace asterion
