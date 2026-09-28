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
OrderLimits::OrderLimits(OrderLimitsConfig config) : config_(config) {
  config_.validate();
}
PluginDescriptor OrderLimits::descriptor() const {
  return {"asterion.risk.order-limits", PluginKind::risk, plugin_contract_version, {}};
}
RiskDecision OrderLimits::evaluate(const PreTradeRiskContext& c) const {
  if (!running_)
    return {RiskReason::unavailable};
  c.order.validate(c.instrument);
  if ((c.offset != Offset::open && c.offset != Offset::close_today &&
       c.offset != Offset::close_yesterday && c.offset != Offset::close) ||
      c.gross_position_quantity < Decimal{} || c.pending_open_quantity < Decimal{} ||
      !c.gross_position_quantity.multiple_of(c.instrument.quantity_increment) ||
      !c.pending_open_quantity.multiple_of(c.instrument.quantity_increment))
    return {RiskReason::invalid_context};
  if (c.order.quantity > config_.max_order_quantity)
    return {RiskReason::order_quantity};
  if (c.working_orders >= config_.max_working_orders)
    return {RiskReason::working_orders};
  if (c.offset == Offset::open) {
    // Subtract from headroom to avoid overflowing sums for an already
    // over-limit account. Reaching the exact configured bound is allowed.
    if (c.gross_position_quantity > config_.max_gross_quantity)
      return {RiskReason::gross_quantity};
    const auto headroom = config_.max_gross_quantity - c.gross_position_quantity;
    if (c.pending_open_quantity > headroom || c.order.quantity > headroom - c.pending_open_quantity)
      return {RiskReason::gross_quantity};
  }
  return {RiskReason::allowed};
}
} // namespace asterion
