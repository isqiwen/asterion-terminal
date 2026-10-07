// Test-only CTP TraderApi double. Never staged as a product resource.
// Exchange state is process-wide so a reconnect sees earlier orders, trades
// and positions. Behaviour keys:
//   AuthCode "bad-auth" -> authentication error 63; password "bad" -> error 3
//   LimitPrice <= 0 -> CTP rejection 15; instrument "zz..." -> exchange rejection 16
//   volume <= 2 fills at the limit price at once; larger orders rest
//   queries closer than 1s apart return -3 (flow control)
#include <ThostFtdcTraderApi.h>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#define FAKE_EXPORT extern "C" __attribute__((visibility("default")))
namespace {
std::atomic<int> quote_rejection_code{0}, quote_rejection_count{0};
// The exchange refuses every order, as it does outside trading hours.
std::atomic<bool> reject_inserts{false};
std::atomic<int> quote_mode{0}, cancel_return_code{0};
std::atomic<int> catalog_side_effects = 0, stale_batches = 0;
std::atomic<bool> hold_positions = false, hold_trades = false;
std::atomic<int> pending_positions = 0;
std::atomic<bool> hold_release = false, hold_insert_return = false, hold_cancel_return = false;
std::atomic<bool> hold_quote_return = false, quote_return_waiting = false;
std::atomic<int> insert_return_code{0};
std::atomic<bool> hold_insert_errors{false};
std::atomic<int> sdk_creations{0}, sdk_releases{0}, sdk_thread_mismatches{0};
template <std::size_t N> void put(char (&dest)[N], const std::string& value) {
  std::memset(dest, 0, N);
  std::memcpy(dest, value.data(), std::min(value.size(), N - 1));
}
struct Position {
  int today = 0, yesterday = 0;
};
struct Exchange {
  std::mutex mutex;
  std::vector<CThostFtdcOrderField> orders;
  std::vector<CThostFtdcTradeField> trades;
  std::map<std::pair<std::string, char>, Position> positions;
  std::string trading_day = "20260928";
  int sessions = 0, sys_ids = 0, trade_ids = 0, query_rejections = 0;
  std::chrono::steady_clock::time_point last_query{};
};
Exchange& exchange() {
  static Exchange instance;
  return instance;
}
class Fake;
std::mutex live_mutex;
std::vector<Fake*> live;
class Fake final : public CThostFtdcTraderApi {
  const std::thread::id sdk_thread = std::this_thread::get_id();
  void check_sdk_thread() const {
    if (std::this_thread::get_id() != sdk_thread)
      ++sdk_thread_mismatches;
  }
  const std::filesystem::path flow;
  std::string catalog_mode;
  CThostFtdcTraderSpi* spi = nullptr;
  std::mutex mutex;
  std::condition_variable wake;
  std::deque<std::function<void(CThostFtdcTraderSpi*)>> events;
  std::deque<std::function<void(CThostFtdcTraderSpi*)>> deferred_positions, deferred_trades;
  std::deque<std::function<void(CThostFtdcTraderSpi*)>> deferred_insert_errors;
  std::jthread worker;
  int front = 1, session = 0;
  std::atomic<int> first_funds_request{0};
  void emit(std::function<void(CThostFtdcTraderSpi*)> event) {
    {
      std::lock_guard lock(mutex);
      events.push_back(std::move(event));
    }
    wake.notify_all();
  }
  void defer_or_emit(std::function<void(CThostFtdcTraderSpi*)> event, bool position) {
    {
      std::lock_guard lock(mutex);
      if (position ? hold_positions.load() : hold_trades.load()) {
        (position ? deferred_positions : deferred_trades).push_back(std::move(event));
        if (position)
          ++pending_positions;
      } else
        events.push_back(std::move(event));
    }
    wake.notify_all();
  }
  static CThostFtdcRspInfoField info(int code) {
    CThostFtdcRspInfoField value{};
    value.ErrorID = code;
    return value;
  }
  bool throttled() {
    auto& x = exchange();
    std::lock_guard lock(x.mutex);
    const auto now = std::chrono::steady_clock::now();
    if (now - x.last_query < std::chrono::seconds(1)) {
      ++x.query_rejections;
      return true;
    }
    x.last_query = now;
    return false;
  }
  void trade(CThostFtdcOrderField& order, int volume) {
    auto& x = exchange();
    CThostFtdcTradeField t{};
    put(t.BrokerID, order.BrokerID);
    put(t.InvestorID, order.InvestorID);
    put(t.ExchangeID, order.ExchangeID);
    put(t.InstrumentID, order.InstrumentID);
    put(t.OrderRef, order.OrderRef);
    put(t.OrderSysID, order.OrderSysID);
    char id[21];
    std::snprintf(id, sizeof id, "%12d", ++x.trade_ids);
    put(t.TradeID, id);
    t.Direction = order.Direction;
    t.OffsetFlag = order.CombOffsetFlag[0];
    t.Price = order.LimitPrice;
    t.Volume = volume;
    put(t.TradingDay, x.trading_day);
    put(t.TradeDate, x.trading_day);
    put(t.TradeTime, "09:30:00");
    x.trades.push_back(t);
    order.VolumeTraded += volume;
    order.VolumeTotal -= volume;
    order.OrderStatus =
        order.VolumeTotal ? THOST_FTDC_OST_PartTradedQueueing : THOST_FTDC_OST_AllTraded;
    const bool open = t.OffsetFlag == THOST_FTDC_OF_Open;
    // Opening a buy adds long; a closing buy reduces short, and vice versa.
    const char side =
        (t.Direction == THOST_FTDC_D_Buy) == open ? THOST_FTDC_PD_Long : THOST_FTDC_PD_Short;
    auto& position = x.positions[{std::string(t.InstrumentID) + "@" + t.ExchangeID, side}];
    if (open)
      position.today += volume;
    else if (t.OffsetFlag == THOST_FTDC_OF_CloseYesterday || position.today < volume)
      position.yesterday -= volume;
    else
      position.today -= volume;
    auto copy = t;
    auto state = order;
    emit([state](CThostFtdcTraderSpi* s) mutable { s->OnRtnOrder(&state); });
    defer_or_emit([copy](CThostFtdcTraderSpi* s) mutable { s->OnRtnTrade(&copy); }, false);
  }

public:
  void release_deferred(bool position) {
    {
      std::lock_guard lock(mutex);
      auto& deferred = position ? deferred_positions : deferred_trades;
      if (position)
        pending_positions -= static_cast<int>(deferred.size());
      while (!deferred.empty()) {
        events.push_back(std::move(deferred.front()));
        deferred.pop_front();
      }
    }
    wake.notify_all();
  }
  void fill(int index, int volume) {
    auto& x = exchange();
    std::lock_guard lock(x.mutex);
    auto& order = x.orders.at(static_cast<std::size_t>(index));
    trade(order, std::min(volume, order.VolumeTotal));
  }
  void replay_order(int index, int filled) {
    auto& x = exchange();
    std::lock_guard lock(x.mutex);
    auto order = x.orders.at(static_cast<std::size_t>(index));
    order.VolumeTraded = filled;
    order.OrderStatus = THOST_FTDC_OST_NoTradeQueueing;
    emit([order](CThostFtdcTraderSpi* s) mutable { s->OnRtnOrder(&order); });
  }
  void release_insert_errors() {
    {
      std::lock_guard lock(mutex);
      while (!deferred_insert_errors.empty()) {
        events.push_back(std::move(deferred_insert_errors.front()));
        deferred_insert_errors.pop_front();
      }
    }
    wake.notify_all();
  }
  void report_burst(int count) {
    auto& x = exchange();
    std::lock_guard lock(x.mutex);
    auto order = x.orders.front();
    emit([order, count](CThostFtdcTraderSpi* s) mutable {
      for (int i = 0; i < count; ++i)
        s->OnRtnOrder(&order);
    });
  }
  explicit Fake(const char* path) : flow(path) {
    ++sdk_creations;
    std::lock_guard lock(live_mutex);
    live.push_back(this);
  }
  void Release() override {
    check_sdk_thread();
    ++sdk_releases;
    hold_release.wait(true);
    {
      std::lock_guard lock(live_mutex);
      std::erase(live, this);
    }
    worker.request_stop();
    wake.notify_all();
    if (worker.joinable())
      worker.join();
    delete this;
  }
  void Init() override {
    check_sdk_thread();
    worker = std::jthread([this](std::stop_token stop) {
      while (!stop.stop_requested()) {
        std::function<void(CThostFtdcTraderSpi*)> event;
        CThostFtdcTraderSpi* target;
        {
          std::unique_lock lock(mutex);
          wake.wait_for(lock, std::chrono::milliseconds(20));
          if (events.empty() || !spi)
            continue;
          event = std::move(events.front());
          events.pop_front();
          target = spi;
        }
        event(target);
      }
    });
    emit([](CThostFtdcTraderSpi* s) { s->OnFrontConnected(); });
  }
  // Test control: drop and restore the connection.
  void reconnect() {
    emit([](CThostFtdcTraderSpi* s) { s->OnFrontDisconnected(0x1001); });
    emit([](CThostFtdcTraderSpi* s) { s->OnFrontConnected(); });
  }
  void stale_queries() {
    const int id = first_funds_request.load();
    emit([id](CThostFtdcTraderSpi* s) {
      CThostFtdcTradingAccountField funds{};
      funds.Balance = funds.Available = 42;
      s->OnRspQryTradingAccount(&funds, nullptr, id, true);
      CThostFtdcInvestorPositionField position{};
      put(position.ExchangeID, "SHFE");
      put(position.InstrumentID, "rb2610");
      position.PosiDirection = THOST_FTDC_PD_Long;
      position.Position = position.TodayPosition = 999;
      s->OnRspQryInvestorPosition(&position, nullptr, id, true);
      s->OnRspQryInstrumentMarginRate(nullptr, nullptr, id, true);
      s->OnRspQryInstrumentCommissionRate(nullptr, nullptr, id, true);
      ++stale_batches;
    });
  }
  int Join() override { return 0; }
  const char* GetTradingDay() override {
    thread_local std::string day;
    auto& x = exchange();
    std::lock_guard lock(x.mutex);
    day = x.trading_day;
    return day.c_str();
  }
  void RegisterFront(char*) override { check_sdk_thread(); }
  void RegisterSpi(CThostFtdcTraderSpi* value) override {
    check_sdk_thread();
    std::lock_guard lock(mutex);
    spi = value;
  }
  void SubscribePrivateTopic(THOST_TE_RESUME_TYPE) override { check_sdk_thread(); }
  void SubscribePublicTopic(THOST_TE_RESUME_TYPE) override { check_sdk_thread(); }
  int ReqAuthenticate(CThostFtdcReqAuthenticateField* request, int id) override {
    check_sdk_thread();
    const int code = std::string(request->AuthCode) == "bad-auth" ? 63 : 0;
    emit([code, id](CThostFtdcTraderSpi* s) {
      auto rsp = info(code);
      CThostFtdcRspAuthenticateField field{};
      s->OnRspAuthenticate(&field, &rsp, id, true);
    });
    return 0;
  }
  int ReqUserLogin(CThostFtdcReqUserLoginField* request, int id
#ifdef __APPLE__
                   ,
                   TThostFtdcSystemInfoLenType, TThostFtdcClientSystemInfoType
#endif
                   ) override {
    check_sdk_thread();
    catalog_mode = request->UserID;
    const int code = std::string(request->Password) == "bad" ? 3 : 0;
    CThostFtdcRspUserLoginField login{};
    {
      auto& x = exchange();
      std::lock_guard lock(x.mutex);
      session = ++x.sessions;
      put(login.TradingDay, x.trading_day);
    }
    login.FrontID = front;
    login.SessionID = session;
    put(login.MaxOrderRef, "        0");
    emit([code, id, login](CThostFtdcTraderSpi* s) mutable {
      auto rsp = info(code);
      s->OnRspUserLogin(&login, &rsp, id, true);
    });
    return 0;
  }
  int ReqSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField*, int id) override {
    check_sdk_thread();
    ++catalog_side_effects;
    emit([id](CThostFtdcTraderSpi* s) {
      auto rsp = info(0);
      CThostFtdcSettlementInfoConfirmField field{};
      s->OnRspSettlementInfoConfirm(&field, &rsp, id, true);
    });
    return 0;
  }
  int ReqQryOrder(CThostFtdcQryOrderField*, int id) override {
    check_sdk_thread();
    if (throttled())
      return -3;
    std::vector<CThostFtdcOrderField> rows;
    {
      auto& x = exchange();
      std::lock_guard lock(x.mutex);
      rows = x.orders;
    }
    emit([rows, id](CThostFtdcTraderSpi* s) mutable {
      if (rows.empty())
        s->OnRspQryOrder(nullptr, nullptr, id, true);
      for (std::size_t i = 0; i < rows.size(); ++i)
        s->OnRspQryOrder(&rows[i], nullptr, id, i + 1 == rows.size());
    });
    return 0;
  }
  int ReqQryTrade(CThostFtdcQryTradeField*, int id) override {
    check_sdk_thread();
    if (throttled())
      return -3;
    std::vector<CThostFtdcTradeField> rows;
    {
      auto& x = exchange();
      std::lock_guard lock(x.mutex);
      rows = x.trades;
    }
    emit([rows, id](CThostFtdcTraderSpi* s) mutable {
      if (rows.empty())
        s->OnRspQryTrade(nullptr, nullptr, id, true);
      for (std::size_t i = 0; i < rows.size(); ++i)
        s->OnRspQryTrade(&rows[i], nullptr, id, i + 1 == rows.size());
    });
    return 0;
  }
  int ReqQryInvestorPosition(CThostFtdcQryInvestorPositionField*, int id) override {
    check_sdk_thread();
    if (throttled())
      return -3;
    std::vector<CThostFtdcInvestorPositionField> rows;
    {
      auto& x = exchange();
      std::lock_guard lock(x.mutex);
      for (const auto& [key, position] : x.positions) {
        const auto at = key.first.find('@');
        const auto instrument = key.first.substr(0, at), venue = key.first.substr(at + 1);
        // SHFE/INE split today and history rows; others report one row.
        const bool split = venue == "SHFE" || venue == "INE";
        CThostFtdcInvestorPositionField row{};
        put(row.InstrumentID, instrument);
        put(row.ExchangeID, venue);
        row.PosiDirection = key.second;
        row.PositionDate = THOST_FTDC_PSD_Today;
        row.Position = split ? position.today : position.today + position.yesterday;
        row.TodayPosition = position.today;
        rows.push_back(row);
        if (split && position.yesterday) {
          row.PositionDate = THOST_FTDC_PSD_History;
          row.Position = row.YdPosition = position.yesterday;
          row.TodayPosition = 0;
          rows.push_back(row);
        }
      }
    }
    defer_or_emit(
        [rows, id](CThostFtdcTraderSpi* s) mutable {
          if (rows.empty())
            s->OnRspQryInvestorPosition(nullptr, nullptr, id, true);
          for (std::size_t i = 0; i < rows.size(); ++i)
            s->OnRspQryInvestorPosition(&rows[i], nullptr, id, i + 1 == rows.size());
        },
        true);
    return 0;
  }
  int ReqQryTradingAccount(CThostFtdcQryTradingAccountField*, int id) override {
    check_sdk_thread();
    int empty = 0;
    first_funds_request.compare_exchange_strong(empty, id);
    if (throttled())
      return -3;
    emit([id](CThostFtdcTraderSpi* s) {
      CThostFtdcTradingAccountField account{};
      account.Balance = 1000000.005;
      account.Available = 950000.0;
      account.CurrMargin = 50000.0;
      account.Commission = 12.34;
      s->OnRspQryTradingAccount(&account, nullptr, id, true);
    });
    return 0;
  }
  int ReqOrderInsert(CThostFtdcInputOrderField* input, int id) override {
    check_sdk_thread();
    ++catalog_side_effects;
    if (const int code = cancel_return_code.load())
      return code;
    auto request = *input;
    if (request.LimitPrice <= 0) {
      emit([request, id](CThostFtdcTraderSpi* s) mutable {
        auto rsp = info(15);
        s->OnRspOrderInsert(&request, &rsp, id, true);
      });
      return 0;
    }
    if (reject_inserts || std::string(request.InstrumentID).starts_with("zz")) {
      if (hold_insert_errors) {
        std::lock_guard lock(mutex);
        deferred_insert_errors.push_back([request, id](CThostFtdcTraderSpi* s) mutable {
          auto rsp = info(16);
          s->OnRspOrderInsert(nullptr, &rsp, id, true);
          s->OnErrRtnOrderInsert(&request, &rsp);
        });
        return 0;
      }
      emit([request](CThostFtdcTraderSpi* s) mutable {
        auto rsp = info(16);
        s->OnErrRtnOrderInsert(&request, &rsp);
      });
      return 0;
    }
    auto& x = exchange();
    std::lock_guard lock(x.mutex);
    CThostFtdcOrderField order{};
    put(order.BrokerID, request.BrokerID);
    put(order.InvestorID, request.InvestorID);
    put(order.OrderRef, request.OrderRef);
    put(order.ExchangeID, request.ExchangeID);
    put(order.InstrumentID, request.InstrumentID);
    order.Direction = request.Direction;
    order.CombOffsetFlag[0] = request.CombOffsetFlag[0];
    order.LimitPrice = request.LimitPrice;
    order.VolumeTotalOriginal = order.VolumeTotal = request.VolumeTotalOriginal;
    put(order.TradingDay, x.trading_day);
    order.FrontID = front;
    order.SessionID = session;
    order.OrderSubmitStatus = THOST_FTDC_OSS_InsertSubmitted;
    order.OrderStatus = THOST_FTDC_OST_Unknown;
    auto submitted = order;
    char sys[21];
    std::snprintf(sys, sizeof sys, "%12d", ++x.sys_ids);
    put(order.OrderSysID, sys);
    order.OrderSubmitStatus = THOST_FTDC_OSS_Accepted;
    order.OrderStatus = THOST_FTDC_OST_NoTradeQueueing;
    auto accepted = order;
    emit([submitted, accepted](CThostFtdcTraderSpi* s) mutable {
      s->OnRtnOrder(&submitted);
      s->OnRtnOrder(&accepted);
    });
    if (order.VolumeTotalOriginal <= 2)
      trade(order, order.VolumeTotalOriginal);
    x.orders.push_back(order);
    hold_insert_return.wait(true);
    return insert_return_code;
  }
  int ReqOrderAction(CThostFtdcInputOrderActionField* input, int id) override {
    check_sdk_thread();
    ++catalog_side_effects;
    if (const int code = cancel_return_code.load())
      return code;
    auto request = *input;
    auto& x = exchange();
    std::lock_guard lock(x.mutex);
    for (auto& order : x.orders) {
      // Exact SDK bytes matter: normalizing here would hide broken cancellations.
      const bool by_sys = request.OrderSysID[0] &&
                          std::string(order.OrderSysID) == request.OrderSysID &&
                          std::string(order.ExchangeID) == request.ExchangeID;
      const bool by_ref = !request.OrderSysID[0] && order.FrontID == request.FrontID &&
                          order.SessionID == request.SessionID &&
                          std::string(order.OrderRef) == request.OrderRef;
      if (!by_sys && !by_ref)
        continue;
      if (order.OrderStatus != THOST_FTDC_OST_NoTradeQueueing &&
          order.OrderStatus != THOST_FTDC_OST_PartTradedQueueing)
        break;
      order.OrderStatus = THOST_FTDC_OST_Canceled;
      auto state = order;
      emit([state](CThostFtdcTraderSpi* s) mutable { s->OnRtnOrder(&state); });
      hold_cancel_return.wait(true);
      return 0;
    }
    emit([request, id](CThostFtdcTraderSpi* s) mutable {
      auto rsp = info(26);
      s->OnRspOrderAction(&request, &rsp, id, true);
    });
    return 0;
  }
  void GetFrontInfo(CThostFtdcFrontInfoField*) override {}
  void RegisterNameServer(char*) override {}
  void RegisterFensUserInfo(CThostFtdcFensUserInfoField*) override {}
  int RegisterUserSystemInfo(CThostFtdcUserSystemInfoField*) override { return -1; }
  int SubmitUserSystemInfo(CThostFtdcUserSystemInfoField*) override { return -1; }
  int ReqUserLogout(CThostFtdcUserLogoutField*, int) override { return -1; }
  int ReqUserPasswordUpdate(CThostFtdcUserPasswordUpdateField*, int) override { return -1; }
  int ReqTradingAccountPasswordUpdate(CThostFtdcTradingAccountPasswordUpdateField*, int) override {
    return -1;
  }
  int ReqUserAuthMethod(CThostFtdcReqUserAuthMethodField*, int) override { return -1; }
  int ReqGenUserCaptcha(CThostFtdcReqGenUserCaptchaField*, int) override { return -1; }
  int ReqGenUserText(CThostFtdcReqGenUserTextField*, int) override { return -1; }
  int ReqUserLoginWithCaptcha(CThostFtdcReqUserLoginWithCaptchaField*, int) override { return -1; }
  int ReqUserLoginWithText(CThostFtdcReqUserLoginWithTextField*, int) override { return -1; }
  int ReqUserLoginWithOTP(CThostFtdcReqUserLoginWithOTPField*, int) override { return -1; }
  int ReqParkedOrderInsert(CThostFtdcParkedOrderField*, int) override { return -1; }
  int ReqParkedOrderAction(CThostFtdcParkedOrderActionField*, int) override { return -1; }
  int ReqQryMaxOrderVolume(CThostFtdcQryMaxOrderVolumeField*, int) override { return -1; }
  int ReqRemoveParkedOrder(CThostFtdcRemoveParkedOrderField*, int) override { return -1; }
  int ReqRemoveParkedOrderAction(CThostFtdcRemoveParkedOrderActionField*, int) override {
    return -1;
  }
  int ReqExecOrderInsert(CThostFtdcInputExecOrderField*, int) override { return -1; }
  int ReqExecOrderAction(CThostFtdcInputExecOrderActionField*, int) override { return -1; }
  int ReqForQuoteInsert(CThostFtdcInputForQuoteField*, int) override { return -1; }
  int ReqQuoteInsert(CThostFtdcInputQuoteField*, int) override { return -1; }
  int ReqQuoteAction(CThostFtdcInputQuoteActionField*, int) override { return -1; }
  int ReqBatchOrderAction(CThostFtdcInputBatchOrderActionField*, int) override { return -1; }
  int ReqOptionSelfCloseInsert(CThostFtdcInputOptionSelfCloseField*, int) override { return -1; }
  int ReqOptionSelfCloseAction(CThostFtdcInputOptionSelfCloseActionField*, int) override {
    return -1;
  }
  int ReqCombActionInsert(CThostFtdcInputCombActionField*, int) override { return -1; }
  int ReqQryInvestor(CThostFtdcQryInvestorField*, int) override { return -1; }
  int ReqQryTradingCode(CThostFtdcQryTradingCodeField*, int) override { return -1; }
  // Margin per contract; commission per product, as many brokers report it.
  int ReqQryInstrumentMarginRate(CThostFtdcQryInstrumentMarginRateField* request, int id) override {
    check_sdk_thread();
    if (throttled())
      return -3;
    const std::string instrument = request->InstrumentID;
    emit([instrument, id](CThostFtdcTraderSpi* s) {
      if (instrument.starts_with("zz"))
        return s->OnRspQryInstrumentMarginRate(nullptr, nullptr, id, true);
      CThostFtdcInstrumentMarginRateField row{};
      put(row.InstrumentID, instrument);
      row.LongMarginRatioByMoney = 0.1;
      row.ShortMarginRatioByMoney = 0.12;
      row.LongMarginRatioByVolume = 0;
      row.ShortMarginRatioByVolume = 0;
      s->OnRspQryInstrumentMarginRate(&row, nullptr, id, true);
    });
    return 0;
  }
  int ReqQryInstrumentCommissionRate(CThostFtdcQryInstrumentCommissionRateField* request,
                                     int id) override {
    check_sdk_thread();
    if (throttled())
      return -3;
    std::string product = request->InstrumentID;
    while (!product.empty() && std::isdigit(static_cast<unsigned char>(product.back())))
      product.pop_back();
    emit([product, id](CThostFtdcTraderSpi* s) {
      CThostFtdcInstrumentCommissionRateField row{};
      put(row.InstrumentID, product);
      row.OpenRatioByMoney = 0.0001;
      row.OpenRatioByVolume = 0;
      row.CloseRatioByMoney = 0.0001;
      row.CloseRatioByVolume = 0;
      row.CloseTodayRatioByMoney = 0.0003;
      row.CloseTodayRatioByVolume = 1.5;
      s->OnRspQryInstrumentCommissionRate(&row, nullptr, id, true);
    });
    return 0;
  }
  int ReqQryExchange(CThostFtdcQryExchangeField*, int) override { return -1; }
  int ReqQryProduct(CThostFtdcQryProductField*, int) override { return -1; }
  int ReqQryInstrument(CThostFtdcQryInstrumentField*, int id) override {
    const auto mode = catalog_mode;
    check_sdk_thread();
    if (mode == "catalog-stall") {
      std::ofstream(flow / "catalog-query-started").put('1');
      return 0;
    }
    emit([id, mode](CThostFtdcTraderSpi* spi) {
      auto status = info(0);
      if (mode != "catalog-empty") {
        CThostFtdcInstrumentField item{};
        put(item.InstrumentName, "\xc2\xdd\xce\xc6\xb8\xd6");
        put(item.ExchangeID, "SHFE");
        put(item.InstrumentID, "rb2610");
        put(item.ProductID, "rb");
        put(item.ExpireDate, "20261015");
        item.DeliveryYear = 2026;
        item.DeliveryMonth = 10;
        item.ProductClass = THOST_FTDC_PC_Futures;
        item.IsTrading = 1;
        item.VolumeMultiple = 10;
        item.PriceTick = mode == "catalog-invalid" ? 0 : 0.5;
        spi->OnRspQryInstrument(&item, &status, id, false);
        if (mode == "catalog-dup")
          spi->OnRspQryInstrument(&item, &status, id, false);
        put(item.InstrumentID, "rb2609");
        item.IsTrading = 0;
        spi->OnRspQryInstrument(&item, &status, id, false);
        put(item.InstrumentID, "rb2610C3500");
        item.IsTrading = 1;
        item.ProductClass = THOST_FTDC_PC_Options;
        spi->OnRspQryInstrument(&item, &status, id, false);
        if (mode == "catalog-stream") {
          for (int i = 0; i < 16; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            spi->OnRspQryInstrument(&item, &status, id, false);
          }
        }
      }
      if (mode == "catalog-partial")
        return;
      if (mode == "catalog-error")
        status = info(7);
      spi->OnRspQryInstrument(nullptr, &status, id, true);
    });
    return 0;
  }
  // rb2610 trades at 3500 within 3300..3700; "zz..." has no market.
  int ReqQryDepthMarketData(CThostFtdcQryDepthMarketDataField* request, int id) override {
    check_sdk_thread();
    if (quote_rejection_count.load() > 0 && quote_rejection_count.fetch_sub(1) > 0) {
      auto& x = exchange();
      std::lock_guard lock(x.mutex);
      ++x.query_rejections;
      return quote_rejection_code.load();
    }
    if (throttled())
      return -3;
    const std::string venue = request->ExchangeID, instrument = request->InstrumentID;
    const std::string day = GetTradingDay();
    const int mode = quote_mode.load();
    emit([venue, instrument, id, day, mode](CThostFtdcTraderSpi* s) {
      if (instrument.starts_with("zz"))
        return s->OnRspQryDepthMarketData(nullptr, nullptr, id, true);
      CThostFtdcDepthMarketDataField row{};
      put(row.ExchangeID, venue);
      put(row.InstrumentID, mode == 4 ? "rb2612" : instrument);
      put(row.TradingDay, mode == 3 ? "20260925" : day);
      put(row.UpdateTime, "10:15:00");
      row.LastPrice = instrument == "rb2611" ? 1.7976931348623157e308 : 3500;
      row.PreSettlementPrice = 3490;
      row.UpperLimitPrice = 3700;
      row.LowerLimitPrice = 3300;
      s->OnRspQryDepthMarketData(&row, nullptr, id, mode == 0 || mode == 3 || mode == 4);
      if (mode == 1) {
        auto failure = info(7);
        s->OnRspQryDepthMarketData(nullptr, &failure, id, true);
      } else if (mode == 5)
        s->OnRspQryDepthMarketData(nullptr, nullptr, id, true);
    });
    quote_return_waiting = hold_quote_return.load();
    hold_quote_return.wait(true);
    quote_return_waiting = false;
    return 0;
  }
  int ReqQryTraderOffer(CThostFtdcQryTraderOfferField*, int) override { return -1; }
  int ReqQrySettlementInfo(CThostFtdcQrySettlementInfoField*, int) override { return -1; }
  int ReqQryTransferBank(CThostFtdcQryTransferBankField*, int) override { return -1; }
  int ReqQryInvestorPositionDetail(CThostFtdcQryInvestorPositionDetailField*, int) override {
    return -1;
  }
  int ReqQryNotice(CThostFtdcQryNoticeField*, int) override { return -1; }
  int ReqQrySettlementInfoConfirm(CThostFtdcQrySettlementInfoConfirmField*, int) override {
    return -1;
  }
  int ReqQryInvestorPositionCombineDetail(CThostFtdcQryInvestorPositionCombineDetailField*,
                                          int) override {
    return -1;
  }
  int ReqQryCFMMCTradingAccountKey(CThostFtdcQryCFMMCTradingAccountKeyField*, int) override {
    return -1;
  }
  int ReqQryEWarrantOffset(CThostFtdcQryEWarrantOffsetField*, int) override { return -1; }
  int ReqQryInvestorProductGroupMargin(CThostFtdcQryInvestorProductGroupMarginField*,
                                       int) override {
    return -1;
  }
  int ReqQryExchangeMarginRate(CThostFtdcQryExchangeMarginRateField*, int) override { return -1; }
  int ReqQryExchangeMarginRateAdjust(CThostFtdcQryExchangeMarginRateAdjustField*, int) override {
    return -1;
  }
  int ReqQryExchangeRate(CThostFtdcQryExchangeRateField*, int) override { return -1; }
  int ReqQrySecAgentACIDMap(CThostFtdcQrySecAgentACIDMapField*, int) override { return -1; }
  int ReqQryProductExchRate(CThostFtdcQryProductExchRateField*, int) override { return -1; }
  int ReqQryProductGroup(CThostFtdcQryProductGroupField*, int) override { return -1; }
  int ReqQryMMInstrumentCommissionRate(CThostFtdcQryMMInstrumentCommissionRateField*,
                                       int) override {
    return -1;
  }
  int ReqQryMMOptionInstrCommRate(CThostFtdcQryMMOptionInstrCommRateField*, int) override {
    return -1;
  }
  int ReqQryInstrumentOrderCommRate(CThostFtdcQryInstrumentOrderCommRateField*, int) override {
    return -1;
  }
  int ReqQrySecAgentTradingAccount(CThostFtdcQryTradingAccountField*, int) override { return -1; }
  int ReqQrySecAgentCheckMode(CThostFtdcQrySecAgentCheckModeField*, int) override { return -1; }
  int ReqQrySecAgentTradeInfo(CThostFtdcQrySecAgentTradeInfoField*, int) override { return -1; }
  int ReqQryOptionInstrTradeCost(CThostFtdcQryOptionInstrTradeCostField*, int) override {
    return -1;
  }
  int ReqQryOptionInstrCommRate(CThostFtdcQryOptionInstrCommRateField*, int) override { return -1; }
  int ReqQryExecOrder(CThostFtdcQryExecOrderField*, int) override { return -1; }
  int ReqQryForQuote(CThostFtdcQryForQuoteField*, int) override { return -1; }
  int ReqQryQuote(CThostFtdcQryQuoteField*, int) override { return -1; }
  int ReqQryOptionSelfClose(CThostFtdcQryOptionSelfCloseField*, int) override { return -1; }
  int ReqQryInvestUnit(CThostFtdcQryInvestUnitField*, int) override { return -1; }
  int ReqQryCombInstrumentGuard(CThostFtdcQryCombInstrumentGuardField*, int) override { return -1; }
  int ReqQryCombAction(CThostFtdcQryCombActionField*, int) override { return -1; }
  int ReqQryTransferSerial(CThostFtdcQryTransferSerialField*, int) override { return -1; }
  int ReqQryAccountregister(CThostFtdcQryAccountregisterField*, int) override { return -1; }
  int ReqQryContractBank(CThostFtdcQryContractBankField*, int) override { return -1; }
  int ReqQryParkedOrder(CThostFtdcQryParkedOrderField*, int) override { return -1; }
  int ReqQryParkedOrderAction(CThostFtdcQryParkedOrderActionField*, int) override { return -1; }
  int ReqQryTradingNotice(CThostFtdcQryTradingNoticeField*, int) override { return -1; }
  int ReqQryBrokerTradingParams(CThostFtdcQryBrokerTradingParamsField*, int) override { return -1; }
  int ReqQryBrokerTradingAlgos(CThostFtdcQryBrokerTradingAlgosField*, int) override { return -1; }
  int ReqQueryCFMMCTradingAccountToken(CThostFtdcQueryCFMMCTradingAccountTokenField*,
                                       int) override {
    return -1;
  }
  int ReqFromBankToFutureByFuture(CThostFtdcReqTransferField*, int) override { return -1; }
  int ReqFromFutureToBankByFuture(CThostFtdcReqTransferField*, int) override { return -1; }
  int ReqQueryBankAccountMoneyByFuture(CThostFtdcReqQueryAccountField*, int) override { return -1; }
  int ReqQryClassifiedInstrument(CThostFtdcQryClassifiedInstrumentField*, int) override {
    return -1;
  }
  int ReqQryCombPromotionParam(CThostFtdcQryCombPromotionParamField*, int) override { return -1; }
  int ReqQryRiskSettleInvstPosition(CThostFtdcQryRiskSettleInvstPositionField*, int) override {
    return -1;
  }
  int ReqQryRiskSettleProductStatus(CThostFtdcQryRiskSettleProductStatusField*, int) override {
    return -1;
  }
  int ReqQrySPBMFutureParameter(CThostFtdcQrySPBMFutureParameterField*, int) override { return -1; }
  int ReqQrySPBMOptionParameter(CThostFtdcQrySPBMOptionParameterField*, int) override { return -1; }
  int ReqQrySPBMIntraParameter(CThostFtdcQrySPBMIntraParameterField*, int) override { return -1; }
  int ReqQrySPBMInterParameter(CThostFtdcQrySPBMInterParameterField*, int) override { return -1; }
  int ReqQrySPBMPortfDefinition(CThostFtdcQrySPBMPortfDefinitionField*, int) override { return -1; }
  int ReqQrySPBMInvestorPortfDef(CThostFtdcQrySPBMInvestorPortfDefField*, int) override {
    return -1;
  }
  int ReqQryInvestorPortfMarginRatio(CThostFtdcQryInvestorPortfMarginRatioField*, int) override {
    return -1;
  }
  int ReqQryInvestorProdSPBMDetail(CThostFtdcQryInvestorProdSPBMDetailField*, int) override {
    return -1;
  }
  int ReqQryInvestorCommoditySPMMMargin(CThostFtdcQryInvestorCommoditySPMMMarginField*,
                                        int) override {
    return -1;
  }
  int ReqQryInvestorCommodityGroupSPMMMargin(CThostFtdcQryInvestorCommodityGroupSPMMMarginField*,
                                             int) override {
    return -1;
  }
  int ReqQrySPMMInstParam(CThostFtdcQrySPMMInstParamField*, int) override { return -1; }
  int ReqQrySPMMProductParam(CThostFtdcQrySPMMProductParamField*, int) override { return -1; }
  int ReqQrySPBMAddOnInterParameter(CThostFtdcQrySPBMAddOnInterParameterField*, int) override {
    return -1;
  }
  int ReqQryRCAMSCombProductInfo(CThostFtdcQryRCAMSCombProductInfoField*, int) override {
    return -1;
  }
  int ReqQryRCAMSInstrParameter(CThostFtdcQryRCAMSInstrParameterField*, int) override { return -1; }
  int ReqQryRCAMSIntraParameter(CThostFtdcQryRCAMSIntraParameterField*, int) override { return -1; }
  int ReqQryRCAMSInterParameter(CThostFtdcQryRCAMSInterParameterField*, int) override { return -1; }
  int ReqQryRCAMSShortOptAdjustParam(CThostFtdcQryRCAMSShortOptAdjustParamField*, int) override {
    return -1;
  }
  int ReqQryRCAMSInvestorCombPosition(CThostFtdcQryRCAMSInvestorCombPositionField*, int) override {
    return -1;
  }
  int ReqQryInvestorProdRCAMSMargin(CThostFtdcQryInvestorProdRCAMSMarginField*, int) override {
    return -1;
  }
  int ReqQryRULEInstrParameter(CThostFtdcQryRULEInstrParameterField*, int) override { return -1; }
  int ReqQryRULEIntraParameter(CThostFtdcQryRULEIntraParameterField*, int) override { return -1; }
  int ReqQryRULEInterParameter(CThostFtdcQryRULEInterParameterField*, int) override { return -1; }
  int ReqQryInvestorProdRULEMargin(CThostFtdcQryInvestorProdRULEMarginField*, int) override {
    return -1;
  }
  int ReqQryInvestorPortfSetting(CThostFtdcQryInvestorPortfSettingField*, int) override {
    return -1;
  }
};
} // namespace
CThostFtdcTraderApi* CThostFtdcTraderApi::CreateFtdcTraderApi(const char* flow) {
  return new Fake(flow);
}
const char* CThostFtdcTraderApi::GetApiVersion() {
  return "fake-trader";
}
FAKE_EXPORT void asterion_fake_trader_hold_insert_return(int hold, int code) {
  insert_return_code = code;
  hold_insert_return = hold != 0;
  hold_insert_return.notify_all();
}
FAKE_EXPORT void asterion_fake_trader_hold_cancel_return(int hold) {
  hold_cancel_return = hold != 0;
  hold_cancel_return.notify_all();
}
FAKE_EXPORT void asterion_fake_trader_hold_insert_errors(int hold) {
  hold_insert_errors = hold != 0;
  if (!hold) {
    std::lock_guard lock(live_mutex);
    for (auto* fake : live)
      fake->release_insert_errors();
  }
}
FAKE_EXPORT void asterion_fake_trader_hold_quote_return(int hold) {
  hold_quote_return = hold != 0;
  hold_quote_return.notify_all();
}
FAKE_EXPORT int asterion_fake_trader_quote_return_waiting() {
  return quote_return_waiting.load();
}
FAKE_EXPORT void asterion_fake_trader_hold_release(int hold) {
  hold_release = hold != 0;
  hold_release.notify_all();
}
FAKE_EXPORT int asterion_fake_trader_sdk_creations() {
  return sdk_creations;
}
FAKE_EXPORT int asterion_fake_trader_sdk_releases() {
  return sdk_releases;
}
FAKE_EXPORT int asterion_fake_trader_sdk_thread_mismatches() {
  return sdk_thread_mismatches;
}
FAKE_EXPORT void asterion_fake_trader_reset() {
  sdk_creations = sdk_releases = sdk_thread_mismatches = 0;
  hold_positions = hold_trades = false;
  pending_positions = 0;
  quote_rejection_code = 0;
  quote_rejection_count = 0;
  reject_inserts = false;
  quote_mode = cancel_return_code = 0;
  hold_insert_errors = false;
  auto& x = exchange();
  std::lock_guard lock(x.mutex);
  x.orders.clear();
  x.trades.clear();
  x.positions.clear();
  x.trading_day = "20260928";
  x.query_rejections = 0;
}
FAKE_EXPORT int asterion_fake_trader_next_session() {
  auto& x = exchange();
  std::lock_guard lock(x.mutex);
  return x.sessions + 1;
}
FAKE_EXPORT void asterion_fake_trader_hold_positions(int hold) {
  hold_positions = hold != 0;
  if (!hold) {
    std::lock_guard lock(live_mutex);
    for (auto* fake : live)
      fake->release_deferred(true);
  }
}
FAKE_EXPORT int asterion_fake_trader_pending_positions() {
  return pending_positions.load();
}
FAKE_EXPORT void asterion_fake_trader_hold_trades(int hold) {
  hold_trades = hold != 0;
  if (!hold) {
    std::lock_guard lock(live_mutex);
    for (auto* fake : live)
      fake->release_deferred(false);
  }
}
FAKE_EXPORT void asterion_fake_trader_fill(int index, int volume) {
  std::lock_guard lock(live_mutex);
  if (!live.empty())
    live.front()->fill(index, volume);
}
FAKE_EXPORT void asterion_fake_trader_replay_order(int index, int filled) {
  std::lock_guard lock(live_mutex);
  if (!live.empty())
    live.front()->replay_order(index, filled);
}
FAKE_EXPORT void asterion_fake_trader_report_burst(int count) {
  std::lock_guard lock(live_mutex);
  live.front()->report_burst(count);
}
FAKE_EXPORT void asterion_fake_trader_reconnect() {
  std::lock_guard lock(live_mutex);
  for (auto* fake : live)
    fake->reconnect();
}
FAKE_EXPORT int asterion_fake_trader_query_rejections() {
  auto& x = exchange();
  std::lock_guard lock(x.mutex);
  return x.query_rejections;
}

