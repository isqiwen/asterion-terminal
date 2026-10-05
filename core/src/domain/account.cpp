#include <asterion/domain/account.hpp>
#include <asterion/domain/daily_bars.hpp>
#include <algorithm>
#include <stdexcept>
#include <type_traits>

namespace asterion {
void validate_cost_schedule(const std::vector<FuturesCostVersion>& schedule) {
  if (schedule.empty() || schedule.size() > 512)
    throw std::invalid_argument("cost schedule requires 1 to 512 versions");
  std::string previous;
  for (const auto& version : schedule) {
    (void)parse_trading_date(version.effective_from);
    if (version.effective_from <= previous)
      throw std::invalid_argument("cost versions must have unique ascending effective dates");
    if (version.source.empty() || version.source.size() > 256 ||
        version.source.find_first_not_of(" \t\r\n") == std::string::npos)
      throw std::invalid_argument("cost version requires a source of at most 256 bytes");
    version.values.validate();
    previous = version.effective_from;
  }
}
const FuturesCostVersion& costs_on(const std::vector<FuturesCostVersion>& schedule,
                                   const std::string& trading_day) {
  const auto next = std::upper_bound(
      schedule.begin(), schedule.end(), trading_day,
      [](const std::string& day, const auto& version) { return day < version.effective_from; });
  if (next == schedule.begin())
    throw std::invalid_argument("cost schedule does not cover the first trading day");
  return *std::prev(next);
}
void FuturesAccount::update_costs(const std::vector<FuturesCosts>& costs) {
  if (costs.size() != contracts_.size() || has_working_orders())
    throw std::invalid_argument("cost changes require every contract and no working orders");
  std::vector<FuturesCosts> previous;
  previous.reserve(costs.size());
  for (std::size_t i = 0; i < costs.size(); ++i) {
    costs[i].validate();
    previous.push_back(contracts_[i].costs);
  }
  for (std::size_t i = 0; i < costs.size(); ++i)
    contracts_[i].costs = costs[i];
  try {
    (void)available();
  } catch (...) {
    for (std::size_t i = 0; i < previous.size(); ++i)
      contracts_[i].costs = previous[i];
    throw;
  }
}
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
struct FuturesAccount::Transaction::State {
  FuturesAccount* owner;
  std::vector<Decimal> marks;
  std::vector<FuturesCosts> costs;
  std::vector<PositionLot> lots;
  std::vector<std::pair<std::size_t, AccountOrder>> orders;
  std::set<std::size_t> working;
  std::size_t order_count, fill_count;
  Decimal balance, fees, realized;
  explicit State(FuturesAccount& account)
      : owner(&account), marks(account.marks_), lots(account.lots_),
        working(account.working_orders_), order_count(account.orders_.size()),
        fill_count(account.fills_.size()), balance(account.balance_), fees(account.fees_),
        realized(account.realized_) {
    for (const auto& contract : account.contracts_)
      costs.push_back(contract.costs);
    orders.reserve(working.size());
    for (const auto index : working)
      orders.emplace_back(index, account.orders_[index]);
  }
  ~State() noexcept {
    if (!owner)
      return;
    auto& account = *owner;
    while (account.fills_.size() > fill_count) {
      account.fill_index_.erase(account.fills_.back().execution_id);
      account.fills_.pop_back();
    }
    while (account.orders_.size() > order_count) {
      account.order_index_.erase(account.orders_.back().order.request().id);
      account.orders_.pop_back();
    }
    for (auto& [index, order] : orders)
      account.orders_[index] = std::move(order);
    account.working_orders_.swap(working);
    account.marks_.swap(marks);
    account.lots_.swap(lots);
    for (std::size_t i = 0; i < costs.size(); ++i)
      account.contracts_[i].costs = costs[i];
    account.balance_ = balance;
    account.fees_ = fees;
    account.realized_ = realized;
  }
};
static_assert(std::is_nothrow_move_assignable_v<AccountOrder>);
static_assert(std::is_nothrow_copy_assignable_v<FuturesCosts>);
FuturesAccount::Transaction::Transaction(FuturesAccount& account)
    : state_(std::make_unique<State>(account)) {}
FuturesAccount::Transaction::Transaction(Transaction&&) noexcept = default;
FuturesAccount::Transaction::~Transaction() = default;
void FuturesAccount::Transaction::commit() noexcept {
  if (state_) {
    state_->owner = nullptr;
    state_.reset();
  }
}
FuturesAccount::Transaction FuturesAccount::transaction() {
  return Transaction(*this);
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
  return !working_orders_.empty();
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
  for (const auto index : working_orders_)
    result = result + reserved(orders_[index]);
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
  for (const auto index : working_orders_) {
    const auto& item = orders_[index];
    if (item.order.request().instrument == instrument && item.offset != Offset::open &&
        item.order.request().side != side &&
        (!today || item.offset == Offset::close || (item.offset == Offset::close_today) == *today))
      result = result - item.order.remaining_quantity();
  }
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
  const auto ordinal = orders_.size();
  const auto working = working_orders_.insert(ordinal).first;
  try {
    orders_.push_back({std::move(order), offset});
    try {
      order_index_.emplace(std::move(id), ordinal);
    } catch (...) {
      orders_.pop_back();
      throw;
    }
  } catch (...) {
    working_orders_.erase(working);
    throw;
  }
}
void FuturesAccount::cancel(const std::string& id) {
  const auto index = index_of(id);
  orders_[index].order.cancel();
  working_orders_.erase(index);
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
  const auto index = index_of(report.order_id);
  auto& item = orders_[index];
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
  // Prepare capacity before publishing ledger changes, with geometric growth.
  // reserve(size + 1) reallocates the entire fill history on every execution.
  if (fills_.size() == fills_.max_size())
    throw std::length_error("futures account fill capacity exhausted");
  if (fills_.size() == fills_.capacity())
    fills_.reserve(std::min(fills_.max_size(), std::max<std::size_t>(1, fills_.capacity() * 2)));
  fill_index_.emplace(report.execution_id, fills_.size());
  // Commit: no operation below can throw.
  item.order = std::move(order);
  if (!active(item))
    working_orders_.erase(index);
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
  settle_traded(std::vector<std::optional<Decimal>>(prices.begin(), prices.end()));
}
void FuturesAccount::settle_traded(const std::vector<std::optional<Decimal>>& prices) {
  if (has_working_orders())
    throw std::invalid_argument("cancel all working orders before settlement");
  if (prices.size() != contracts_.size())
    throw std::invalid_argument("settlement requires one price per contract");
  auto previous = marks_;
  auto next = marks_;
  for (std::size_t i = 0; i < prices.size(); ++i) {
    const auto& id = contracts_[i].instrument.id;
    if (!prices[i]) {
      if (std::ranges::any_of(lots_, [&](const auto& lot) { return lot.instrument == id; }))
        throw std::invalid_argument(
            "a position remains in a contract on a day it has no settlement price: " + id.symbol);
      continue;
    }
    if (*prices[i] <= zero || !prices[i]->multiple_of(contracts_[i].instrument.price_increment))
      throw std::invalid_argument("invalid settlement price");
    next[i] = *prices[i];
  }
  marks_ = next;
  try {
    const auto pnl = unrealized();
    const auto balance = balance_ + pnl;
    const auto realized = realized_ + pnl;
    auto lots = lots_;
    for (auto& lot : lots) {
      lot.today = false;
      lot.price = *prices[contract_index(lot.instrument)];
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
