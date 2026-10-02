#include <array>
#include "ctp_trader.hpp"
#include "ctp_feed.hpp"
#include "ctp_support.hpp"
#include <ThostFtdcTraderApi.h>
#include <asterion/foundation/error.hpp>
#include <charconv>
#include <condition_variable>
#include <deque>
#include <future>
#include <locale>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <thread>
#include <utility>
namespace asterion::ctp {
namespace {
using namespace std::chrono_literals;
// CTP allows one query per second per session; keep a margin.
constexpr auto query_spacing = 1100ms;
constexpr auto query_timeout = 10s;
constexpr auto request_timeout = 5s;
enum class Kind {
  authenticate,
  login,
  confirm,
  query_orders,
  query_trades,
  query_positions,
  query_funds,
  query_margin,
  query_commission,
  query_quote,
  insert,
  cancel
};
bool is_query(Kind kind) {
  return kind == Kind::query_orders || kind == Kind::query_trades ||
         kind == Kind::query_positions || kind == Kind::query_funds || kind == Kind::query_margin ||
         kind == Kind::query_commission || kind == Kind::query_quote;
}
bool is_rate_query(Kind kind) {
  return kind == Kind::query_margin || kind == Kind::query_commission;
}
struct Command {
  Kind kind;
  std::uint64_t generation = 0, query_generation = 0;
  int query_request_id = 0;
  Kind query_kind = Kind::query_orders;
  CThostFtdcInputOrderField order{};
  CThostFtdcInputOrderActionField action{};
  // Rate queries: the contract and its product code.
  InstrumentId instrument;
  std::string product;
  std::uint64_t serial = 0; // quote queries
  std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
  std::shared_ptr<std::promise<int>> result;
};
Decimal rate(double value) {
  const auto parsed = price(value);
  if (!parsed || *parsed < Decimal{})
    throw std::runtime_error("invalid CTP rate");
  return *parsed;
}
Decimal money(double value) {
  const auto parsed = price(value);
  if (!parsed)
    throw std::runtime_error("invalid CTP money value");
  return quantize(*parsed, Decimal::parse("0.01"), Rounding::half_up);
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
char offset_flag(Offset offset) {
  switch (offset) {
  case Offset::open:
    return THOST_FTDC_OF_Open;
  case Offset::close_today:
    return THOST_FTDC_OF_CloseToday;
  case Offset::close_yesterday:
    return THOST_FTDC_OF_CloseYesterday;
  case Offset::close:
    return THOST_FTDC_OF_Close;
  }
  return THOST_FTDC_OF_Open;
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
int whole_lots(Decimal quantity) {
  if (quantity <= Decimal{} || !quantity.multiple_of(Decimal::parse("1")) ||
      quantity > Decimal::parse("1000000"))
    throw std::invalid_argument("CTP orders need a whole number of lots between 1 and 1000000");
  return static_cast<int>(quantity.raw() / Decimal::parse("1").raw());
}
double sdk_price(Decimal value) {
  if (value <= Decimal{})
    throw std::invalid_argument("CTP limit price must be positive");
  std::istringstream text(value.str());
  text.imbue(std::locale::classic());
  double result = 0;
  text >> result;
  if (!text || !text.eof())
    throw std::invalid_argument("invalid CTP limit price");
  return result;
}
} // namespace
struct Trader::Impl final : CThostFtdcTraderSpi {
  using Factory = CThostFtdcTraderApi* (*)(const char*);
  std::unique_ptr<SharedLibrary> library;
  Factory factory = nullptr;
  std::filesystem::path flow;
  CThostFtdcTraderApi* api = nullptr;
  mutable std::mutex mutex;
  std::condition_variable wake;
  TraderConfiguration config;
  BrokerSnapshot state;
  std::map<std::string, std::string> known;    // broker key -> order ID
  std::map<std::string, std::string> keys;     // order ID -> broker key
  std::map<std::string, std::size_t> order_at; // broker key -> index
  std::map<std::string, std::string> exchange; // exchange order ID -> key
  // CTP cancellation must echo the SDK identity, including its leading spaces.
  std::map<std::string, std::array<char, sizeof(TThostFtdcOrderSysIDType)>> raw_order_ids;
  std::set<std::string> trade_ids;
  std::map<std::pair<InstrumentId, Side>, BrokerPosition> positions_in_progress;
  // Rate answers per contract until both queries complete.
  struct Rates {
    std::string product;
    bool margin_done = false, commission_done = false;
    std::optional<std::pair<Decimal, Decimal>> margin; // per lot, by money
    std::optional<std::array<Decimal, 6>> commission;
  };
  std::map<InstrumentId, Rates> rates;
  InstrumentId rate_instrument; // the outstanding rate query
  // Quote answers by request serial; the waiting caller takes its own.
  std::uint64_t quote_serial = 0, quote_outstanding = 0;
  std::map<std::uint64_t, std::optional<BrokerQuote>> quotes;
  std::deque<Command> queue;
  // Session identity from the latest login; generation changes on every
  // disconnect so requests prepared for an older session are never sent.
  int front_id = 0, session_id = 0, next_ref = 0, request_id = 0;
  std::uint64_t generation = 0, query_generation = 0;
  int query_request_id = 0;
  Kind query_kind = Kind::query_orders;
  bool closing = true, query_outstanding = false, refresh_queued = false;
  std::chrono::steady_clock::time_point query_started, next_query_at;
  std::jthread commands, retiring;

  explicit Impl(const std::filesystem::path& path, const std::filesystem::path& directory)
      : flow(directory) {
    library = std::make_unique<SharedLibrary>(
        path, "?CreateFtdcTraderApi@CThostFtdcTraderApi@@SAPEAV1@PEBD@Z",
        "_ZN19CThostFtdcTraderApi19CreateFtdcTraderApiEPKc");
    factory = library->symbol<Factory>();
    if (!factory) {
      library.reset();
      throw Error(ErrorCode::unavailable, "CTP 6.7.7 trader SDK unavailable or incompatible");
    }
  }
  ~Impl() {
    close();
    finish_release();
    library.reset();
  }
  void changed() { ++state.sequence; }
  void fail(int code) {
    state.phase = "error";
    state.error_code = code;
    changed();
  }
  // Callbacks run on SDK threads; they only update state and queue work.
  template <class F> void callback(F action) noexcept {
    try {
      std::lock_guard lock(mutex);
      if (!closing)
        action();
    } catch (...) {
      try {
        std::lock_guard lock(mutex);
        fail(-1000);
      } catch (...) {
      }
    }
    wake.notify_all();
  }
  void push(Kind kind) {
    Command command;
    command.kind = kind;
    command.generation = generation;
    queue.push_back(std::move(command));
  }
  void synchronize() {
    state.phase = "synchronizing";
    changed();
    push(Kind::query_orders);
    push(Kind::query_trades);
    push(Kind::query_positions);
    push(Kind::query_funds);
  }
  // Positions and funds after a trade; one refresh is queued at a time.
  void refresh() {
    if (refresh_queued || state.phase != "ready")
      return;
    refresh_queued = true;
    push(Kind::query_positions);
    push(Kind::query_funds);
  }
  std::string key(int front, int session, const std::string& ref) const {
    return std::to_string(front) + ":" + std::to_string(session) + ":" + ref;
  }
  BrokerOrder& order_for(const std::string& broker_key) {
    if (const auto found = order_at.find(broker_key); found != order_at.end())
      return state.orders[found->second];
    BrokerOrder order;
    order.broker_key = broker_key;
    if (const auto id = known.find(broker_key); id != known.end()) {
      order.order_id = id->second;
      keys[id->second] = broker_key;
    }
    order_at[broker_key] = state.orders.size();
    state.orders.push_back(std::move(order));
    return state.orders.back();
  }
  void apply(const CThostFtdcOrderField& report) {
    auto& order = order_for(key(report.FrontID, report.SessionID, trimmed(report.OrderRef)));
    order.instrument = {field(report.ExchangeID), field(report.InstrumentID)};
    order.side = side_of(report.Direction);
    order.offset = offset_of(report.CombOffsetFlag[0]);
    order.quantity = lots(report.VolumeTotalOriginal);
    order.filled = lots(report.VolumeTraded);
    if (const auto limit = price(report.LimitPrice))
      order.limit_price = *limit;
    if (const auto id = trimmed(report.OrderSysID); !id.empty()) {
      order.exchange_order_id = order.instrument.venue + ":" + id;
      exchange[order.exchange_order_id] = order.broker_key;
      auto& raw = raw_order_ids[order.broker_key];
      std::copy(std::begin(report.OrderSysID), std::end(report.OrderSysID), raw.begin());
    }
    // A late report never moves a terminal order back to working.
    const auto status = status_of(report);
    if (!terminal(order.status) || terminal(status))
      order.status = status;
    changed();
  }
  bool apply(const CThostFtdcTradeField& report) {
    const auto venue = field(report.ExchangeID);
    BrokerTrade trade;
    trade.trade_id = venue + ":" + trimmed(report.TradeID);
    if (!trade_ids.insert(trade.trade_id).second)
      return false;
    trade.instrument = {venue, field(report.InstrumentID)};
    trade.exchange_order_id = venue + ":" + trimmed(report.OrderSysID);
    if (const auto found = exchange.find(trade.exchange_order_id); found != exchange.end())
      trade.order_id = state.orders[order_at.at(found->second)].order_id;
    trade.side = side_of(report.Direction);
    trade.offset = offset_of(report.OffsetFlag);
    trade.quantity = lots(report.Volume);
    const auto fill = price(report.Price);
    if (!fill)
      throw std::runtime_error("invalid CTP trade price");
    trade.price = *fill;
    trade.trading_day = field(report.TradingDay);
    trade.trade_time = field(report.TradeDate) + " " + field(report.TradeTime);
    state.trades.push_back(std::move(trade));
    changed();
    return true;
  }
  void reject(const std::string& ref, int code) {
    const auto found = order_at.find(key(front_id, session_id, ref));
    if (found == order_at.end())
      return;
    auto& order = state.orders[found->second];
    order.status = BrokerOrderStatus::rejected;
    order.error_code = code;
    changed();
  }
  void query_done() { query_outstanding = false; }
  bool accepts_query(int id, Kind kind) const {
    return query_outstanding && id == query_request_id && kind == query_kind &&
           query_generation == generation;
  }
  // Rate queries dropped with their session report as unavailable.
  void abandon_rates() {
    for (const auto& [instrument, _] : rates) {
      auto& costs = costs_for(instrument);
      costs.state = "unavailable";
      costs.error_code = -1003;
    }
    rates.clear();
  }
  BrokerCosts& costs_for(const InstrumentId& id) {
    for (auto& entry : state.costs)
      if (entry.instrument == id)
        return entry;
    BrokerCosts entry;
    entry.instrument = id;
    state.costs.push_back(std::move(entry));
    return state.costs.back();
  }
  // Records one rate answer; both answered completes the contract's costs.
  void rate_answered(Kind kind, int error_code) {
    auto& entry = rates[rate_instrument];
    (kind == Kind::query_margin ? entry.margin_done : entry.commission_done) = true;
    auto& costs = costs_for(rate_instrument);
    if (error_code)
      costs.error_code = error_code;
    if (entry.margin_done && entry.commission_done) {
      costs.queried_ms = now_ms();
      if (entry.margin && entry.commission && !costs.error_code) {
        const auto& c = *entry.commission;
        costs.costs = FuturesCosts{entry.margin->first,  c[0], c[4], c[2],
                                   entry.margin->second, c[1], c[5], c[3]};
        costs.state = "ready";
      } else
        costs.state = "unavailable";
      rates.erase(rate_instrument);
    }
    changed();
  }
  // Commission rows may name the product rather than the contract.
  bool rate_row_matches(const char* instrument) const {
    const auto found = rates.find(rate_instrument);
    const auto name = trimmed_text(instrument);
    return name == rate_instrument.symbol ||
           (found != rates.end() && name == found->second.product);
  }
  static std::string trimmed_text(const char* value) {
    std::string text(value);
    while (!text.empty() && text.back() == ' ')
      text.pop_back();
    return text;
  }

  // ---- SDK callbacks ----
  void OnFrontConnected() override {
    callback([&] {
      state.phase = "authenticating";
      state.error_code = 0;
      changed();
      push(Kind::authenticate);
    });
  }
  void OnFrontDisconnected(int reason) override {
    callback([&] {
      ++generation;
      query_outstanding = refresh_queued = false;
      std::erase_if(queue, [](const Command& c) { return !c.result; });
      abandon_rates();
      positions_in_progress.clear();
      state.costs.clear();
      state.phase = "connecting";
      state.error_code = reason;
      changed();
    });
  }
  void OnRspAuthenticate(CThostFtdcRspAuthenticateField*, CThostFtdcRspInfoField* info, int,
                         bool) override {
    callback([&] {
      if (info && info->ErrorID) {
        erase(config.password);
        erase(config.auth_code);
        return fail(info->ErrorID);
      }
      state.phase = "logging_in";
      changed();
      push(Kind::login);
    });
  }
  void OnRspUserLogin(CThostFtdcRspUserLoginField* login, CThostFtdcRspInfoField* info, int,
                      bool) override {
    callback([&] {
      if (info && info->ErrorID) {
        erase(config.password);
        erase(config.auth_code);
        return fail(info->ErrorID);
      }
      if (!login)
        return fail(-1001);
      front_id = login->FrontID;
      session_id = login->SessionID;
      int max_ref = 0;
      const auto ref = trimmed(login->MaxOrderRef);
      std::from_chars(ref.data(), ref.data() + ref.size(), max_ref);
      next_ref = max_ref;
      // Login starts a new broker reconciliation, including automatic SDK
      // reconnects. Historical journal identities survive in known, but cached
      // reports cannot prove that the broker still confirms an order. Daily
      // exchange IDs may be reused and yesterday's rates are not today's rates.
      keys.clear();
      order_at.clear();
      exchange.clear();
      raw_order_ids.clear();
      trade_ids.clear();
      positions_in_progress.clear();
      rates.clear();
      state.orders.clear();
      state.trades.clear();
      state.positions.clear();
      state.funds.reset();
      state.costs.clear();
      state.synchronized_ms = 0;
      state.trading_day = field(login->TradingDay);
      state.phase = "confirming";
      changed();
      push(Kind::confirm);
    });
  }
  void OnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField*,
                                  CThostFtdcRspInfoField* info, int, bool) override {
    callback([&] {
      if (info && info->ErrorID)
        return fail(info->ErrorID);
      synchronize();
    });
  }
  void OnRspQryOrder(CThostFtdcOrderField* order, CThostFtdcRspInfoField* info, int request,
                     bool last) override {
    callback([&] {
      if (!accepts_query(request, Kind::query_orders))
        return;
      if (info && info->ErrorID)
        return fail(info->ErrorID);
      if (order)
        apply(*order);
      if (last)
        query_done();
    });
  }
  void OnRspQryTrade(CThostFtdcTradeField* trade, CThostFtdcRspInfoField* info, int request,
                     bool last) override {
    callback([&] {
      if (!accepts_query(request, Kind::query_trades))
        return;
      if (info && info->ErrorID)
        return fail(info->ErrorID);
      if (trade)
        apply(*trade);
      if (last)
        query_done();
    });
  }
  void OnRspQryInvestorPosition(CThostFtdcInvestorPositionField* row, CThostFtdcRspInfoField* info,
                                int request, bool last) override {
    callback([&] {
      if (!accepts_query(request, Kind::query_positions))
        return;
      if (info && info->ErrorID)
        return fail(info->ErrorID);
      if (row &&
          (row->PosiDirection == THOST_FTDC_PD_Long || row->PosiDirection == THOST_FTDC_PD_Short)) {
        const InstrumentId id{field(row->ExchangeID), field(row->InstrumentID)};
        const auto side = row->PosiDirection == THOST_FTDC_PD_Long ? Side::buy : Side::sell;
        auto& position = positions_in_progress[{id, side}];
        position.instrument = id;
        position.side = side;
        // SHFE/INE report today and history rows separately; other venues
        // report one row. TodayPosition covers both layouts.
        position.today = position.today + lots(row->TodayPosition);
        position.yesterday = position.yesterday + lots(row->Position - row->TodayPosition);
      }
      if (last) {
        state.positions.clear();
        for (auto& [_, position] : positions_in_progress)
          if (position.today > Decimal{} || position.yesterday > Decimal{})
            state.positions.push_back(std::move(position));
        positions_in_progress.clear();
        changed();
        query_done();
      }
    });
  }
  void OnRspQryTradingAccount(CThostFtdcTradingAccountField* account, CThostFtdcRspInfoField* info,
                              int request, bool last) override {
    callback([&] {
      if (!accepts_query(request, Kind::query_funds))
        return;
      if (info && info->ErrorID)
        return fail(info->ErrorID);
      if (account)
        state.funds = BrokerFunds{money(account->Balance),     money(account->Available),
                                  money(account->CurrMargin),  money(account->Commission),
                                  money(account->CloseProfit), money(account->PositionProfit)};
      if (last) {
        query_done();
        refresh_queued = false;
        state.synchronized_ms = now_ms();
        if (state.phase == "synchronizing")
          state.phase = "ready";
        changed();
      }
    });
  }
  void OnRspQryInstrumentMarginRate(CThostFtdcInstrumentMarginRateField* row,
                                    CThostFtdcRspInfoField* info, int request, bool last) override {
    callback([&] {
      if (!accepts_query(request, Kind::query_margin))
        return;
      const int code = info ? info->ErrorID : 0;
      if (!code && row && rate_row_matches(row->InstrumentID))
        rates[rate_instrument].margin = std::pair{
            std::max(rate(row->LongMarginRatioByVolume), rate(row->ShortMarginRatioByVolume)),
            std::max(rate(row->LongMarginRatioByMoney), rate(row->ShortMarginRatioByMoney))};
      if (last || code) {
        query_done();
        rate_answered(Kind::query_margin, code);
      }
    });
  }
  void OnRspQryInstrumentCommissionRate(CThostFtdcInstrumentCommissionRateField* row,
                                        CThostFtdcRspInfoField* info, int request,
                                        bool last) override {
    callback([&] {
      if (!accepts_query(request, Kind::query_commission))
        return;
      const int code = info ? info->ErrorID : 0;
      // Fixed order: open, close (yesterday), close today; each per lot, by money.
      if (!code && row && rate_row_matches(row->InstrumentID))
        rates[rate_instrument].commission =
            std::array{rate(row->OpenRatioByVolume),       rate(row->OpenRatioByMoney),
                       rate(row->CloseRatioByVolume),      rate(row->CloseRatioByMoney),
                       rate(row->CloseTodayRatioByVolume), rate(row->CloseTodayRatioByMoney)};
      if (last || code) {
        query_done();
        rate_answered(Kind::query_commission, code);
      }
    });
  }
  void OnRspQryDepthMarketData(CThostFtdcDepthMarketDataField* row, CThostFtdcRspInfoField* info,
                               int request, bool last) override {
    callback([&] {
      if (!accepts_query(request, Kind::query_quote))
        return;
      const int code = info ? info->ErrorID : 0;
      const auto found = quotes.find(quote_outstanding);
      if (found == quotes.end()) {
        if (last || code)
          query_done();
        return; // A timed-out caller has retired this quote.
      }
      auto& answer = found->second;
      if (!code && row && !answer) {
        BrokerQuote quote;
        quote.instrument = {field(row->ExchangeID), field(row->InstrumentID)};
        quote.last = price(row->LastPrice);
        quote.pre_settlement = price(row->PreSettlementPrice);
        quote.upper_limit = price(row->UpperLimitPrice);
        quote.lower_limit = price(row->LowerLimitPrice);
        quote.trading_day = field(row->TradingDay);
        quote.update_time = field(row->UpdateTime);
        answer = std::move(quote);
      }
      if (last || code)
        query_done();
    });
  }
  void OnRtnOrder(CThostFtdcOrderField* order) override {
    callback([&] {
      if (order)
        apply(*order);
    });
  }
  void OnRtnTrade(CThostFtdcTradeField* trade) override {
    callback([&] {
      if (trade && apply(*trade))
        refresh();
    });
  }
  void OnRspOrderInsert(CThostFtdcInputOrderField* input, CThostFtdcRspInfoField* info, int,
                        bool) override {
    callback([&] {
      if (input && info && info->ErrorID)
        reject(trimmed(input->OrderRef), info->ErrorID);
    });
  }
  void OnErrRtnOrderInsert(CThostFtdcInputOrderField* input,
                           CThostFtdcRspInfoField* info) override {
    OnRspOrderInsert(input, info, 0, true);
  }
  void action_failed(const std::string& broker_key, int code) {
    if (const auto found = order_at.find(broker_key); found != order_at.end()) {
      state.orders[found->second].error_code = code;
      changed();
    }
  }
  void OnRspOrderAction(CThostFtdcInputOrderActionField* input, CThostFtdcRspInfoField* info, int,
                        bool) override {
    callback([&] {
      if (input && info && info->ErrorID)
        action_failed(key(input->FrontID, input->SessionID, trimmed(input->OrderRef)),
                      info->ErrorID);
    });
  }
  void OnErrRtnOrderAction(CThostFtdcOrderActionField* input,
                           CThostFtdcRspInfoField* info) override {
    callback([&] {
      if (input && info && info->ErrorID)
        action_failed(key(input->FrontID, input->SessionID, trimmed(input->OrderRef)),
                      info->ErrorID);
    });
  }
  void OnRspError(CThostFtdcRspInfoField* info, int, bool) override {
    callback([&] {
      if (info && info->ErrorID)
        fail(info->ErrorID);
    });
  }

  // ---- worker ----
  // Session requests: authenticate, login and settlement confirmation.
  int send(const Command& command) {
    const int id = ++request_id;
    switch (command.kind) {
    case Kind::authenticate: {
      CThostFtdcReqAuthenticateField request{};
      {
        std::lock_guard lock(mutex);
        copy(request.BrokerID, config.broker);
        copy(request.UserID, config.user);
        copy(request.AppID, config.app_id);
        copy(request.AuthCode, config.auth_code);
      }
      const int code = api->ReqAuthenticate(&request, id);
      erase(request.AuthCode);
      return code;
    }
    case Kind::login: {
      CThostFtdcReqUserLoginField request{};
      {
        std::lock_guard lock(mutex);
        copy(request.BrokerID, config.broker);
        copy(request.UserID, config.user);
        copy(request.Password, config.password);
      }
#ifdef __APPLE__
      // The macOS SDK takes collected client system information; collection
      // is not implemented, so none is sent.
      TThostFtdcClientSystemInfoType system{};
      const int code = api->ReqUserLogin(&request, id, 0, system);
#else
      const int code = api->ReqUserLogin(&request, id);
#endif
      erase(request.Password);
      return code;
    }
    case Kind::confirm: {
      CThostFtdcSettlementInfoConfirmField request{};
      {
        std::lock_guard lock(mutex);
        copy(request.BrokerID, config.broker);
        copy(request.InvestorID, config.user);
      }
      return api->ReqSettlementInfoConfirm(&request, id);
    }
    default:
      throw std::logic_error("not a session request");
    }
  }
  int send_query(const Command& command) {
    const auto kind = command.kind;
    CThostFtdcQryDepthMarketDataField market{};
    CThostFtdcQryInstrumentMarginRateField margin{};
    CThostFtdcQryInstrumentCommissionRateField commission{};
    CThostFtdcQryOrderField orders{};
    CThostFtdcQryTradeField trades{};
    CThostFtdcQryInvestorPositionField positions{};
    CThostFtdcQryTradingAccountField funds{};
    int id;
    {
      std::lock_guard lock(mutex);
      if (closing || command.generation != generation)
        return -1003;
      id = ++request_id;
      query_request_id = id;
      query_kind = kind;
      query_generation = command.generation;
      copy(orders.BrokerID, config.broker);
      copy(orders.InvestorID, config.user);
      copy(trades.BrokerID, config.broker);
      copy(trades.InvestorID, config.user);
      copy(positions.BrokerID, config.broker);
      copy(positions.InvestorID, config.user);
      copy(funds.BrokerID, config.broker);
      copy(funds.InvestorID, config.user);
      copy(margin.BrokerID, config.broker);
      copy(margin.InvestorID, config.user);
      copy(commission.BrokerID, config.broker);
      copy(commission.InvestorID, config.user);
    }
    if (kind == Kind::query_quote) {
      copy(market.ExchangeID, command.instrument.venue);
      copy(market.InstrumentID, command.instrument.symbol);
    }
    if (is_rate_query(kind)) {
      copy(margin.ExchangeID, command.instrument.venue);
      copy(margin.InstrumentID, command.instrument.symbol);
      margin.HedgeFlag = THOST_FTDC_HF_Speculation;
      copy(commission.ExchangeID, command.instrument.venue);
      copy(commission.InstrumentID, command.instrument.symbol);
    }
    switch (kind) {
    case Kind::query_quote:
      return api->ReqQryDepthMarketData(&market, id);
    case Kind::query_margin:
      return api->ReqQryInstrumentMarginRate(&margin, id);
    case Kind::query_commission:
      return api->ReqQryInstrumentCommissionRate(&commission, id);
    case Kind::query_orders:
      return api->ReqQryOrder(&orders, id);
    case Kind::query_trades:
      return api->ReqQryTrade(&trades, id);
    case Kind::query_positions:
      return api->ReqQryInvestorPosition(&positions, id);
    default:
      return api->ReqQryTradingAccount(&funds, id);
    }
  }
  void run(std::stop_token stop) {
    while (!stop.stop_requested()) {
      Command command;
      {
        std::unique_lock lock(mutex);
        wake.wait_for(lock, 50ms);
        if (closing)
          return;
        const auto now = std::chrono::steady_clock::now();
        if (query_outstanding && now - query_started > query_timeout) {
          query_outstanding = false;
          fail(-1002);
        }
        // Orders and session requests first; queries one at a time. A quote
        // gates a waiting order, so it goes before refreshes and rate queries.
        auto next = std::find_if(queue.begin(), queue.end(),
                                 [](const Command& c) { return !is_query(c.kind); });
        if (next == queue.end() && !query_outstanding && now >= next_query_at) {
          next = std::find_if(queue.begin(), queue.end(),
                              [](const Command& c) { return c.kind == Kind::query_quote; });
          if (next == queue.end())
            next = queue.begin();
        }
        if (next == queue.end())
          continue;
        command = std::move(*next);
        queue.erase(next);
        if (command.generation != generation || now >= command.deadline ||
            (command.kind == Kind::query_quote && !quotes.contains(command.serial))) {
          // Prepared for a session that has since disconnected.
          if (command.result)
            command.result->set_value(-1003);
          continue;
        }
        if (is_query(command.kind)) {
          query_outstanding = true;
          query_started = now;
          next_query_at = now + query_spacing;
        }
        if (is_rate_query(command.kind))
          rate_instrument = command.instrument;
        if (command.kind == Kind::query_quote)
          quote_outstanding = command.serial;
      }
      int code = 0;
      if (is_query(command.kind))
        code = send_query(command);
      else if (command.kind == Kind::insert)
        code = api->ReqOrderInsert(&command.order, ++request_id);
      else if (command.kind == Kind::cancel)
        code = api->ReqOrderAction(&command.action, ++request_id);
      else
        code = send(command);
      if (is_query(command.kind) && (code == -2 || code == -3)) {
        std::lock_guard lock(mutex);
        if (closing || command.generation != generation) {
          if (command.result)
            command.result->set_value(-1003);
          continue;
        }
        // Flow control: retry the same query later.
        query_outstanding = false;
        next_query_at = std::chrono::steady_clock::now() + query_spacing;
        queue.push_front(std::move(command));
        continue; // Only complete the promise once, after retries finish.
      }
      if (command.result)
        command.result->set_value(code);
      if (!code)
        continue;
      std::lock_guard lock(mutex);
      if (is_query(command.kind))
        query_outstanding = false;
      if (is_rate_query(command.kind)) {
        // A rate the broker cannot provide does not end the trading session.
        query_outstanding = false;
        rate_answered(command.kind, code);
      } else if (!command.result) {
        if (is_query(command.kind))
          query_outstanding = false;
        fail(code);
      }
    }
  }
  std::future<int> enqueue(Command command) {
    command.result = std::make_shared<std::promise<int>>();
    auto future = command.result->get_future();
    queue.push_back(std::move(command));
    return future;
  }
  void finish_release() {
    if (retiring.joinable())
      retiring.join();
  }
  // Releases the SDK in the background; Release() may block while the SDK
  // retries an unreachable front.
  void close() {
    CThostFtdcTraderApi* old;
    {
      std::lock_guard lock(mutex);
      erase(config.password);
      erase(config.auth_code);
      if (closing)
        return;
      closing = true;
      ++generation;
      old = api;
      for (auto& command : queue)
        if (command.result)
          command.result->set_value(-1003);
      queue.clear();
      query_outstanding = refresh_queued = false;
      abandon_rates();
      state.phase = "disconnected";
      changed();
    }
    wake.notify_all();
    finish_release();
    retiring = std::jthread([this, old, runner = std::move(commands)]() mutable {
      runner.request_stop();
      if (runner.joinable())
        runner.join();
      if (old) {
        old->RegisterSpi(nullptr);
        old->Release();
      }
      std::lock_guard lock(mutex);
      if (api == old)
        api = nullptr;
    });
  }
};

Trader::Trader(const std::filesystem::path& library, const std::filesystem::path& flow)
    : impl_(std::make_unique<Impl>(library, flow)) {}
Trader::~Trader() = default;
PluginDescriptor Trader::descriptor() const {
  return {"asterion.execution.ctp", PluginKind::execution, plugin_contract_version, {}};
}
void Trader::start() {
  if (!impl_->factory)
    throw Error(ErrorCode::unavailable, "CTP trader SDK unavailable");
}
void Trader::stop() noexcept {
  try {
    disconnect();
  } catch (...) {
  }
}
void Trader::connect(TraderConfiguration config, std::map<std::string, std::string> known) {
  if (!std::regex_match(config.front, std::regex("tcp://[A-Za-z0-9.-]+:[0-9]{1,5}")))
    throw std::invalid_argument("invalid CTP front address");
  const auto port = std::stoi(config.front.substr(config.front.rfind(':') + 1));
  if (port < 1 || port > 65535)
    throw std::invalid_argument("invalid CTP front port");
  {
    // Validate every field length before anything is started.
    CThostFtdcReqAuthenticateField authenticate{};
    CThostFtdcReqUserLoginField login{};
    copy(authenticate.BrokerID, config.broker);
    copy(authenticate.UserID, config.user);
    copy(authenticate.AppID, config.app_id);
    copy(authenticate.AuthCode, config.auth_code);
    copy(login.Password, config.password);
    erase(authenticate.AuthCode);
    erase(login.Password);
  }
  impl_->close();
  impl_->finish_release();
  {
    std::lock_guard lock(impl_->mutex);
    impl_->config = std::move(config);
    impl_->known = std::move(known);
    impl_->keys.clear();
    impl_->order_at.clear();
    impl_->exchange.clear();
    impl_->raw_order_ids.clear();
    impl_->trade_ids.clear();
    impl_->positions_in_progress.clear();
    impl_->rates.clear();
    impl_->state = {};
    impl_->state.phase = "connecting";
    impl_->closing = false;
    impl_->changed();
  }
  std::filesystem::create_directories(impl_->flow);
  const auto raw = impl_->flow.u8string();
  std::string path(raw.begin(), raw.end());
  path += "/";
  auto* api = impl_->factory(path.c_str());
  if (!api)
    throw Error(ErrorCode::unavailable, "CTP trader factory failed");
  {
    std::lock_guard lock(impl_->mutex);
    impl_->api = api;
  }
  api->RegisterSpi(impl_.get());
  api->RegisterFront(impl_->config.front.data());
  // QUICK: only reports after login; orders and trades are then queried.
  api->SubscribePrivateTopic(THOST_TERT_QUICK);
  api->SubscribePublicTopic(THOST_TERT_QUICK);
  impl_->commands = std::jthread([this](std::stop_token stop) {
    try {
      impl_->run(stop);
    } catch (...) {
      impl_->callback([&] { impl_->fail(-1000); });
    }
  });
  api->Init();
}
BrokerOrder Trader::submit(const LimitOrder& order, Offset offset,
                           std::uint64_t connection_generation,
                           const std::function<void(const BrokerOrder&)>& journal) {
  order.instrument.validate();
  if (order.id.empty())
    throw std::invalid_argument("order ID is required");
  const int volume = whole_lots(order.quantity);
  const double limit = sdk_price(order.limit_price);
  if (offset == Offset::close &&
      close_policy(order.instrument.venue) == ClosePolicy::explicit_buckets)
    throw std::invalid_argument(order.instrument.venue +
                                " requires close_today or close_yesterday");
  Command command;
  command.kind = Kind::insert;
  BrokerOrder pending;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->closing || impl_->state.phase != "ready")
      throw Error(ErrorCode::unavailable, "CTP trading session is not ready");
    if (connection_generation != impl_->generation)
      throw Error(ErrorCode::unavailable, "CTP trading session changed; the order was not sent");
    if (impl_->keys.contains(order.id))
      throw Error(ErrorCode::conflict, "duplicate order ID");
    const auto ref = std::to_string(++impl_->next_ref);
    command.generation = impl_->generation;
    auto& input = command.order;
    copy(input.BrokerID, impl_->config.broker);
    copy(input.InvestorID, impl_->config.user);
    copy(input.UserID, impl_->config.user);
    copy(input.OrderRef, ref);
    copy(input.ExchangeID, order.instrument.venue);
    copy(input.InstrumentID, order.instrument.symbol);
    input.OrderPriceType = THOST_FTDC_OPT_LimitPrice;
    input.Direction = order.side == Side::buy ? THOST_FTDC_D_Buy : THOST_FTDC_D_Sell;
    input.CombOffsetFlag[0] = offset_flag(offset);
    input.CombHedgeFlag[0] = THOST_FTDC_HF_Speculation;
    input.LimitPrice = limit;
    input.VolumeTotalOriginal = volume;
    input.TimeCondition = THOST_FTDC_TC_GFD;
    input.VolumeCondition = THOST_FTDC_VC_AV;
    input.MinVolume = 1;
    input.ContingentCondition = THOST_FTDC_CC_Immediately;
    input.ForceCloseReason = THOST_FTDC_FCC_NotForceClose;
    pending.order_id = order.id;
    pending.broker_key = impl_->key(impl_->front_id, impl_->session_id, ref);
    pending.instrument = order.instrument;
    pending.side = order.side;
    pending.offset = offset;
    pending.quantity = order.quantity;
    pending.limit_price = order.limit_price;
  }
  // The caller makes the intent durable before anything reaches the broker.
  journal(pending);
  std::future<int> result;
  {
    std::lock_guard lock(impl_->mutex);
    impl_->known[pending.broker_key] = order.id;
    if (impl_->closing || command.generation != impl_->generation) {
      // Journaled but never sent: do not insert a local rejection into the
      // newly synchronized broker snapshot as if the broker had confirmed it.
      pending.status = BrokerOrderStatus::rejected;
      pending.error_code = -1003;
      return pending;
    }
    impl_->keys[order.id] = pending.broker_key;
    impl_->order_at[pending.broker_key] = impl_->state.orders.size();
    impl_->state.orders.push_back(pending);
    impl_->changed();
    result = impl_->enqueue(std::move(command));
  }
  impl_->wake.notify_all();
  const bool answered = result.wait_for(request_timeout) == std::future_status::ready;
  std::lock_guard lock(impl_->mutex);
  const auto found = impl_->order_at.find(pending.broker_key);
  if (found == impl_->order_at.end()) {
    // Reconciliation can replace the cache while this caller waits. Return
    // the request outcome without republishing an unconfirmed cached order.
    if (answered) {
      if (const int code = result.get()) {
        pending.status = BrokerOrderStatus::rejected;
        pending.error_code = code;
      }
    }
    return pending;
  }
  auto& stored = impl_->state.orders[found->second];
  if (answered) {
    if (const int code = result.get()) {
      // Not sent (or refused by the SDK before reaching the broker).
      stored.status = BrokerOrderStatus::rejected;
      stored.error_code = code;
      impl_->changed();
    }
  }
  // Unanswered: status stays submitted until a report or reconciliation.
  return stored;
}
void Trader::cancel(const std::string& order_id) {
  Command command;
  command.kind = Kind::cancel;
  std::future<int> result;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->closing || impl_->state.phase != "ready")
      throw Error(ErrorCode::unavailable, "CTP trading session is not ready");
    const auto key = impl_->keys.find(order_id);
    if (key == impl_->keys.end())
      throw Error(ErrorCode::not_found, "unknown order ID");
    const auto& order = impl_->state.orders[impl_->order_at.at(key->second)];
    if (terminal(order.status))
      throw Error(ErrorCode::conflict, "order is no longer working");
    auto& action = command.action;
    copy(action.BrokerID, impl_->config.broker);
    copy(action.InvestorID, impl_->config.user);
    copy(action.UserID, impl_->config.user);
    action.ActionFlag = THOST_FTDC_AF_Delete;
    copy(action.InstrumentID, order.instrument.symbol);
    copy(action.ExchangeID, order.instrument.venue);
    if (!order.exchange_order_id.empty()) {
      const auto& raw = impl_->raw_order_ids.at(order.broker_key);
      std::copy(raw.begin(), raw.end(), std::begin(action.OrderSysID));
    } else {
      // front:session:ref identifies an order before the exchange accepts it.
      const auto& text = order.broker_key;
      const auto first = text.find(':'), second = text.find(':', first + 1);
      action.FrontID = std::stoi(text.substr(0, first));
      action.SessionID = std::stoi(text.substr(first + 1, second - first - 1));
      copy(action.OrderRef, text.substr(second + 1));
    }
    command.generation = impl_->generation;
    result = impl_->enqueue(std::move(command));
  }
  impl_->wake.notify_all();
  if (result.wait_for(request_timeout) != std::future_status::ready)
    throw Error(ErrorCode::unavailable, "CTP cancel request timed out");
  if (const int code = result.get())
    throw Error(ErrorCode::operation_failed,
                "CTP cancel request failed with code " + std::to_string(code));
}
void Trader::query_costs(const std::vector<std::pair<InstrumentId, std::string>>& contracts) {
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->closing || impl_->state.phase != "ready")
      throw Error(ErrorCode::unavailable, "CTP trading session is not ready");
    for (const auto& [instrument, product] : contracts) {
      instrument.validate();
      if (impl_->rates.contains(instrument))
        continue; // already being queried
      impl_->rates[instrument].product = product;
      auto& costs = impl_->costs_for(instrument);
      costs = BrokerCosts{};
      costs.instrument = instrument;
      for (const auto kind : {Kind::query_margin, Kind::query_commission}) {
        Command command;
        command.kind = kind;
        command.generation = impl_->generation;
        command.instrument = instrument;
        command.product = product;
        impl_->queue.push_back(std::move(command));
      }
    }
    impl_->changed();
  }
  impl_->wake.notify_all();
}
std::optional<BrokerQuote> Trader::quote(const InstrumentId& instrument) {
  instrument.validate();
  std::future<int> sent;
  std::uint64_t serial = 0, generation = 0;
  const auto deadline = std::chrono::steady_clock::now() + query_timeout;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->closing || impl_->state.phase != "ready")
      throw Error(ErrorCode::unavailable, "CTP trading session is not ready");
    Command command;
    command.kind = Kind::query_quote;
    command.generation = impl_->generation;
    command.instrument = instrument;
    command.serial = serial = ++impl_->quote_serial;
    command.deadline = deadline;
    generation = command.generation;
    impl_->quotes.emplace(serial, std::nullopt);
    sent = impl_->enqueue(std::move(command));
  }
  impl_->wake.notify_all();
  // Flow control spaces queries; allow for those already queued.
  const bool accepted = sent.wait_until(deadline) == std::future_status::ready && sent.get() == 0;
  std::unique_lock lock(impl_->mutex);
  if (!accepted) {
    impl_->quotes.erase(serial);
    std::erase_if(impl_->queue, [&](const Command& c) {
      return c.kind == Kind::query_quote && c.serial == serial;
    });
    return std::nullopt;
  }
  // Answered once a response for this serial arrived and its query completed.
  const auto answered = [&] {
    return impl_->closing || impl_->generation != generation || impl_->state.phase != "ready" ||
           (impl_->quotes.contains(serial) &&
            !(impl_->quote_outstanding == serial && impl_->query_outstanding));
  };
  impl_->wake.wait_until(lock, deadline, answered);
  std::optional<BrokerQuote> result;
  if (const auto found = impl_->quotes.find(serial); found != impl_->quotes.end()) {
    if (impl_->generation == generation && impl_->state.phase == "ready" && found->second &&
        found->second->instrument == instrument)
      result = std::move(found->second);
    impl_->quotes.erase(found);
  }
  return result;
}
BrokerSnapshot Trader::snapshot() const {
  std::lock_guard lock(impl_->mutex);
  auto result = impl_->state;
  result.connection_generation = impl_->generation;
  return result;
}
void Trader::disconnect() {
  impl_->close();
}
} // namespace asterion::ctp
