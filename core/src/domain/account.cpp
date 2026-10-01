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
  case Offset::close:
    return "close";
  }
  throw std::invalid_argument("invalid open/close offset");
}
const auto cent = Decimal::parse("0.01");
// Notional component of a cost: price x quantity x multiplier x rate,
// rounded half-up to the cent.
Decimal notional_part(Decimal quantity, Decimal price, Decimal multiplier, Decimal rate) {
  if (rate == zero)
    return zero;
  return quantize(multiply(price * quantity * multiplier, rate, Rounding::half_up), cent,
                  Rounding::half_up);
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
ClosePolicy close_policy(std::string_view venue) noexcept {
  if (venue == "CFFEX")
    return ClosePolicy::today_first;
  if (venue == "DCE" || venue == "CZCE" || venue == "GFEX")
    return ClosePolicy::yesterday_first;
  return ClosePolicy::explicit_buckets; // SHFE, INE and anything unverified
}
void FuturesCosts::validate() const {
  const auto one_or_less = [](Decimal rate) { return rate >= zero && rate < one; };
  if (margin_per_lot < zero || open_fee < zero || close_today_fee < zero ||
      close_yesterday_fee < zero || !one_or_less(margin_rate) || !one_or_less(open_fee_rate) ||
      !one_or_less(close_today_fee_rate) || !one_or_less(close_yesterday_fee_rate))
    throw std::invalid_argument("costs must be nonnegative and rates below 1");
  if (margin_per_lot == zero && margin_rate == zero)
    throw std::invalid_argument("margin per lot or margin rate must be positive");
}
Decimal FuturesCosts::fee(Offset bucket, Decimal quantity, Decimal price,
                          Decimal multiplier) const {
  switch (bucket) {
  case Offset::open:
    return quantity * open_fee + notional_part(quantity, price, multiplier, open_fee_rate);
  case Offset::close_today:
    return quantity * close_today_fee +
           notional_part(quantity, price, multiplier, close_today_fee_rate);
  case Offset::close_yesterday:
    return quantity * close_yesterday_fee +
           notional_part(quantity, price, multiplier, close_yesterday_fee_rate);
  case Offset::close:
    break;
  }
  throw std::invalid_argument("fee requires a position bucket");
}
Decimal FuturesCosts::margin(Decimal quantity, Decimal price, Decimal multiplier) const {
  return quantity * margin_per_lot + notional_part(quantity, price, multiplier, margin_rate);
}
FuturesAccount::FuturesAccount(Decimal deposit, std::vector<ContractTerms> contracts)
    : contracts_(std::move(contracts)), marks_(contracts_.size()), balance_(deposit) {
  if (contracts_.empty() || contracts_.size() > max_portfolio_contracts || deposit <= zero)
    throw std::invalid_argument(
        "futures account requires a positive deposit and 1 to 20 contracts");
  for (std::size_t i = 0; i < contracts_.size(); ++i) {
    const auto& terms = contracts_[i];
    terms.instrument.validate();
    terms.costs.validate();
    if (terms.instrument.asset_class != AssetClass::futures ||
        !terms.instrument.quantity_increment.multiple_of(one) ||
        terms.instrument.quote_currency != contracts_.front().instrument.quote_currency)
      throw std::invalid_argument(
          "portfolio contracts must be futures in one currency with whole-lot quantities");
    for (std::size_t j = 0; j < i; ++j)
      if (contracts_[j].instrument.id == terms.instrument.id)
        throw std::invalid_argument("duplicate portfolio contract");
  }
}
std::size_t FuturesAccount::contract_index(const InstrumentId& instrument) const {
  for (std::size_t i = 0; i < contracts_.size(); ++i)
    if (contracts_[i].instrument.id == instrument)
      return i;
  throw std::invalid_argument("contract is not part of this account");
}
Decimal FuturesAccount::last_mark(const InstrumentId& instrument) const {
  return marks_[contract_index(instrument)];
}
ClosePolicy FuturesAccount::close_policy(const InstrumentId& instrument) const {
  return asterion::close_policy(contracts_[contract_index(instrument)].instrument.id.venue);
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
// Positions are margined at their contract's latest mark (their basis before any mark).
Decimal FuturesAccount::margin() const {
  Decimal result;
  for (const auto& lot : lots_) {
    const auto index = contract_index(lot.instrument);
    const auto& terms = contracts_[index];
    result =
        result + terms.costs.margin(lot.quantity, marks_[index] == zero ? lot.price : marks_[index],
                                    terms.instrument.multiplier);
  }
  return result;
}
Decimal FuturesAccount::unrealized() const {
  Decimal result;
  for (const auto& lot : lots_) {
    const auto index = contract_index(lot.instrument);
    const auto mark = marks_[index];
    if (mark == zero)
      continue;
    result = result + (lot.side == Side::buy ? mark - lot.price : lot.price - mark) * lot.quantity *
                          contracts_[index].instrument.multiplier;
  }
  return result;
}
Decimal FuturesAccount::reserved(const AccountOrder& item) const {
  const auto& terms = contracts_[contract_index(item.order.request().instrument)];
  const auto quantity = item.order.remaining_quantity();
  const auto price = item.order.request().limit_price;
  const auto multiplier = terms.instrument.multiplier;
  switch (item.offset) {
  case Offset::open:
    return terms.costs.margin(quantity, price, multiplier) +
           terms.costs.fee(Offset::open, quantity, price, multiplier);
  case Offset::close_today:
  case Offset::close_yesterday:
    return terms.costs.fee(item.offset, quantity, price, multiplier);
  case Offset::close:
    // The exchange picks the buckets at fill time: reserve the dearer fee.
    return std::max(terms.costs.fee(Offset::close_today, quantity, price, multiplier),
                    terms.costs.fee(Offset::close_yesterday, quantity, price, multiplier));
  }
  throw std::invalid_argument("invalid open/close offset");
}
Decimal FuturesAccount::frozen() const {
  Decimal result;
  for (const auto& item : orders_)
    if (active(item))
      result = result + reserved(item);
  return result;
}
Decimal FuturesAccount::available() const {
  return std::min(balance_, balance_ + unrealized()) - margin() - frozen();
}
Decimal FuturesAccount::closable(const InstrumentId& instrument, Side side,
                                 std::optional<bool> today) const {
  Decimal result;
  for (const auto& lot : lots_)
    if (lot.instrument == instrument && lot.side == side && (!today || lot.today == *today))
      result = result + lot.quantity;
  for (const auto& item : orders_)
    if (active(item) && item.order.request().instrument == instrument &&
        item.offset != Offset::open && item.order.request().side != side &&
        (!today || item.offset == Offset::close || (item.offset == Offset::close_today) == *today))
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
  const auto index = contract_index(request.instrument);
  const auto instrument = contracts_[index].instrument.id;
  Order order(std::move(request), contracts_[index].instrument);
  const auto& accepted = order.request();
  if (marks_[index] == zero)
    throw std::invalid_argument("replay at least one market event of this contract first");
  const auto policy = asterion::close_policy(instrument.venue);
  const bool explicit_close = offset == Offset::close_today || offset == Offset::close_yesterday;
  if (offset != Offset::open && explicit_close != (policy == ClosePolicy::explicit_buckets))
    throw std::invalid_argument(policy == ClosePolicy::explicit_buckets
                                    ? "this venue requires close_today or close_yesterday"
                                    : "this venue assigns buckets itself; use close");
  if (offset != Offset::open &&
      closable(instrument, accepted.side == Side::buy ? Side::sell : Side::buy,
               explicit_close ? std::optional<bool>(offset == Offset::close_today) : std::nullopt) <
          accepted.quantity)
    throw std::invalid_argument(
        "insufficient closable position or already reserved by other orders");
  const auto required = reserved({order, offset});
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
  const auto instrument = order.request().instrument;
  const auto& terms = contracts_[contract_index(instrument)];
  const auto multiplier = terms.instrument.multiplier;
  auto fees = fees_;
  auto balance = balance_;
  auto realized = realized_;
  auto lots = lots_;
  const auto charge = [&](Offset bucket, Decimal quantity) {
    const auto fee = terms.costs.fee(bucket, quantity, report.price, multiplier);
    fees = fees + fee;
    balance = balance - fee;
  };
  if (item.offset == Offset::open) {
    charge(Offset::open, report.quantity);
    lots.push_back({instrument, side, true, report.quantity, report.price});
  } else {
    // Buckets in the order this fill consumes them.
    const auto policy = asterion::close_policy(instrument.venue);
    std::vector<bool> buckets;
    if (item.offset == Offset::close_today)
      buckets = {true};
    else if (item.offset == Offset::close_yesterday)
      buckets = {false};
    else if (policy == ClosePolicy::today_first)
      buckets = {true, false};
    else
      buckets = {false, true};
    auto remaining = report.quantity;
    for (const bool today : buckets) {
      Decimal consumed;
      for (auto& lot : lots) {
        if (remaining == zero)
          break;
        if (lot.instrument != instrument || lot.side == side || lot.today != today)
          continue;
        const auto amount = std::min(remaining, lot.quantity);
        const auto pnl =
            (lot.side == Side::buy ? report.price - lot.price : lot.price - report.price) * amount *
            multiplier;
        balance = balance + pnl;
        realized = realized + pnl;
        lot.quantity = lot.quantity - amount;
        remaining = remaining - amount;
        consumed = consumed + amount;
      }
      if (consumed != zero)
        charge(today ? Offset::close_today : Offset::close_yesterday, consumed);
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
void FuturesAccount::mark(const InstrumentId& instrument, Decimal price) {
  const auto index = contract_index(instrument);
  if (price <= zero || !price.multiple_of(contracts_[index].instrument.price_increment))
    throw std::invalid_argument("invalid mark price");
  const auto previous = marks_[index];
  marks_[index] = price;
  try {
    (void)available();
  } catch (...) {
    marks_[index] = previous;
    throw;
  }
}
void FuturesAccount::settle(const std::vector<Decimal>& prices) {
  if (has_working_orders())
    throw std::invalid_argument("cancel all working orders before settlement");
  if (prices.size() != contracts_.size())
    throw std::invalid_argument("settlement requires one price per contract");
  for (std::size_t i = 0; i < prices.size(); ++i)
    if (prices[i] <= zero || !prices[i].multiple_of(contracts_[i].instrument.price_increment))
      throw std::invalid_argument("invalid settlement price");
  auto previous = marks_;
  marks_ = prices;
  try {
    const auto pnl = unrealized();
    const auto balance = balance_ + pnl;
    const auto realized = realized_ + pnl;
    auto lots = lots_;
    for (auto& lot : lots) {
      lot.today = false;
      lot.price = prices[contract_index(lot.instrument)];
    }
    (void)available();
    lots_ = std::move(lots);
    balance_ = balance;
    realized_ = realized;
  } catch (...) {
    marks_ = std::move(previous);
    throw;
  }
}
Json FuturesAccount::snapshot() const {
  Json positions = Json::array(), orders = Json::array(), fills = Json::array(),
       marks = Json::array();
  for (std::size_t i = 0; i < contracts_.size(); ++i)
    marks.push_back({{"venue", contracts_[i].instrument.id.venue},
                     {"symbol", contracts_[i].instrument.id.symbol},
                     {"mark", marks_[i].str()}});
  for (const auto& lot : lots_)
    positions.push_back({{"venue", lot.instrument.venue},
                         {"symbol", lot.instrument.symbol},
                         {"side", lot.side == Side::buy ? "buy" : "sell"},
                         {"bucket", lot.today ? "today" : "yesterday"},
                         {"quantity", lot.quantity.str()},
                         {"basis", lot.price.str()}});
  for (const auto& item : orders_) {
    const auto& r = item.order.request();
    orders.push_back({{"id", r.id},
                      {"venue", r.instrument.venue},
                      {"symbol", r.instrument.symbol},
                      {"side", r.side == Side::buy ? "buy" : "sell"},
                      {"offset", offset_name(item.offset)},
                      {"quantity", r.quantity.str()},
                      {"limit_price", r.limit_price.str()},
                      {"filled", item.order.filled_quantity().str()},
                      {"state", state_name(item.order.state())}});
  }
  for (const auto& f : fills_) {
    const auto& r = orders_[index_of(f.order_id)].order.request();
    fills.push_back({{"id", f.execution_id},
                     {"order_id", f.order_id},
                     {"venue", r.instrument.venue},
                     {"symbol", r.instrument.symbol},
                     {"quantity", f.quantity.str()},
                     {"price", f.price.str()}});
  }
  return {{"balance", balance_.str()},
          {"equity", (balance_ + unrealized()).str()},
          {"available", available().str()},
          {"margin", margin().str()},
          {"frozen", frozen().str()},
          {"fees", fees_.str()},
          {"realized", realized_.str()},
          {"unrealized", unrealized().str()},
          {"marks", marks},
          {"positions", positions},
          {"orders", orders},
          {"fills", fills}};
}
} // namespace asterion
