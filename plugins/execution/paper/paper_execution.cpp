#include "paper_execution.hpp"
#include "portfolio.hpp"
#include <asterion/domain/position_target.hpp>
#include <asterion/protocol/data.hpp>
#include <algorithm>
#include <stdexcept>
namespace asterion {
namespace {
template <class Data>
const MarketBar& bar_of(const Data& data, const PaperExecution::Event& event) {
  return data.contracts[event.contract].bars[event.bar];
}
// A contract's price increment on a trading day: that of the last change on
// or before it, or of its first bar's day.
Decimal increment_on(const ContractBars& contract, const std::string& trading_day) {
  auto result = contract.terms.instrument.price_increment;
  for (const auto& change : contract.price_increment_changes) {
    if (change.trading_day > trading_day)
      break;
    result = change.price_increment;
  }
  return result;
}
// What a fill gives up on a day.
Decimal slippage_on(const ContractBars& contract, const std::string& trading_day) {
  return increment_on(contract, trading_day) *
         Decimal::parse(std::to_string(contract.slippage_ticks));
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
    // Each change follows the first bar's day and the change before it, and
    // is to another increment.
    auto instrument = contract.terms.instrument;
    for (const std::string* after = nullptr;
         const auto& change : contract.price_increment_changes) {
      if (contract.bars.empty() || change.trading_day <= contract.bars.front().trading_day ||
          (after && change.trading_day <= *after) || change.price_increment <= Decimal{} ||
          change.price_increment == instrument.price_increment)
        throw std::invalid_argument("invalid price increment change");
      instrument.price_increment = change.price_increment;
      after = &change.trading_day;
    }
    const MarketBar* previous = nullptr;
    for (std::size_t b = 0; b < contract.bars.size(); ++b) {
      const auto& bar = contract.bars[b];
      instrument.price_increment = increment_on(contract, bar.trading_day);
      bar.validate(instrument);
      if (bar.low <= Decimal{})
        throw std::invalid_argument("futures paper model requires positive market prices");
      // A sale that gives the slippage up must still have a price.
      if (bar.low <= slippage_on(contract, bar.trading_day))
        throw std::invalid_argument("slippage reaches a contract's lowest price");
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
// Called only for FuturesAccount::working_orders(). A bar must reach the
// price that lies the slippage inside the limit; the fill gives the slippage
// back up and so never passes the limit.
std::optional<Decimal> fill_price(const AccountOrder& item, const MarketBar& bar,
                                  Decimal slippage) {
  const auto& request = item.order.request();
  if (request.side == Side::buy) {
    const auto reached = request.limit_price - slippage;
    return bar.low <= reached ? std::optional(std::min(bar.open, reached) + slippage)
                              : std::nullopt;
  }
  const auto reached = request.limit_price + slippage;
  return bar.high >= reached ? std::optional(std::max(bar.open, reached) - slippage) : std::nullopt;
}
} // namespace
void PaperExecution::advance(const std::optional<Target>& target) {
  require_running();
  if (cursor_ == size())
    throw std::invalid_argument("replay has finished");
  const auto& current = event(cursor_);
  const auto& bar = this->bar(current);
  const auto& terms = data_->contracts[current.contract].terms;
  const auto& instrument = terms.instrument.id;
  const auto slippage = slippage_on(data_->contracts[current.contract], bar.trading_day);
  const auto fills = [&](const AccountOrder& item) {
    return item.order.request().instrument == instrument &&
           fill_price(item, bar, slippage).has_value();
  };
  const bool new_terms =
      cursor_ && bar.trading_day != this->bar(event(cursor_ - 1)).trading_day && changes_terms();
  if (!target && !new_terms &&
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
    if (new_terms)
      enter_day(bar.trading_day);
    if (target)
      reconcile_target(target->order_id, instrument, target->quantity, target->limit_price);
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
      const auto price = fill_price(item, bar, slippage);
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
bool PaperExecution::changes_terms() const {
  return std::ranges::any_of(data_->contracts, [](const auto& c) {
    return !c.cost_schedule.empty() || !c.price_increment_changes.empty();
  });
}
void PaperExecution::enter_day(const std::string& trading_day) {
  std::vector<FuturesCosts> costs;
  std::vector<Decimal> increments;
  for (const auto& contract : data_->contracts) {
    costs.push_back(contract.cost_schedule.empty()
                        ? contract.terms.costs
                        : costs_on(contract.cost_schedule, trading_day).values);
    increments.push_back(increment_on(contract, trading_day));
  }
  account_.update_costs(costs);
  account_.update_price_increments(increments);
}
void PaperExecution::settle_scheduled(const std::string& trading_day,
                                      const std::vector<std::optional<Decimal>>& prices) {
  require_running();
  if (trading_day <= last_settled_day_ ||
      (cursor_ && bar(event(cursor_ - 1)).trading_day > trading_day) ||
      (cursor_ < size() && bar(event(cursor_)).trading_day <= trading_day))
    throw std::invalid_argument(
        "a day is settled once, after its bars and before those of a later day");
  // The first bar of a day brings the day's terms. A day after the first bar
  // that has none of its own follows the calendar here: the schedules cover
  // the days from that bar on.
  const bool idle = cursor_ && bar(event(cursor_ - 1)).trading_day < trading_day && changes_terms();
  auto batch = account_.transaction();
  if (idle)
    enter_day(trading_day);
  account_.settle_traded(prices);
  batch.commit();
  last_settled_day_ = trading_day;
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
void PaperExecution::reconcile_target(const std::string& order_id, const InstrumentId& instrument,
                                      Decimal target, Decimal price) {
  require_running();
  if (order_id.empty() || order_id.size() > 128)
    throw std::invalid_argument("invalid target order identity");
  const auto index = account_.contract_index(instrument);
  const auto& spec = account_.contracts()[index].instrument;
  if (!target.multiple_of(spec.quantity_increment))
    throw std::invalid_argument("target must be lot aligned");
  HeldPosition held;
  for (const auto& lot : account_.positions()) {
    if (lot.instrument != instrument)
      continue;
    if (held.today + held.yesterday > Decimal{} && lot.side != held.side)
      throw std::invalid_argument("a target needs the contract held on one side only");
    held.side = lot.side;
    auto& bucket = lot.today ? held.today : held.yesterday;
    bucket = bucket + lot.quantity;
  }
  // On the increment the account trades the contract at now: a price decided
  // before it changed is moved onto it against the order.
  const auto increment = spec.price_increment;
  const auto slippage =
      increment * Decimal::parse(std::to_string(data_->contracts[index].slippage_ticks));
  const auto orders = target_orders(target, held, account_.close_policy(instrument));
  const auto working = std::ranges::any_of(account_.working_orders(), [&](const auto index) {
    return account_.orders()[index].order.request().instrument == instrument;
  });
  if (orders.empty() && !working)
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
    for (const auto& order : orders)
      // The limit lies the slippage beyond the price a bar must reach.
      submit({order_id + std::string(order.suffix), instrument, order.side, order.quantity,
              order.side == Side::buy ? quantize(price, increment, Rounding::ceiling) + slippage
                                      : quantize(price, increment, Rounding::floor) - slippage},
             order.offset);
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
