#include <asterion/domain/order.hpp>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace asterion {
void LimitOrder::validate(const Instrument& spec) const {
  spec.validate();
  if (id.empty() || id.size() > 128 || instrument != spec.id ||
      (side != Side::buy && side != Side::sell) || quantity <= Decimal{} ||
      !quantity.multiple_of(spec.quantity_increment) ||
      !limit_price.multiple_of(spec.price_increment)) {
    throw std::invalid_argument("invalid limit order");
  }
}
Order::Order(LimitOrder request, Instrument spec)
    : request_(std::move(request)), spec_(std::move(spec)) {
  request_.validate(spec_);
}
void Order::accept() {
  if (state_ != OrderState::pending)
    throw std::logic_error("only pending orders can be accepted");
  state_ = OrderState::accepted;
}
void Order::reject() {
  if (state_ != OrderState::pending)
    throw std::logic_error("only pending orders can be rejected");
  state_ = OrderState::rejected;
}
void Order::cancel() {
  if (state_ != OrderState::accepted && state_ != OrderState::partially_filled) {
    throw std::logic_error("only active orders can be cancelled");
  }
  state_ = OrderState::cancelled;
}
bool Order::apply(const Fill& fill) {
  if (fill.execution_id.empty() || fill.order_id != request_.id) {
    throw std::invalid_argument("invalid execution identity");
  }
  const auto previous = std::ranges::find_if(
      fills_, [&](const auto& value) { return value.execution_id == fill.execution_id; });
  if (previous != fills_.end()) {
    if (*previous == fill)
      return false;
    throw std::invalid_argument("execution identity conflicts with an earlier report");
  }
  if (state_ != OrderState::accepted && state_ != OrderState::partially_filled) {
    throw std::logic_error("order does not accept new fills");
  }
  if (fill.quantity <= Decimal{} || fill.quantity > remaining_quantity() ||
      !fill.quantity.multiple_of(spec_.quantity_increment) ||
      !fill.price.multiple_of(spec_.price_increment) ||
      (request_.side == Side::buy && fill.price > request_.limit_price) ||
      (request_.side == Side::sell && fill.price < request_.limit_price)) {
    throw std::invalid_argument("fill violates order contract");
  }
  const auto next = filled_ + fill.quantity;
  fills_.insert(fill); // Allocate before changing authoritative state.
  filled_ = next;
  state_ = filled_ == request_.quantity ? OrderState::filled : OrderState::partially_filled;
  return true;
}
} // namespace asterion
