#include "order_limits.hpp"
#include <limits>
#include <stdexcept>
namespace asterion {
void OrderLimitsConfig::validate() const {
  if (max_order_quantity <= Decimal{} || max_gross_quantity <= Decimal{} || max_working_orders == 0)
    throw std::invalid_argument("risk limits must be positive");
}
Json encode_order_limits(const OrderLimitsConfig& config) {
  config.validate();
  return Json{{"max_order_quantity", config.max_order_quantity.str()},
              {"max_gross_quantity", config.max_gross_quantity.str()},
              {"max_working_orders", config.max_working_orders}};
}
OrderLimitsConfig decode_order_limits(const Json& value) {
  require_fields(value, {"max_order_quantity", "max_gross_quantity", "max_working_orders"});
  if (!value.at("max_working_orders").is_number_unsigned() ||
      value.at("max_working_orders").get<std::uint64_t>() > std::numeric_limits<std::size_t>::max())
    throw std::invalid_argument("risk working order limit must be an unsigned integer");
  auto decimal = [&](const char* key) {
    const auto text = value.at(key).get<std::string>();
    const auto result = Decimal::parse(text);
    if (result.str() != text)
      throw std::invalid_argument("risk quantity must use canonical decimal text");
    return result;
  };
  OrderLimitsConfig result{decimal("max_order_quantity"), decimal("max_gross_quantity"),
                           value.at("max_working_orders").get<std::size_t>()};
  result.validate();
  return result;
}
} // namespace asterion