FAKE_EXPORT int asterion_fake_catalog_side_effects() {
  return catalog_side_effects.load();
}

FAKE_EXPORT void asterion_fake_trader_quote_mode(int mode) {
  quote_mode = mode;
}
FAKE_EXPORT void asterion_fake_trader_reject_inserts(int reject) {
  reject_inserts = reject != 0;
}
FAKE_EXPORT void asterion_fake_trader_cancel_code(int code) {
  cancel_return_code = code;
}

FAKE_EXPORT void asterion_fake_trader_reject_quotes(int code, int count) {
  quote_rejection_code = code;
  quote_rejection_count = count;
}

// Test-only day transition: daily reports expire, holdings carry, exchange IDs
// may repeat. No production service exposes this control.
FAKE_EXPORT void asterion_fake_trader_next_day(const char* day) {
  {
    auto& x = exchange();
    std::lock_guard lock(x.mutex);
    x.trading_day = day;
    x.orders.clear();
    x.trades.clear();
    x.sys_ids = x.trade_ids = 0;
    for (auto& [_, position] : x.positions) {
      position.yesterday += position.today;
      position.today = 0;
    }
  }
  asterion_fake_trader_reconnect();
}

FAKE_EXPORT void asterion_fake_trader_stale_queries() {
  std::lock_guard lock(live_mutex);
  for (auto* fake : live)
    fake->stale_queries();
}
FAKE_EXPORT int asterion_fake_trader_stale_batches() {
  return stale_batches.load();
}
