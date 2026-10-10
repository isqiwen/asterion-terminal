#pragma once
#include <asterion/domain/account.hpp>
#include <string_view>
#include <vector>
namespace asterion {
// Which sides of a contract a strategy may hold.
enum class PositionSides { both, long_only, short_only };
std::string_view position_sides_name(PositionSides sides) noexcept;
PositionSides parse_position_sides(std::string_view name);

// One contract's position on the side it is held; no quantity means flat.
struct HeldPosition {
  Side side = Side::buy;
  Decimal today, yesterday;
};
struct TargetOrder {
  Side side;
  Offset offset;
  Decimal quantity;
  // Names the bucket an explicit-bucket close takes; empty otherwise.
  std::string_view suffix;
};
// The orders that move a contract toward a signed target: positive lots long,
// negative lots short, zero flat. A position on the other side is closed
// first and nothing is opened in that step, so the two sides are never held
// together; the caller asks again once the close has filled. Closes follow
// the exchange's rule: explicit-bucket venues close yesterday's lots before
// today's, with one order for each.
std::vector<TargetOrder> target_orders(Decimal target, const HeldPosition& held,
                                       ClosePolicy policy);
} // namespace asterion
