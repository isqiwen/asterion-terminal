#include "order_limits.hpp"
#include <limits>
#include <stdexcept>
namespace asterion {
OrderLimits::OrderLimits(OrderLimitsConfig config) : config_(config) {
  config_.validate();
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
