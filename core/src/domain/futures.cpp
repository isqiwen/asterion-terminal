#include <asterion/domain/futures.hpp>
#include <algorithm>
#include <array>
#include <stdexcept>
#include <string_view>

namespace asterion {
void FuturesContract::validate() const {
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
  if (delivery_month.size() != 7 || delivery_month[4] != '-' ||
      !std::ranges::all_of(delivery_month.substr(0, 4) + delivery_month.substr(5),
                           [](char c) { return c >= '0' && c <= '9'; }) ||
      delivery_month.substr(0, 4) < "1970" || delivery_month.substr(5) < "01" ||
      delivery_month.substr(5) > "12") {
    throw std::invalid_argument("delivery month must be a valid YYYY-MM");
  }
  const auto suffix = instrument.id.venue == "CZCE"
                          ? delivery_month.substr(3, 1) + delivery_month.substr(5)
                          : delivery_month.substr(2, 2) + delivery_month.substr(5);
  if (instrument.id.symbol != product + suffix) {
    throw std::invalid_argument("contract symbol does not match product and delivery month; "
                                "continuous or dominant aliases are not accepted");
  }
  if (!instrument.quantity_increment.multiple_of(Decimal::parse("1")) ||
      !instrument.multiplier.multiple_of(Decimal::parse("1"))) {
    throw std::invalid_argument(
        "futures quantity increment and multiplier must be positive integers");
  }
}
} // namespace asterion
