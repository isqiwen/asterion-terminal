#include <asterion/domain/account.hpp>
#include <algorithm>
#include <stdexcept>
#include <type_traits>

namespace asterion {
namespace {
const auto zero = Decimal{};
const auto one = Decimal::parse("1");
bool active(const AccountOrder& item) {
  return item.order.state() == OrderState::accepted ||
         item.order.state() == OrderState::partially_filled;
}
const char* offset_name(Offset value) {
  switch (value) {
  case Offset::open:
    return "open";
  case Offset::close_today:
    return "close_today";
  case Offset::close_yesterday:
    return "close_yesterday";
  }
  throw std::invalid_argument("invalid open/close offset");
}
const char* state_name(OrderState value) {
  switch (value) {
  case OrderState::pending:
    return "pending";
  case OrderState::accepted:
    return "accepted";
  case OrderState::partially_filled:
    return "partially_filled";
  case OrderState::filled:
    return "filled";
  case OrderState::cancelled:
    return "cancelled";
  case OrderState::rejected:
    return "rejected";
  }
  throw std::invalid_argument("invalid order state");
}
} // namespace
void FuturesCosts::validate() const {
  if (margin_per_lot <= zero || open_fee < zero || close_today_fee < zero ||
      close_yesterday_fee < zero)
    throw std::invalid_argument("margin per lot must be positive and fees nonnegative");
}
Decimal FuturesCosts::fee(Offset offset) const {
  switch (offset) {
  case Offset::open:
    return open_fee;
  case Offset::close_today:
    return close_today_fee;
  case Offset::close_yesterday:
    return close_yesterday_fee;
  }
  throw std::invalid_argument("invalid open/close offset");
}
FuturesAccount::FuturesAccount(Instrument instrument, Decimal deposit, FuturesCosts costs)
    : instrument_(std::move(instrument)), costs_(costs), balance_(deposit) {
  instrument_.validate();
  costs_.validate();
  if (instrument_.asset_class != AssetClass::futures || deposit <= zero ||
      !instrument_.quantity_increment.multiple_of(one))
    throw std::invalid_argument(
        "futures account requires a positive deposit and whole-lot quantity increment");
}
std::size_t FuturesAccount::index_of(const std::string& id) const {
  const auto found = order_index_.find(id);
  if (found == order_index_.end())
    throw std::invalid_argument("order does not exist");
  return found->second;
}
bool FuturesAccount::has_working_orders() const noexcept {
  return std::ranges::any_of(orders_, active);
}
Decimal FuturesAccount::margin() const {
  Decimal result;
  for (const auto& lot : lots_)
    result = result + lot.quantity * costs_.margin_per_lot;
  return result;
}
Decimal FuturesAccount::unrealized() const {
  Decimal result;
  if (mark_ == zero)
    return result;
  for (const auto& lot : lots_)
    result = result + (lot.side == Side::buy ? mark_ - lot.price : lot.price - mark_) *
                          lot.quantity * instrument_.multiplier;
  return result;
}
Decimal FuturesAccount::frozen() const {
  Decimal result;
  for (const auto& item : orders_)
    if (active(item))
      result = result + item.order.remaining_quantity() *
                            (costs_.fee(item.offset) +
                             (item.offset == Offset::open ? costs_.margin_per_lot : zero));
  return result;
}
Decimal FuturesAccount::available() const {
  return std::min(balance_, balance_ + unrealized()) - margin() - frozen();
}
Decimal FuturesAccount::closable(Side side, bool today) const {
  Decimal result;
  for (const auto& lot : lots_)
    if (lot.side == side && lot.today == today)
      result = result + lot.quantity;
  for (const auto& item : orders_)
    if (active(item) && item.offset != Offset::open && item.order.request().side != side &&
        (item.offset == Offset::close_today) == today)
      result = result - item.order.remaining_quantity();
  return result;
}
// Every check and every value that can overflow is computed before the first
// mutation; the remaining mutations either cannot throw or are rolled back.
void FuturesAccount::submit(LimitOrder request, Offset offset) {
  (void)offset_name(offset);
  if (request.side != Side::buy && request.side != Side::sell)
    throw std::invalid_argument("invalid order side");
  if (orders_.size() >= 10000)
    throw std::invalid_argument("paper account holds at most 10000 orders");
  if (order_index_.contains(request.id))
    throw std::invalid_argument("duplicate order identity");
  if (request.limit_price <= zero)
    throw std::invalid_argument("futures paper model requires a positive limit price");
  Order order(std::move(request), instrument_);
  const auto& accepted = order.request();
  if (mark_ == zero)
    throw std::invalid_argument("replay at least one market event first");
  if (offset != Offset::open && closable(accepted.side == Side::buy ? Side::sell : Side::buy,
                                         offset == Offset::close_today) < accepted.quantity)
    throw std::invalid_argument(
        "insufficient closable position or already reserved by other orders");
  const auto required =
      accepted.quantity *
      (costs_.fee(offset) + (offset == Offset::open ? costs_.margin_per_lot : zero));
  const auto before = available();
  if (offset == Offset::open && required > before)
    throw std::invalid_argument("insufficient available funds");
  // The post-submit ledger must remain representable.
  (void)(frozen() + required);
  (void)(before - required);
  order.accept();
  auto id = accepted.id;
  orders_.push_back({std::move(order), offset});
  try {
    order_index_.emplace(std::move(id), orders_.size() - 1);
  } catch (...) {
    orders_.pop_back();
    throw;
  }
}
void FuturesAccount::cancel(const std::string& id) {
  orders_[index_of(id)].order.cancel();
}
static_assert(std::is_nothrow_move_assignable_v<Order>);
static_assert(std::is_nothrow_move_constructible_v<Fill>);
bool FuturesAccount::fill(const Fill& report) {
  if (const auto previous = fill_index_.find(report.execution_id); previous != fill_index_.end()) {
    if (fills_[previous->second] == report)
      return false;
    throw std::invalid_argument("execution identity conflicts with an earlier report");
  }
  if (report.price <= zero)
    throw std::invalid_argument("futures paper model requires a positive fill price");
  auto& item = orders_[index_of(report.order_id)];
  auto order = item.order;
  order.apply(report);
  const auto side = order.request().side;
  const auto fee = report.quantity * costs_.fee(item.offset);
  auto fees = fees_ + fee;
  auto balance = balance_ - fee;
  auto realized = realized_;
  auto lots = lots_;
  if (item.offset == Offset::open)
    lots.push_back({side, true, report.quantity, report.price});
  else {
    auto remaining = report.quantity;
    for (auto& lot : lots) {
      if (remaining == zero)
        break;
      if (lot.side == side || lot.today != (item.offset == Offset::close_today))
        continue;
      const auto amount = std::min(remaining, lot.quantity);
      const auto pnl =
          (lot.side == Side::buy ? report.price - lot.price : lot.price - report.price) * amount *
          instrument_.multiplier;
      balance = balance + pnl;
      realized = realized + pnl;
      lot.quantity = lot.quantity - amount;
      remaining = remaining - amount;
    }
    if (remaining != zero)
      throw std::logic_error("fill exceeds closable position");
    std::erase_if(lots, [](const auto& lot) { return lot.quantity == zero; });
  }
  auto recorded = report;
  fills_.reserve(fills_.size() + 1);
  fill_index_.emplace(report.execution_id, fills_.size());
  // Commit: no operation below can throw.
  item.order = std::move(order);
  lots_ = std::move(lots);
  fees_ = fees;
  balance_ = balance;
  realized_ = realized;
  fills_.push_back(std::move(recorded));
  return true;
}
void FuturesAccount::mark(Decimal price) {
  if (price <= zero || !price.multiple_of(instrument_.price_increment))
    throw std::invalid_argument("invalid mark price");
  const auto previous = mark_;
  mark_ = price;
  try {
    (void)available();
  } catch (...) {
    mark_ = previous;
    throw;
  }
}
void FuturesAccount::settle(Decimal price) {
  if (has_working_orders())
    throw std::invalid_argument("cancel all working orders before settlement");
  const auto previous = mark_;
  mark(price);
  try {
    const auto pnl = unrealized();
    const auto balance = balance_ + pnl;
    const auto realized = realized_ + pnl;
    auto lots = lots_;
    for (auto& lot : lots) {
      lot.today = false;
      lot.price = price;
    }
    lots_ = std::move(lots);
    balance_ = balance;
    realized_ = realized;
  } catch (...) {
    mark_ = previous;
    throw;
  }
}
Json FuturesAccount::snapshot() const {
  Json positions = Json::array(), orders = Json::array(), fills = Json::array();
  for (const auto& lot : lots_)
    positions.push_back({{"side", lot.side == Side::buy ? "buy" : "sell"},
                         {"bucket", lot.today ? "today" : "yesterday"},
                         {"quantity", lot.quantity.str()},
                         {"basis", lot.price.str()}});
  for (const auto& item : orders_) {
    const auto& r = item.order.request();
    orders.push_back({{"id", r.id},
                      {"side", r.side == Side::buy ? "buy" : "sell"},
                      {"offset", offset_name(item.offset)},
                      {"quantity", r.quantity.str()},
                      {"limit_price", r.limit_price.str()},
                      {"filled", item.order.filled_quantity().str()},
                      {"state", state_name(item.order.state())}});
  }
  for (const auto& f : fills_)
    fills.push_back({{"id", f.execution_id},
                     {"order_id", f.order_id},
                     {"quantity", f.quantity.str()},
                     {"price", f.price.str()}});
  return {{"balance", balance_.str()},
          {"equity", (balance_ + unrealized()).str()},
          {"available", available().str()},
          {"margin", margin().str()},
          {"frozen", frozen().str()},
          {"fees", fees_.str()},
          {"realized", realized_.str()},
          {"unrealized", unrealized().str()},
          {"mark", mark_.str()},
          {"positions", positions},
          {"orders", orders},
          {"fills", fills}};
}
} // namespace asterion
