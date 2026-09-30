#include "paper_execution.hpp"
#include <algorithm>
#include <stdexcept>
namespace asterion {
PaperExecution::PaperExecution(Instrument instrument, Decimal deposit, FuturesCosts costs,
                               std::vector<MarketBar> bars, std::shared_ptr<const RiskPort> risk)
    : account_(instrument, deposit, costs), risk_(std::move(risk)),
      bars_(std::make_shared<const std::vector<MarketBar>>(std::move(bars))) {
  if (!risk_)
    throw std::invalid_argument("risk plugin is required");
  if (bars_->empty() || bars_->size() > paper_max_bars)
    throw std::invalid_argument("paper replay requires 1 to 20000 historical bars");
  const MarketBar* previous = nullptr;
  for (const auto& bar : *bars_) {
    bar.validate(instrument);
    if (bar.low <= Decimal{})
      throw std::invalid_argument("futures paper model requires positive market prices");
    if (previous &&
        (bar.timestamp_ns <= previous->timestamp_ns || bar.trading_day < previous->trading_day))
      throw std::invalid_argument("historical bars must be strictly ascending");
    previous = &bar;
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
  if (cursor_ == bars_->size())
    throw std::invalid_argument("replay has finished; no new orders accepted");
  const auto decision = assess_order(*risk_, account_, order, offset);
  if (!decision.allowed())
    throw std::invalid_argument("pre-trade risk rejected: " +
                                std::string(risk_reason_name(decision.reason)));
  account_.submit(std::move(order), offset);
  ++revision_;
}
void PaperExecution::cancel(const std::string& id) {
  require_running();
  account_.cancel(id);
  ++revision_;
}
namespace {
bool working(const AccountOrder& item) {
  return item.order.state() == OrderState::accepted ||
         item.order.state() == OrderState::partially_filled;
}
// Conservative fill price on a bar, or none when the bar never reached the limit.
std::optional<Decimal> fill_price(const AccountOrder& item, const MarketBar& bar) {
  if (!working(item))
    return std::nullopt;
  const auto& request = item.order.request();
  if (request.side == Side::buy)
    return bar.low <= request.limit_price ? std::optional(std::min(bar.open, request.limit_price))
                                          : std::nullopt;
  return bar.high >= request.limit_price ? std::optional(std::max(bar.open, request.limit_price))
                                         : std::nullopt;
}
} // namespace
void PaperExecution::advance() {
  require_running();
  if (cursor_ == bars_->size())
    throw std::invalid_argument("replay has finished");
  const auto& bar = bars_->at(cursor_);
  if (std::ranges::none_of(account_.orders(),
                           [&](const auto& item) { return fill_price(item, bar).has_value(); })) {
    // Nothing can fill: marking alone has a strong guarantee, no ledger copy.
    account_.mark(bar.close);
    ++cursor_;
    ++revision_;
    return;
  }
  auto next = account_;
  auto sequence = execution_sequence_;
  auto liquidity = quantize(multiply(bar.volume, paper_bar_participation, Rounding::floor),
                            account_.instrument().quantity_increment, Rounding::floor);
  // Arrival order shares the bar's participation volume. Fills and cancels
  // never append orders, so indexes stay stable during the pass.
  for (std::size_t i = 0; i < next.orders().size() && liquidity != Decimal{}; ++i) {
    const auto& item = next.orders()[i];
    const auto price = fill_price(item, bar);
    if (!price)
      continue;
    const auto id = item.order.request().id;
    if (item.offset == Offset::open && next.available() < Decimal{}) {
      next.cancel(id);
      continue;
    }
    const auto quantity = std::min(liquidity, item.order.remaining_quantity());
    next.fill({"paper.fill." + std::to_string(++sequence), id, quantity, *price});
    liquidity = liquidity - quantity;
  }
  next.mark(bar.close);
  account_ = std::move(next);
  execution_sequence_ = sequence;
  ++cursor_;
  ++revision_;
}
void PaperExecution::settle(Decimal price) {
  require_running();
  if (cursor_ != bars_->size())
    throw std::invalid_argument("manual settlement is allowed only after the replay finishes");
  account_.settle(price);
  ++revision_;
}
void PaperExecution::settle_day_end(Decimal price) {
  require_running();
  if (cursor_ == 0 || cursor_ == bars_->size())
    throw std::invalid_argument("day-end settlement must fall between replay days");
  const auto& day = bars_->at(cursor_ - 1).trading_day;
  if (day == bars_->at(cursor_).trading_day || day == last_settled_day_)
    throw std::invalid_argument("settlement must follow the last bar of an unsettled day");
  account_.settle(price);
  last_settled_day_ = day;
  ++revision_;
}
void PaperExecution::cancel_open_orders() {
  require_running();
  // Cancelling a working order cannot fail, so the loop is all-or-nothing.
  for (std::size_t i = 0; i < account_.orders().size(); ++i)
    if (working(account_.orders()[i])) {
      account_.cancel(account_.orders()[i].order.request().id);
      ++revision_;
    }
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
  } else if (target < current && account_.close_policy() != ClosePolicy::explicit_buckets) {
    // The exchange assigns buckets (and their fees) itself.
    candidate.submit({order_id, account_.instrument().id, Side::sell, current - target, price},
                     Offset::close);
  } else if (target < current) {
    // Explicit-bucket venues: yesterday first, then today. A simulator policy,
    // not a fee optimization. Each bucket retains its fee.
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
  ++revision_;
}
std::optional<std::int64_t> PaperExecution::timestamp_ns() const {
  if (!cursor_)
    return std::nullopt;
  return bars_->at(cursor_ - 1).timestamp_ns;
}
Json PaperExecution::snapshot() const {
  auto result = account_.snapshot();
  result["cursor"] = cursor_;
  result["total"] = bars_->size();
  result["timestamp_ns"] =
      cursor_ ? Json(std::to_string(bars_->at(cursor_ - 1).timestamp_ns)) : Json(nullptr);
  result["mode"] = "historical_paper";
  return result;
}
} // namespace asterion
