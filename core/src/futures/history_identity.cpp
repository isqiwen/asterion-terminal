#include <asterion/futures/history_identity.hpp>
#include <asterion/futures/trading_days.hpp>
#include <algorithm>
#include <array>
#include <stdexcept>
#include <string_view>

namespace asterion {
namespace {

constexpr std::array<std::string_view, 6> venues{"SHFE", "DCE", "CZCE", "CFFEX", "INE", "GFEX"};

} // namespace

std::string HistoryIdentity::key() const
{
    if (std::ranges::find(venues, venue) == venues.end() || product.empty() || product.size() > 8
        || !std::ranges::all_of(
            product,
            [](char c) {
                return c >= 'a' && c <= 'z';
            })
        || delivery_month.size() != 7)
        throw std::invalid_argument("invalid historical contract identity");
    (void)parse_trading_date(delivery_month + "-01");
    return venue + "/" + product + "/" + delivery_month;
}

} // namespace asterion
