#include <asterion/domain/futures.hpp>
#include <algorithm>
#include <array>
#include <stdexcept>
#include <string_view>

namespace asterion {
void FuturesContract::validate() const {
  instrument.validate();
  constexpr std::array<std::string_view, 6> venues{"SHFE", "DCE", "CZCE", "CFFEX", "INE", "GFEX"};
  if (instrument.asset_class != AssetClass::futures ||
      std::ranges::find(venues, instrument.id.venue) == venues.end()) {
    throw std::invalid_argument("当前期货契约仅支持 SHFE / DCE / CZCE / CFFEX / INE / GFEX");
  }
  if (product.empty() || product.size() > 8 || !std::ranges::all_of(product, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
      }))
    throw std::invalid_argument("品种代码必须为英文字母");
  if (delivery_month.size() != 7 || delivery_month[4] != '-' ||
      !std::ranges::all_of(delivery_month.substr(0, 4) + delivery_month.substr(5),
                           [](char c) { return c >= '0' && c <= '9'; }) ||
      delivery_month.substr(0, 4) < "1970" || delivery_month.substr(5) < "01" ||
      delivery_month.substr(5) > "12") {
    throw std::invalid_argument("交割月份必须为有效 YYYY-MM");
  }
  const auto suffix = instrument.id.venue == "CZCE"
                          ? delivery_month.substr(3, 1) + delivery_month.substr(5)
                          : delivery_month.substr(2, 2) + delivery_month.substr(5);
  if (instrument.id.symbol != product + suffix) {
    throw std::invalid_argument("实际合约代码与品种、交割月份不一致；不接受主力或连续合约别名");
  }
  if (!instrument.quantity_increment.multiple_of(Decimal::parse("1")) ||
      !instrument.multiplier.multiple_of(Decimal::parse("1"))) {
    throw std::invalid_argument("期货手数步长和合约乘数必须为正整数");
  }
}
} // namespace asterion
