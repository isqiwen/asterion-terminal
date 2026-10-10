#include <asterion/domain/position_target.hpp>
#include <algorithm>
#include <stdexcept>
namespace asterion {
std::string_view position_sides_name(PositionSides sides) noexcept {
  switch (sides) {
  case PositionSides::long_only:
    return "long";
  case PositionSides::short_only:
    return "short";
  case PositionSides::both:
    break;
  }
  return "both";
}
PositionSides parse_position_sides(std::string_view name) {
  for (const auto sides :
       {PositionSides::both, PositionSides::long_only, PositionSides::short_only})
    if (name == position_sides_name(sides))
      return sides;
  throw std::invalid_argument("position sides must be both, long or short");
}
std::vector<TargetOrder> target_orders(Decimal target, const HeldPosition& held,
                                       ClosePolicy policy) {
  if (held.today < Decimal{} || held.yesterday < Decimal{})
    throw std::invalid_argument("a held position cannot be negative");
  const auto quantity = held.today + held.yesterday;
  const auto side = target < Decimal{} ? Side::sell : Side::buy;
  const auto wanted = target < Decimal{} ? Decimal{} - target : target;
  // What stays of the held side: all of it only while the target is on it.
  const auto keep = held.side == side ? std::min(wanted, quantity) : Decimal{};
  std::vector<TargetOrder> orders;
  if (const auto excess = quantity - keep; excess > Decimal{}) {
    const auto closing = held.side == Side::buy ? Side::sell : Side::buy;
    if (policy != ClosePolicy::explicit_buckets) {
      // The exchange assigns buckets (and their fees) itself.
      orders.push_back({closing, Offset::close, excess, ""});
      return orders;
    }
    const auto old = std::min(excess, held.yesterday);
    if (old > Decimal{})
      orders.push_back({closing, Offset::close_yesterday, old, ".yesterday"});
    if (excess > old)
      orders.push_back({closing, Offset::close_today, excess - old, ".today"});
    return orders;
  }
  if (wanted > quantity)
    orders.push_back({side, Offset::open, wanted - quantity, ""});
  return orders;
}
} // namespace asterion
