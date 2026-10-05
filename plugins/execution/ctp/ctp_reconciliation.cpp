#include "ctp_reconciliation.hpp"
#include "ctp_support.hpp"
#include "ctp_feed.hpp"
#include <algorithm>
namespace asterion::ctp {
namespace {
bool current_day(const std::string& day, const BrokerSnapshot& state) {
  if (day.empty())
    throw std::runtime_error("CTP execution report has no trading day");
  return day == state.trading_day;
}
Decimal lots(int value) {
  return Decimal::parse(std::to_string(std::max(0, value)));
}
Side side_of(char direction) {
  return direction == THOST_FTDC_D_Sell ? Side::sell : Side::buy;
}
Offset offset_of(char flag) {
  switch (flag) {
  case THOST_FTDC_OF_Open:
    return Offset::open;
  case THOST_FTDC_OF_CloseToday:
    return Offset::close_today;
  case THOST_FTDC_OF_CloseYesterday:
    return Offset::close_yesterday;
  default:
    return Offset::close;
  }
}
BrokerOrderStatus status_of(const CThostFtdcOrderField& order) {
  if (order.OrderSubmitStatus == THOST_FTDC_OSS_InsertRejected)
    return BrokerOrderStatus::rejected;
  switch (order.OrderStatus) {
  case THOST_FTDC_OST_AllTraded:
    return BrokerOrderStatus::filled;
  case THOST_FTDC_OST_PartTradedQueueing:
    return BrokerOrderStatus::partially_filled;
  case THOST_FTDC_OST_NoTradeQueueing:
  case THOST_FTDC_OST_NotTouched:
  case THOST_FTDC_OST_Touched:
    return BrokerOrderStatus::accepted;
  case THOST_FTDC_OST_PartTradedNotQueueing:
  case THOST_FTDC_OST_NoTradeNotQueueing:
  case THOST_FTDC_OST_Canceled:
    return BrokerOrderStatus::cancelled;
  default:
    return BrokerOrderStatus::submitted;
  }
}
bool terminal(BrokerOrderStatus status) {
  return status == BrokerOrderStatus::filled || status == BrokerOrderStatus::cancelled ||
         status == BrokerOrderStatus::rejected;
}

} // namespace
bool ReportReconciler::fills_reported() const {
  return std::ranges::all_of(state_.orders, [&](const BrokerOrder& order) {
    if (order.filled == Decimal{})
      return true;
    const auto found = traded_quantities_.find(order.exchange_order_id);
    return found != traded_quantities_.end() && found->second >= order.filled;
  });
}
std::string ReportReconciler::key(int front, int session, const std::string& ref) {
  return std::to_string(front) + ":" + std::to_string(session) + ":" + ref;
}
BrokerOrder& ReportReconciler::order_for(const std::string& broker_key) {
  if (const auto found = order_at_.find(broker_key); found != order_at_.end())
    return state_.orders[found->second];
  BrokerOrder order;
  order.broker_key = broker_key;
  if (const auto id = known_.find({state_.trading_day, broker_key}); id != known_.end()) {
    order.order_id = id->second.order_id;
    keys_[id->second.order_id] = {state_.trading_day, broker_key};
  }
  order_at_[broker_key] = state_.orders.size();
  state_.orders.push_back(std::move(order));
  return state_.orders.back();
}
bool ReportReconciler::apply(const CThostFtdcOrderField& report) {
  if (!current_day(field(report.TradingDay), state_))
    return false;
  auto& order = order_for(key(report.FrontID, report.SessionID, trimmed(report.OrderRef)));
  const auto reported_filled = lots(report.VolumeTraded);
  const bool additional_fill = reported_filled > order.filled;
  order.instrument = {field(report.ExchangeID), field(report.InstrumentID)};
  order.side = side_of(report.Direction);
  order.offset = offset_of(report.CombOffsetFlag[0]);
  order.quantity = lots(report.VolumeTotalOriginal);
  order.filled = std::max(order.filled, reported_filled);
  if (const auto limit = price(report.LimitPrice))
    order.limit_price = *limit;
  if (const auto id = trimmed(report.OrderSysID); !id.empty()) {
    order.exchange_order_id = order.instrument.venue + ":" + id;
    exchange_[order.exchange_order_id] = order.broker_key;
    auto& raw = raw_order_ids_[order.broker_key];
    std::copy(std::begin(report.OrderSysID), std::end(report.OrderSysID), raw.begin());
  }
  // A late report never moves a terminal order back to working.
  const auto status = status_of(report);
  if (!terminal(order.status) || terminal(status))
    order.status = status;
  ++state_.exposure_revision;
  if (additional_fill)
    state_.positions_reconciled = false;
  ++state_.sequence;
  return additional_fill;
}
bool ReportReconciler::apply(const CThostFtdcTradeField& report) {
  if (!current_day(field(report.TradingDay), state_))
    return false;
  const auto venue = field(report.ExchangeID);
  BrokerTrade trade;
  trade.trade_id = venue + ":" + trimmed(report.TradeID);
  if (!trade_ids_.insert(trade.trade_id).second)
    return false;
  trade.instrument = {venue, field(report.InstrumentID)};
  trade.exchange_order_id = venue + ":" + trimmed(report.OrderSysID);
  if (const auto found = exchange_.find(trade.exchange_order_id); found != exchange_.end())
    trade.order_id = state_.orders[order_at_.at(found->second)].order_id;
  trade.side = side_of(report.Direction);
  trade.offset = offset_of(report.OffsetFlag);
  trade.quantity = lots(report.Volume);
  const auto fill = price(report.Price);
  if (!fill)
    throw std::runtime_error("invalid CTP trade price");
  trade.price = *fill;
  trade.trading_day = field(report.TradingDay);
  trade.trade_time = field(report.TradeDate) + " " + field(report.TradeTime);
  traded_quantities_[trade.exchange_order_id] =
      traded_quantities_[trade.exchange_order_id] + trade.quantity;
  state_.trades.push_back(std::move(trade));
  ++state_.exposure_revision;
  state_.positions_reconciled = false;
  ++state_.sequence;
  return true;
}
void ReportReconciler::reject(const BrokerOrder& request, int code) {
  auto& order = order_for(request.broker_key);
  order = request;
  order.status = BrokerOrderStatus::rejected;
  order.error_code = code;
  ++state_.exposure_revision;
  ++state_.sequence;
}

void ReportReconciler::reset(KnownOrders known) {
  known_ = std::move(known);
  keys_.clear();
  for (const auto& [key, order] : known_)
    keys_[order.order_id] = key;
  clear_reports();
}
void ReportReconciler::clear_reports() {
  order_at_.clear();
  exchange_.clear();
  raw_order_ids_.clear();
  trade_ids_.clear();
  traded_quantities_.clear();
  abandon_positions();
  state_.orders.clear();
  state_.trades.clear();
  state_.positions.clear();
  state_.positions_reconciled = false;
}
void ReportReconciler::abandon_positions() {
  positions_in_progress_.clear();
}
void ReportReconciler::begin_day(std::string day) {
  state_.trading_day = std::move(day);
  std::erase_if(known_,
                [&](const auto& entry) { return entry.first.trading_day != state_.trading_day; });
  keys_.clear();
  for (const auto& [identity, order] : known_)
    keys_[order.order_id] = identity;
  clear_reports();
}
void ReportReconciler::apply(const CThostFtdcInvestorPositionField& row) {
  if (row.PosiDirection != THOST_FTDC_PD_Long && row.PosiDirection != THOST_FTDC_PD_Short)
    return;
  const InstrumentId id{field(row.ExchangeID), field(row.InstrumentID)};
  const auto side = row.PosiDirection == THOST_FTDC_PD_Long ? Side::buy : Side::sell;
  auto& position = positions_in_progress_[{id, side}];
  position.instrument = id;
  position.side = side;
  // SHFE/INE split today/history rows; TodayPosition handles both layouts.
  position.today = position.today + lots(row.TodayPosition);
  position.yesterday = position.yesterday + lots(row.Position - row.TodayPosition);
}
bool ReportReconciler::finish_positions(std::uint64_t query_revision) {
  state_.positions.clear();
  for (auto& [_, position] : positions_in_progress_)
    if (position.today > Decimal{} || position.yesterday > Decimal{})
      state_.positions.push_back(std::move(position));
  positions_in_progress_.clear();
  const bool changed = query_revision != state_.exposure_revision;
  state_.positions_reconciled = !changed && fills_reported();
  ++state_.exposure_revision;
  ++state_.sequence;
  return changed;
}
void ReportReconciler::remember(OrderIdentity identity, KnownOrder order) {
  // A request that never reached the broker may have used a reference which
  // login later makes available again. Retire the previous reverse binding.
  if (const auto previous = known_.find(identity); previous != known_.end()) {
    if (previous->second.sequence >= order.sequence)
      return;
    keys_.erase(previous->second.order_id);
  }
  keys_[order.order_id] = identity;
  if (identity.trading_day == state_.trading_day) {
    const auto at = order_at_.find(identity.broker_key);
    if (at != order_at_.end()) {
      auto& reported = state_.orders[at->second];
      if (reported.order_id != order.order_id) {
        reported.order_id = order.order_id;
        for (auto& trade : state_.trades)
          if (trade.exchange_order_id == reported.exchange_order_id)
            trade.order_id = order.order_id;
        ++state_.sequence;
        ++state_.exposure_revision;
      }
    }
  }
  known_[std::move(identity)] = std::move(order);
}
bool ReportReconciler::contains(const std::string& order_id) const {
  return keys_.contains(order_id);
}
const BrokerOrder* ReportReconciler::find(const std::string& broker_key) const {
  const auto found = order_at_.find(broker_key);
  return found == order_at_.end() ? nullptr : &state_.orders[found->second];
}
const BrokerOrder* ReportReconciler::find_order(const std::string& order_id) const {
  const auto key = keys_.find(order_id);
  return key == keys_.end() || key->second.trading_day != state_.trading_day
             ? nullptr
             : find(key->second.broker_key);
}
const ReportReconciler::RawOrderId&
ReportReconciler::raw_exchange_id(const std::string& key) const {
  return raw_order_ids_.at(key);
}
void ReportReconciler::action_failed(const std::string& broker_key, int code) {
  if (const auto found = order_at_.find(broker_key); found != order_at_.end()) {
    state_.orders[found->second].error_code = code;
    ++state_.sequence;
  }
}

} // namespace asterion::ctp
