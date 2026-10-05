#include "paper_execution.hpp"
#include "portfolio.hpp"
#include <asterion/protocol/data.hpp>
#include <algorithm>
#include <stdexcept>
namespace asterion {
namespace {
template <class Data>
const MarketBar& bar_of(const Data& data, const PaperExecution::Event& event) {
  return data.contracts[event.contract].bars[event.bar];
}
std::vector<ContractTerms> terms_of(const std::vector<ContractBars>& contracts) {
  std::vector<ContractTerms> result;
  for (const auto& contract : contracts) {
    auto terms = contract.terms;
    if (!contract.cost_schedule.empty()) {
      validate_cost_schedule(contract.cost_schedule);
      if (!contract.bars.empty())
        terms.costs = costs_on(contract.cost_schedule, contract.bars.front().trading_day).values;
    }
    result.push_back(std::move(terms));
  }
  return result;
}
} // namespace
PaperExecution::PaperExecution(Decimal deposit, std::vector<ContractBars> contracts,
                               std::shared_ptr<const RiskPort> risk)
    : account_(deposit, terms_of(contracts)), risk_(std::move(risk)) {
  if (!risk_)
    throw std::invalid_argument("risk plugin is required");
  auto data = std::make_shared<Data>();
  data->contracts = std::move(contracts);
  std::size_t total = 0;
  for (std::size_t c = 0; c < data->contracts.size(); ++c) {
    const auto& contract = data->contracts[c];
    const MarketBar* previous = nullptr;
    for (std::size_t b = 0; b < contract.bars.size(); ++b) {
      const auto& bar = contract.bars[b];
      bar.validate(contract.terms.instrument);
      if (bar.low <= Decimal{})
        throw std::invalid_argument("futures paper model requires positive market prices");
      if (previous &&
          (bar.timestamp_ns <= previous->timestamp_ns || bar.trading_day < previous->trading_day))
        throw std::invalid_argument("historical bars must be strictly ascending");
      previous = &bar;
    }
    total += contract.bars.size();
  }
  // In-memory replay shares the task protocol budget. Durable sessions
  // enforce their smaller bar budget before constructing the engine.
  if (!total || total > protocol::max_dataset_bars)
    throw std::invalid_argument("paper replay requires 1 to 200000 historical bars");
  for (const auto& contract : data->contracts)
    if (contract.bars.empty())
      throw std::invalid_argument("every portfolio contract requires historical bars");
  // One clock for the engine, its schedule and strategy replays.
  std::vector<const std::vector<MarketBar>*> series;
  for (const auto& contract : data->contracts)
    series.push_back(&contract.bars);
  data->events = replay_order(series);
  for (std::size_t i = 1; i < data->events.size(); ++i)
    if (bar_of(*data, data->events[i]).trading_day < bar_of(*data, data->events[i - 1]).trading_day)
      throw std::invalid_argument("portfolio bars disagree on trading days by time");
  data_ = std::move(data);
}
void PaperExecution::require_running() const {
  if (!running_)
    throw std::logic_error("paper execution plugin is not started");
}
void PaperExecution::submit(LimitOrder order, Offset offset) {
  require_running();
  if (cursor_ == size())
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
// Called only for FuturesAccount::working_orders(). A bar must reach the limit.
std::optional<Decimal> fill_price(const AccountOrder& item, const MarketBar& bar) {
  const auto& request = item.order.request();
  if (request.side == Side::buy)
    return bar.low <= request.limit_price ? std::optional(std::min(bar.open, request.limit_price))
                                          : std::nullopt;
  return bar.high >= request.limit_price ? std::optional(std::max(bar.open, request.limit_price))
                                         : std::nullopt;
}
} // namespace
void PaperExecution::advance(const std::optional<LongTarget>& target) {
  require_running();
  if (cursor_ == size())
    throw std::invalid_argument("replay has finished");
  const auto& current = event(cursor_);
  const auto& bar = this->bar(current);
  const auto& terms = data_->contracts[current.contract].terms;
  const auto& instrument = terms.instrument.id;
  const auto fills = [&](const AccountOrder& item) {
    return item.order.request().instrument == instrument && fill_price(item, bar).has_value();
  };
  const bool changes_day = cursor_ && bar.trading_day != this->bar(event(cursor_ - 1)).trading_day;
  const bool changes_costs =
      changes_day &&
      std::ranges::any_of(data_->contracts, [](const auto& c) { return !c.cost_schedule.empty(); });
  if (!target && !changes_costs &&
      std::ranges::none_of(account_.working_orders(),
                           [&](const auto index) { return fills(account_.orders()[index]); })) {
    // Nothing can fill: marking alone has a strong guarantee, no ledger copy.
    account_.mark(instrument, bar.close);
    ++cursor_;
    ++revision_;
    return;
  }
  const auto previous_revision = revision_;
  auto batch = account_.transaction();
  try {
    auto& next = account_;
    if (changes_costs) {
      std::vector<FuturesCosts> costs;
      for (const auto& contract : data_->contracts)
        costs.push_back(contract.cost_schedule.empty()
                            ? contract.terms.costs
                            : costs_on(contract.cost_schedule, bar.trading_day).values);
      next.update_costs(costs);
    }
    if (target)
      reconcile_long_target(target->order_id, instrument, target->quantity, target->limit_price);
    auto sequence = execution_sequence_;
    auto liquidity = quantize(multiply(bar.volume, paper_bar_participation, Rounding::floor),
                              terms.instrument.quantity_increment, Rounding::floor);
    // Arrival order shares the bar's participation volume. Fills and cancels
    // never append orders, so indexes stay stable during the pass.
    for (auto pending = next.working_orders().begin();
         pending != next.working_orders().end() && liquidity != Decimal{};) {
      // Advance before filling/cancelling: completion removes just this index.
      const auto i = *pending++;
      const auto& item = next.orders()[i];
      if (item.order.request().instrument != instrument)
        continue;
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
    next.mark(instrument, bar.close);
    batch.commit();
    execution_sequence_ = sequence;
    ++cursor_;
    ++revision_;
  } catch (...) {
    revision_ = previous_revision;
    throw;
  }
}
void PaperExecution::settle(const std::vector<Decimal>& prices) {
  settle_scheduled({prices.begin(), prices.end()}, true);
}
void PaperExecution::settle_day_end(const std::vector<Decimal>& prices) {
  settle_scheduled({prices.begin(), prices.end()}, false);
}
void PaperExecution::settle_scheduled(const std::vector<std::optional<Decimal>>& prices,
                                      bool final) {
  require_running();
  if (final) {
    if (cursor_ != size())
      throw std::invalid_argument("final settlement is allowed only after the replay finishes");
    account_.settle_traded(prices);
    ++revision_;
    return;
  }
  if (cursor_ == 0 || cursor_ == size())
    throw std::invalid_argument("day-end settlement must fall between replay days");
  const auto& day = bar(event(cursor_ - 1)).trading_day;
  if (day == bar(event(cursor_)).trading_day || day == last_settled_day_)
    throw std::invalid_argument("settlement must follow the last bar of an unsettled day");
  account_.settle_traded(prices);
  last_settled_day_ = day;
  ++revision_;
}
void PaperExecution::cancel_open_orders() {
  require_running();
  // Cancelling a working order cannot fail, so the loop is all-or-nothing.
  while (!account_.working_orders().empty()) {
    account_.cancel(account_.orders()[*account_.working_orders().begin()].order.request().id);
    ++revision_;
  }
}
void PaperExecution::cancel_open_orders(const InstrumentId& instrument) {
  require_running();
  (void)account_.contract_index(instrument);
  for (auto pending = account_.working_orders().begin();
       pending != account_.working_orders().end();) {
    const auto& item = account_.orders()[*pending++];
    if (item.order.request().instrument == instrument) {
      account_.cancel(item.order.request().id);
      ++revision_;
    }
  }
}
void PaperExecution::reconcile_long_target(const std::string& order_id,
                                           const InstrumentId& instrument, Decimal target,
                                           Decimal price) {
  require_running();
  if (order_id.empty() || order_id.size() > 128)
    throw std::invalid_argument("invalid target order identity");
  const auto& spec = account_.contracts()[account_.contract_index(instrument)].instrument;
  if (target < Decimal{} || !target.multiple_of(spec.quantity_increment))
    throw std::invalid_argument("target must be nonnegative and lot aligned");
  Decimal today, yesterday;
  for (const auto& lot : account_.positions()) {
    if (lot.instrument != instrument)
      continue;
    if (lot.side != Side::buy)
      throw std::invalid_argument("long/flat target requires long positions");
    auto& bucket = lot.today ? today : yesterday;
    bucket = bucket + lot.quantity;
  }
  const auto current = today + yesterday;
  const auto working = std::ranges::any_of(account_.working_orders(), [&](const auto index) {
    return account_.orders()[index].order.request().instrument == instrument;
  });
  if (target == current && !working)
    return;
  // Keep account and revision changes atomic across both close buckets without
  // copying the completed order/fill history. Risk checks keep the same order.
  auto batch = account_.transaction();
  const auto previous_revision = revision_;
  try {
    for (auto pending = account_.working_orders().begin();
         pending != account_.working_orders().end();) {
      const auto index = *pending++;
      const auto& item = account_.orders()[index];
      if (item.order.request().instrument == instrument)
        account_.cancel(item.order.request().id);
    }
    if (target > current) {
      submit({order_id, instrument, Side::buy, target - current, price}, Offset::open);
    } else if (target < current &&
               account_.close_policy(instrument) != ClosePolicy::explicit_buckets) {
      // The exchange assigns buckets (and their fees) itself.
      submit({order_id, instrument, Side::sell, current - target, price}, Offset::close);
    } else if (target < current) {
      // Explicit-bucket venues: yesterday first, then today. A simulator policy,
      // not a fee optimization. Each bucket retains its fee.
      const auto old_quantity = std::min(current - target, yesterday);
      const auto new_quantity = current - target - old_quantity;
      const bool split = old_quantity > Decimal{} && new_quantity > Decimal{};
      if (old_quantity > Decimal{})
        submit({split ? order_id + ".yesterday" : order_id, instrument, Side::sell, old_quantity,
                price},
               Offset::close_yesterday);
      if (new_quantity > Decimal{})
        submit(
            {split ? order_id + ".today" : order_id, instrument, Side::sell, new_quantity, price},
            Offset::close_today);
    }
  } catch (...) {
    revision_ = previous_revision;
    throw;
  }
  batch.commit();
  ++revision_;
}
std::optional<std::int64_t> PaperExecution::timestamp_ns() const {
  if (!cursor_)
    return std::nullopt;
  return bar(event(cursor_ - 1)).timestamp_ns;
}
Json PaperExecution::snapshot() const {
  auto result = account_.snapshot();
  result["cursor"] = cursor_;
  result["total"] = size();
  result["timestamp_ns"] = cursor_ ? Json(std::to_string(*timestamp_ns())) : Json(nullptr);
  result["mode"] = "historical_paper";
  return result;
}
} // namespace asterion
