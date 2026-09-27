#include "paper_execution.hpp"
#include <algorithm>
#include <stdexcept>
namespace asterion {
PaperExecution::PaperExecution(Instrument instrument, Decimal deposit, FuturesCosts costs,
                               std::vector<TradeTick> ticks, std::shared_ptr<const RiskPort> risk)
    : account_(instrument, deposit, costs), risk_(std::move(risk)),
      ticks_(std::make_shared<const std::vector<TradeTick>>(std::move(ticks))) {
  if (!risk_)
    throw std::invalid_argument("risk plugin is required");
  if (ticks_->empty() || ticks_->size() > 10000)
    throw std::invalid_argument("paper replay requires 1 to 10000 historical trades");
  std::int64_t previous = 0;
  for (const auto& tick : *ticks_) {
    tick.validate(instrument);
    if (tick.price <= Decimal{})
      throw std::invalid_argument("futures paper model requires positive market prices");
    if (tick.timestamp_ns < previous)
      throw std::invalid_argument("historical trade times must be ascending");
    previous = tick.timestamp_ns;
  }
}
PluginDescriptor PaperExecution::descriptor() const {
  return {"asterion.execution.paper", PluginKind::execution, plugin_contract_version, {}};
}
void PaperExecution::require_running() const {
  if (!running_)
    throw std::logic_error("paper execution plugin is not started");
}
void PaperExecution::submit(LimitOrder order, Offset offset) {
  require_running();
  if (cursor_ == ticks_->size())
    throw std::invalid_argument("replay has finished; no new orders accepted");
  const auto decision = assess_order(*risk_, account_, order, offset);
  if (!decision.allowed())
    throw std::invalid_argument("pre-trade risk rejected: " +
                                std::string(risk_reason_name(decision.reason)));
  account_.submit(std::move(order), offset);
}
void PaperExecution::cancel(const std::string& id) {
  require_running();
  account_.cancel(id);
}
namespace {
bool working(const AccountOrder& item) {
  return item.order.state() == OrderState::accepted ||
         item.order.state() == OrderState::partially_filled;
}
bool crosses(const AccountOrder& item, Decimal price) {
  const auto& request = item.order.request();
  return working(item) &&
         (request.side == Side::buy ? price <= request.limit_price : price >= request.limit_price);
}
} // namespace
void PaperExecution::advance() {
  require_running();
  if (cursor_ == ticks_->size())
    throw std::invalid_argument("replay has finished");
  const auto& tick = ticks_->at(cursor_);
  if (std::ranges::none_of(account_.orders(),
                           [&](const auto& item) { return crosses(item, tick.price); })) {
    // Nothing can fill: marking alone has a strong guarantee, no ledger copy.
    account_.mark(tick.price);
    ++cursor_;
    return;
  }
  auto next = account_;
  auto sequence = execution_sequence_;
  next.mark(tick.price);
  auto liquidity = tick.quantity;
  // Arrival order, shared per-tick volume. Fills and cancels never append
  // orders, so indexes stay stable during the pass.
  for (std::size_t i = 0; i < next.orders().size() && liquidity != Decimal{}; ++i) {
    const auto& item = next.orders()[i];
    if (!crosses(item, tick.price))
      continue;
    const auto id = item.order.request().id;
    if (item.offset == Offset::open && next.available() < Decimal{}) {
      next.cancel(id);
      continue;
    }
    auto quantity = quantize(std::min(liquidity, item.order.remaining_quantity()),
                             account_.instrument().quantity_increment, Rounding::floor);
    if (quantity == Decimal{})
      continue;
    next.fill({"paper.fill." + std::to_string(++sequence), id, quantity, tick.price});
    liquidity = liquidity - quantity;
  }
  account_ = std::move(next);
  execution_sequence_ = sequence;
  ++cursor_;
}
void PaperExecution::settle(Decimal price) {
  require_running();
  if (cursor_ != ticks_->size())
    throw std::invalid_argument("manual settlement is allowed only after the replay finishes");
  account_.settle(price);
}
void PaperExecution::settle_before_next(std::int64_t boundary_ns, Decimal price) {
  require_running();
  if (cursor_ == 0 || cursor_ == ticks_->size() || boundary_ns <= last_settlement_boundary_ ||
      boundary_ns <= ticks_->at(cursor_ - 1).timestamp_ns ||
      boundary_ns > ticks_->at(cursor_).timestamp_ns)
    throw std::invalid_argument("settlement boundary must precede the next replay event");
  account_.settle(price);
  last_settlement_boundary_ = boundary_ns;
}
void PaperExecution::cancel_open_orders() {
  require_running();
  // Cancelling a working order cannot fail, so the loop is all-or-nothing.
  for (std::size_t i = 0; i < account_.orders().size(); ++i)
    if (working(account_.orders()[i]))
      account_.cancel(account_.orders()[i].order.request().id);
}
void PaperExecution::reconcile_long_target(const std::string& order_id, Decimal target,
                                           Decimal price) {
  require_running();
  if (order_id.empty() || order_id.size() > 128)
    throw std::invalid_argument("invalid target order identity");
  if (target < Decimal{} || !target.multiple_of(account_.instrument().quantity_increment))
    throw std::invalid_argument("target must be nonnegative and lot aligned");
  Decimal today, yesterday;
  for (const auto& lot : account_.positions()) {
    if (lot.side != Side::buy)
      throw std::invalid_argument("long/flat target requires long positions");
    auto& bucket = lot.today ? today : yesterday;
    bucket = bucket + lot.quantity;
  }
  const auto current = today + yesterday;
  if (target == current && !account_.has_working_orders())
    return;
  // All child orders are checked against one candidate account. Rejection of
  // the second close must not cancel or partially replace existing orders.
  auto candidate = *this;
  candidate.cancel_open_orders();
  if (target > current) {
    candidate.submit({order_id, account_.instrument().id, Side::buy, target - current, price},
                     Offset::open);
  } else if (target < current) {
    // Explicit simulator policy: yesterday first, then today. This is not an
    // inferred exchange rule or fee optimization. Each bucket retains its fee.
    const auto old_quantity = std::min(current - target, yesterday);
    const auto new_quantity = current - target - old_quantity;
    const bool split = old_quantity > Decimal{} && new_quantity > Decimal{};
    if (old_quantity > Decimal{})
      candidate.submit({split ? order_id + ".yesterday" : order_id, account_.instrument().id,
                        Side::sell, old_quantity, price},
                       Offset::close_yesterday);
    if (new_quantity > Decimal{})
      candidate.submit({split ? order_id + ".today" : order_id, account_.instrument().id,
                        Side::sell, new_quantity, price},
                       Offset::close_today);
  }
  *this = std::move(candidate);
}
std::optional<std::int64_t> PaperExecution::timestamp_ns() const {
  if (!cursor_)
    return std::nullopt;
  return ticks_->at(cursor_ - 1).timestamp_ns;
}
Json PaperExecution::snapshot() const {
  auto result = account_.snapshot();
  result["cursor"] = cursor_;
  result["total"] = ticks_->size();
  result["timestamp_ns"] =
      cursor_ ? Json(std::to_string(ticks_->at(cursor_ - 1).timestamp_ns)) : Json(nullptr);
  result["mode"] = "historical_paper";
  return result;
}
} // namespace asterion
