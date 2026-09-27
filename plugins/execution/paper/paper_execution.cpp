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
    throw std::invalid_argument("模拟回放需要 1 至 10000 笔历史成交");
  std::int64_t previous = 0;
  for (const auto& tick : *ticks_) {
    tick.validate(instrument);
    if (tick.price <= Decimal{})
      throw std::invalid_argument("当前期货模拟模型要求正数行情价格");
    if (tick.timestamp_ns < previous)
      throw std::invalid_argument("历史成交时间必须升序");
    previous = tick.timestamp_ns;
  }
}
PluginDescriptor PaperExecution::descriptor() const {
  return {"asterion.execution.paper", PluginKind::execution, plugin_contract_version, {}};
}
void PaperExecution::require_running() const {
  if (!running_)
    throw std::logic_error("模拟执行插件未启动");
}
void PaperExecution::submit(LimitOrder order, Offset offset) {
  require_running();
  if (cursor_ == ticks_->size())
    throw std::invalid_argument("行情已回放结束，不能新增委托");
  const auto decision = assess_order(*risk_, account_, order, offset);
  if (!decision.allowed())
    throw std::invalid_argument("pre-trade risk rejected: " +
                                std::to_string(static_cast<int>(decision.reason)));
  account_.submit(std::move(order), offset);
}
void PaperExecution::cancel(const std::string& id) {
  require_running();
  account_.cancel(id);
}
void PaperExecution::advance() {
  require_running();
  if (cursor_ == ticks_->size())
    throw std::invalid_argument("行情已回放结束");
  auto next = account_;
  auto sequence = execution_sequence_;
  const auto& tick = ticks_->at(cursor_);
  next.mark(tick.price);
  auto liquidity = tick.quantity;
  // Snapshot order sequence before fills: arrival order, shared per-tick
  // volume.
  const auto orders = next.orders();
  for (const auto& item : orders) {
    if (liquidity == Decimal{})
      break;
    if (item.order.state() != OrderState::accepted &&
        item.order.state() != OrderState::partially_filled)
      continue;
    const auto& request = item.order.request();
    if ((request.side == Side::buy && tick.price > request.limit_price) ||
        (request.side == Side::sell && tick.price < request.limit_price))
      continue;
    if (item.offset == Offset::open &&
        Decimal::parse(next.snapshot().at("available").get<std::string>()) < Decimal{}) {
      next.cancel(request.id);
      continue;
    }
    auto quantity = quantize(std::min(liquidity, item.order.remaining_quantity()),
                             account_.instrument().quantity_increment, Rounding::floor);
    if (quantity == Decimal{})
      continue;
    next.fill({"paper.fill." + std::to_string(++sequence), request.id, quantity, tick.price});
    liquidity = liquidity - quantity;
  }
  account_ = std::move(next);
  execution_sequence_ = sequence;
  ++cursor_;
}
void PaperExecution::settle(Decimal price) {
  require_running();
  if (cursor_ != ticks_->size())
    throw std::invalid_argument("本次回放结束后才能手动结算");
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
  const auto orders = account_.orders();
  for (const auto& item : orders)
    if (item.order.state() == OrderState::accepted ||
        item.order.state() == OrderState::partially_filled)
      account_.cancel(item.order.request().id);
}
void PaperExecution::reconcile_long_target(const std::string& order_id, Decimal target,
                                           Decimal price) {
  require_running();
  if (order_id.empty() || order_id.size() > 128)
    throw std::invalid_argument("invalid target order identity");
  if (target < Decimal{} || !target.multiple_of(account_.instrument().quantity_increment))
    throw std::invalid_argument("target must be nonnegative and lot aligned");
  Decimal today, yesterday;
  const auto state = account_.snapshot();
  for (const auto& lot : state.at("positions")) {
    if (lot.at("side") != "buy")
      throw std::invalid_argument("long/flat target requires long positions");
    auto& bucket = lot.at("bucket") == "today" ? today : yesterday;
    bucket = bucket + Decimal::parse(lot.at("quantity").get<std::string>());
  }
  const auto current = today + yesterday;
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
