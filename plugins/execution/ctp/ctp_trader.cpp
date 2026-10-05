#include <array>
#include "ctp_trader.hpp"
#include "ctp_reconciliation.hpp"
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
#include <variant>
#include <functional>
namespace asterion::ctp {
namespace {
using namespace std::chrono_literals;
// CTP allows one query per second per session; keep a margin.
constexpr auto query_spacing = 1100ms;
constexpr auto query_timeout = 10s;
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
struct FrontConnectedEvent {};
struct FrontDisconnectedEvent {
  int reason;
};
struct RspAuthenticateEvent {
  std::optional<CThostFtdcRspInfoField> info;
};
struct RspUserLoginEvent {
  std::optional<CThostFtdcRspUserLoginField> login;
  std::optional<CThostFtdcRspInfoField> info;
};
struct RspSettlementInfoConfirmEvent {
  std::optional<CThostFtdcRspInfoField> info;
};
struct RspQryOrderEvent {
  std::optional<CThostFtdcOrderField> order;
  std::optional<CThostFtdcRspInfoField> info;
  int request;
  bool last;
};
struct RspQryTradeEvent {
  std::optional<CThostFtdcTradeField> trade;
  std::optional<CThostFtdcRspInfoField> info;
  int request;
  bool last;
};
struct RspQryInvestorPositionEvent {
  std::optional<CThostFtdcInvestorPositionField> row;
  std::optional<CThostFtdcRspInfoField> info;
  int request;
  bool last;
};
struct RspQryTradingAccountEvent {
  std::optional<CThostFtdcTradingAccountField> account;
  std::optional<CThostFtdcRspInfoField> info;
  int request;
  bool last;
};
struct RspQryInstrumentMarginRateEvent {
  std::optional<CThostFtdcInstrumentMarginRateField> row;
  std::optional<CThostFtdcRspInfoField> info;
  int request;
  bool last;
};
struct RspQryInstrumentCommissionRateEvent {
  std::optional<CThostFtdcInstrumentCommissionRateField> row;
  std::optional<CThostFtdcRspInfoField> info;
  int request;
  bool last;
};
struct RspQryDepthMarketDataEvent {
  std::optional<CThostFtdcDepthMarketDataField> row;
  std::optional<CThostFtdcRspInfoField> info;
  int request;
  bool last;
};
struct RtnOrderEvent {
  std::optional<CThostFtdcOrderField> order;
};
struct RtnTradeEvent {
  std::optional<CThostFtdcTradeField> trade;
};
struct RspOrderInsertEvent {
  std::optional<CThostFtdcRspInfoField> info;
  int request;
};
struct RspOrderActionEvent {
  std::optional<CThostFtdcInputOrderActionField> input;
  std::optional<CThostFtdcRspInfoField> info;
};
struct ErrRtnOrderActionEvent {
  std::optional<CThostFtdcOrderActionField> input;
  std::optional<CThostFtdcRspInfoField> info;
};
struct RspErrorEvent {
  std::optional<CThostFtdcRspInfoField> info;
};
using CallbackData =
    std::variant<FrontConnectedEvent, FrontDisconnectedEvent, RspAuthenticateEvent,
                 RspUserLoginEvent, RspSettlementInfoConfirmEvent, RspQryOrderEvent,
                 RspQryTradeEvent, RspQryInvestorPositionEvent, RspQryTradingAccountEvent,
                 RspQryInstrumentMarginRateEvent, RspQryInstrumentCommissionRateEvent,
                 RspQryDepthMarketDataEvent, RtnOrderEvent, RtnTradeEvent, RspOrderInsertEvent,
                 RspOrderActionEvent, ErrRtnOrderActionEvent, RspErrorEvent>;
struct CallbackEntry {
  CallbackData data;
  std::chrono::steady_clock::time_point observed_at;
};
template <class T> std::optional<T> copy_callback(const T* value) noexcept {
  return value ? std::optional<T>(*value) : std::nullopt;
}

struct Command {
  Kind kind;
  std::string order_id;
  BrokerSendPermit permit;
  std::uint64_t generation = 0;
  std::uint64_t exposure_revision = 0;
  int request_id = 0;
  CThostFtdcInputOrderField order{};
  CThostFtdcInputOrderActionField action{};
  // Rate queries: the contract and its product code.
  InstrumentId instrument;
  std::string product;
  std::uint64_t serial = 0; // quote queries
  std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
  std::shared_ptr<std::promise<BrokerDispatchResult>> result;
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
  struct Prepared final : PreparedBrokerOrder {
    Impl* owner;
    Command command;
    Prepared(Impl* owner, Command command, BrokerOrder order)
        : PreparedBrokerOrder(std::move(order)), owner(owner), command(std::move(command)) {}
  };
  using Factory = CThostFtdcTraderApi* (*)(const char*);
  std::unique_ptr<SharedLibrary> library;
  Factory factory = nullptr;
  std::filesystem::path flow;
  CThostFtdcTraderApi* api = nullptr;
  mutable std::mutex mutex;
  std::condition_variable wake;
  TraderConfiguration config;
  BrokerSnapshot state;
  ReportReconciler reports{state};
  std::uint64_t position_query_revision = 0;
  bool position_refresh_again = false;
  // Rate answers per contract until both queries complete.
  struct Rates {
    std::string product;
    bool margin_done = false, commission_done = false;
    std::optional<std::pair<Decimal, Decimal>> margin; // per lot, by money
    std::optional<std::array<Decimal, 6>> commission;
  };
  std::map<InstrumentId, Rates> rates;
  InstrumentId rate_instrument; // the outstanding rate query
  // Quote futures complete after both SDK acceptance and the final response.
  std::uint64_t quote_serial = 0, quote_outstanding = 0;
  struct QuoteAnswer {
    std::optional<BrokerQuote> quote;
    bool completed = false, dispatched = false;
    InstrumentId instrument;
    std::uint64_t generation = 0;
    std::chrono::steady_clock::time_point deadline;
    std::promise<std::optional<BrokerQuote>> result;
  };
  std::map<std::uint64_t, QuoteAnswer> quotes;
  std::deque<Command> queue;
  // Session identity from the latest login; generation changes on every
  // disconnect so requests prepared for an older session are never sent.
  int front_id = 0, session_id = 0, next_ref = 0, request_id = 0;
  std::uint64_t generation = 0, query_generation = 0;
  int query_request_id = 0;
  Kind query_kind = Kind::query_orders;
  bool closing = true, query_outstanding = false, refresh_queued = false;
  std::chrono::steady_clock::time_point query_started, next_query_at;
  struct Connection {
    TraderConfiguration config;
    KnownOrders known;
  };
  std::optional<Connection> connecting;
  bool order_in_flight = false;
  // Original requests awaiting their first broker fact. Both rejection and
  // pre-report cancellation use this identity, never the latest login identity.
  struct OrderRequest {
    BrokerOrder order;
    CThostFtdcInputOrderActionField cancel;
  };
  std::map<int, OrderRequest> order_requests;
  std::jthread commands;

  explicit Impl(const std::filesystem::path& path, const std::filesystem::path& directory,
                BrokerSendGate& gate, std::function<void()> ready)
      : flow(directory), send_gate(gate), events_ready(std::move(ready)) {
    library = std::make_unique<SharedLibrary>(
        path, "?CreateFtdcTraderApi@CThostFtdcTraderApi@@SAPEAV1@PEBD@Z",
        "_ZN19CThostFtdcTraderApi19CreateFtdcTraderApiEPKc");
    factory = library->symbol<Factory>();
    if (!factory) {
      library.reset();
      throw Error(ErrorCode::unavailable, "CTP 6.7.7 trader SDK unavailable or incompatible");
    }
    commands = std::jthread([this](std::stop_token stop) {
      lifecycle(stop);
      sdk_stopped = true;
      wake.notify_all();
      events_ready();
    });
  }
  ~Impl() {
    close();
    commands.request_stop();
    wake.notify_all();
    // Keep the sole state owner alive while Release stops callback producers.
    // The SDK may still need its reserved owner operation to finish shutdown.
    while (!sdk_stopped) {
      std::unique_lock lock(mutex);
      poll_owner();
      wake.wait_for(lock, 25ms, [this] { return sdk_stopped.load(); });
    }
    commands.join();
    {
      std::lock_guard lock(mutex);
      poll_owner(); // Every producer is stopped; consume the final accepted prefix.
    }
    library.reset();
  }
  void changed() { ++state.sequence; }
  void fail(int code) {
    send_gate.invalidate();
    state.phase = "error";
    state.error_code = code;
    changed();
  }
  // A fixed ring owns callback bytes. Producers never take the state mutex.
  // The ingress lock linearizes invalidation with publication and acknowledgement.
  static constexpr std::size_t callback_capacity = 1024;
  std::array<std::optional<CallbackEntry>, callback_capacity> callbacks;
  std::mutex ingress_mutex;
  std::size_t callback_head = 0, callback_size = 0;
  int ingress_error = 0;
  BrokerSendGate& send_gate;
  std::function<void()> events_ready;
  // The single SDK worker can have only one owner operation awaiting completion.
  // This reserved slot is independent of callback and external request capacity.
  std::function<void()> owner_work;
  std::atomic<bool> sdk_stopped = false;
  template <class F> auto on_owner(F action) {
    using Result = std::invoke_result_t<F>;
    auto task = std::make_shared<std::packaged_task<Result()>>(std::move(action));
    auto result = task->get_future();
    {
      std::lock_guard lock(ingress_mutex);
      owner_work = [task = std::move(task)] { (*task)(); };
    }
    events_ready();
    wake.notify_all();
    return result.get();
  }
  void poll_owner() {
    drain_callbacks();
    if (query_outstanding && std::chrono::steady_clock::now() - query_started >= query_timeout) {
      query_outstanding = false;
      fail(-1002);
      finish_quotes();
    }
    std::function<void()> work;
    {
      std::lock_guard lock(ingress_mutex);
      work = std::exchange(owner_work, {});
    }
    if (work)
      work();
  }
  template <class T> void receive(T value, bool affects_permission) noexcept {
    const auto observed_at = std::chrono::steady_clock::now();
    {
      std::lock_guard lock(ingress_mutex);
      if (affects_permission)
        send_gate.pending();
      if (callback_size == callback_capacity) {
        ingress_error = -1008;
        if (!affects_permission)
          send_gate.pending();
      } else {
        callbacks[(callback_head + callback_size) % callback_capacity].emplace(
            CallbackEntry{std::move(value), observed_at});
        ++callback_size;
      }
    }
    events_ready();
  }
  // Called only by the account owner, under the adapter state mutex.
  void drain_callbacks() {
    std::size_t count;
    {
      std::lock_guard lock(ingress_mutex);
      count = callback_size;
    }
    for (std::size_t i = 0; i < count; ++i) {
      CallbackEntry entry;
      {
        std::lock_guard lock(ingress_mutex);
        auto& slot = callbacks[callback_head];
        entry = std::move(*slot);
        slot.reset();
        callback_head = (callback_head + 1) % callback_capacity;
        --callback_size;
      }
      {
        bool malformed = false;
        try {
          std::visit(
              [&](const auto& value) {
                using Event = std::decay_t<decltype(value)>;
                constexpr bool fact = std::is_same_v<Event, RtnOrderEvent> ||
                                      std::is_same_v<Event, RtnTradeEvent> ||
                                      std::is_same_v<Event, RspOrderInsertEvent> ||
                                      std::is_same_v<Event, RspOrderActionEvent> ||
                                      std::is_same_v<Event, ErrRtnOrderActionEvent>;
                if (!closing || fact)
                  apply(value, entry.observed_at);
              },
              entry.data);
        } catch (const std::invalid_argument&) {
          malformed = true;
        } catch (const std::runtime_error&) {
          malformed = true;
        }
        if (malformed) {
          std::lock_guard lock(ingress_mutex);
          ingress_error = -1000;
          send_gate.pending();
        }
      }
    }
    {
      std::lock_guard lock(ingress_mutex);
      if (ingress_error) {
        if (!closing && (state.phase != "error" || state.error_code != ingress_error ||
                         state.positions_reconciled)) {
          state.positions_reconciled = false;
          fail(ingress_error);
        }
      } else if (!callback_size) {
        send_gate.acknowledge(send_gate.revision());
      }
      if (callback_size)
        events_ready();
    }
    finish_quotes();
    wake.notify_one();
  }
  void finish_quotes() {
    const auto now = std::chrono::steady_clock::now();
    for (auto it = quotes.begin(); it != quotes.end();) {
      auto& answer = it->second;
      const bool valid = !closing && generation == answer.generation && state.phase == "ready" &&
                         now < answer.deadline;
      if (valid && !(answer.dispatched && answer.completed)) {
        ++it;
        continue;
      }
      if (!valid || (answer.quote && (answer.quote->instrument != answer.instrument ||
                                      answer.quote->trading_day != state.trading_day)))
        answer.quote.reset();
      answer.result.set_value(std::move(answer.quote));
      const auto serial = it->first;
      std::erase_if(queue, [serial](const Command& command) {
        return command.kind == Kind::query_quote && command.serial == serial;
      });
      it = quotes.erase(it);
    }
  }
  void quote_dispatched(std::uint64_t serial, int code) {
    if (const auto found = quotes.find(serial); found != quotes.end()) {
      auto& answer = found->second;
      answer.dispatched = true;
      if (code) {
        answer.completed = true;
        answer.quote.reset();
      }
    }
    finish_quotes();
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

  // Callback producers only copy owned records. apply() runs in poll().
  void OnFrontConnected() noexcept override { receive(FrontConnectedEvent{}, true); }
  void apply(const FrontConnectedEvent&, std::chrono::steady_clock::time_point) {

    state.phase = "authenticating";
    state.error_code = 0;
    changed();
    push(Kind::authenticate);
  }
  void OnFrontDisconnected(int reason) noexcept override {
    receive(FrontDisconnectedEvent{reason}, true);
  }
  void apply(const FrontDisconnectedEvent& event, std::chrono::steady_clock::time_point) {
    const auto& reason = event.reason;
    ++generation;
    query_outstanding = refresh_queued = false;
    state.positions_reconciled = false;
    std::erase_if(queue, [](const Command& c) { return !c.result; });
    abandon_rates();
    reports.abandon_positions();
    state.costs.clear();
    state.phase = "connecting";
    state.error_code = reason;
    changed();
  }
  void OnRspAuthenticate(CThostFtdcRspAuthenticateField*, CThostFtdcRspInfoField* info, int,
                         bool) noexcept override {
    receive(RspAuthenticateEvent{copy_callback(info)}, true);
  }
  void apply(const RspAuthenticateEvent& event, std::chrono::steady_clock::time_point) {
    const auto& info = event.info;
    if (info && info->ErrorID) {
      erase(config.password);
      erase(config.auth_code);
      return fail(info->ErrorID);
    }
    state.phase = "logging_in";
    changed();
    push(Kind::login);
  }
  void OnRspUserLogin(CThostFtdcRspUserLoginField* login, CThostFtdcRspInfoField* info, int,
                      bool) noexcept override {
    receive(RspUserLoginEvent{copy_callback(login), copy_callback(info)}, true);
  }
  void apply(const RspUserLoginEvent& event, std::chrono::steady_clock::time_point) {
    const auto& login = event.login;
    const auto& info = event.info;
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
    reports.begin_day(field(login->TradingDay));
    order_requests.clear();
    rates.clear();
    state.funds.reset();
    state.costs.clear();
    state.synchronized_ms = 0;
    state.positions_reconciled = false;
    state.phase = "confirming";
    changed();
    push(Kind::confirm);
  }
  void OnRspSettlementInfoConfirm(CThostFtdcSettlementInfoConfirmField*,
                                  CThostFtdcRspInfoField* info, int, bool) noexcept override {
    receive(RspSettlementInfoConfirmEvent{copy_callback(info)}, true);
  }
  void apply(const RspSettlementInfoConfirmEvent& event, std::chrono::steady_clock::time_point) {
    const auto& info = event.info;
    if (info && info->ErrorID)
      return fail(info->ErrorID);
    synchronize();
  }
  void OnRspQryOrder(CThostFtdcOrderField* order, CThostFtdcRspInfoField* info, int request,
                     bool last) noexcept override {
    receive(RspQryOrderEvent{copy_callback(order), copy_callback(info), request, last}, true);
  }
  void apply(const RspQryOrderEvent& event, std::chrono::steady_clock::time_point) {
    const auto& order = event.order;
    const auto& info = event.info;
    const auto& request = event.request;
    const auto& last = event.last;
    if (!accepts_query(request, Kind::query_orders))
      return;
    if (info && info->ErrorID)
      return fail(info->ErrorID);
    if (order)
      apply_order(*order);
    if (last)
      query_done();
  }
  void OnRspQryTrade(CThostFtdcTradeField* trade, CThostFtdcRspInfoField* info, int request,
                     bool last) noexcept override {
    receive(RspQryTradeEvent{copy_callback(trade), copy_callback(info), request, last}, true);
  }
  void apply(const RspQryTradeEvent& event, std::chrono::steady_clock::time_point) {
    const auto& trade = event.trade;
    const auto& info = event.info;
    const auto& request = event.request;
    const auto& last = event.last;
    if (!accepts_query(request, Kind::query_trades))
      return;
    if (info && info->ErrorID)
      return fail(info->ErrorID);
    if (trade)
      reports.apply(*trade);
    if (last)
      query_done();
  }
  void OnRspQryInvestorPosition(CThostFtdcInvestorPositionField* row, CThostFtdcRspInfoField* info,
                                int request, bool last) noexcept override {
    receive(RspQryInvestorPositionEvent{copy_callback(row), copy_callback(info), request, last},
            true);
  }
  void apply(const RspQryInvestorPositionEvent& event, std::chrono::steady_clock::time_point) {
    const auto& row = event.row;
    const auto& info = event.info;
    const auto& request = event.request;
    const auto& last = event.last;
    if (!accepts_query(request, Kind::query_positions))
      return;
    if (info && info->ErrorID)
      return fail(info->ErrorID);
    if (row)
      reports.apply(*row);
    if (last) {
      position_refresh_again = reports.finish_positions(position_query_revision);
      position_query_revision = state.exposure_revision;
      query_done();
    }
  }
  void OnRspQryTradingAccount(CThostFtdcTradingAccountField* account, CThostFtdcRspInfoField* info,
                              int request, bool last) noexcept override {
    receive(RspQryTradingAccountEvent{copy_callback(account), copy_callback(info), request, last},
            true);
  }
  void apply(const RspQryTradingAccountEvent& event, std::chrono::steady_clock::time_point) {
    const auto& account = event.account;
    const auto& info = event.info;
    const auto& request = event.request;
    const auto& last = event.last;
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
      if (position_refresh_again || position_query_revision != state.exposure_revision)
        refresh();
      changed();
    }
  }
  void OnRspQryInstrumentMarginRate(CThostFtdcInstrumentMarginRateField* row,
                                    CThostFtdcRspInfoField* info, int request,
                                    bool last) noexcept override {
    receive(RspQryInstrumentMarginRateEvent{copy_callback(row), copy_callback(info), request, last},
            false);
  }
  void apply(const RspQryInstrumentMarginRateEvent& event, std::chrono::steady_clock::time_point) {
    const auto& row = event.row;
    const auto& info = event.info;
    const auto& request = event.request;
    const auto& last = event.last;
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
  }
  void OnRspQryInstrumentCommissionRate(CThostFtdcInstrumentCommissionRateField* row,
                                        CThostFtdcRspInfoField* info, int request,
                                        bool last) noexcept override {
    receive(
        RspQryInstrumentCommissionRateEvent{copy_callback(row), copy_callback(info), request, last},
        false);
  }
  void apply(const RspQryInstrumentCommissionRateEvent& event,
             std::chrono::steady_clock::time_point) {
    const auto& row = event.row;
    const auto& info = event.info;
    const auto& request = event.request;
    const auto& last = event.last;
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
  }
  void OnRspQryDepthMarketData(CThostFtdcDepthMarketDataField* row, CThostFtdcRspInfoField* info,
                               int request, bool last) noexcept override {
    receive(RspQryDepthMarketDataEvent{copy_callback(row), copy_callback(info), request, last},
            false);
  }
  void apply(const RspQryDepthMarketDataEvent& event,
             std::chrono::steady_clock::time_point observed_at) {
    const auto& row = event.row;
    const auto& info = event.info;
    const auto& request = event.request;
    const auto& last = event.last;
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
    if (!code && row && !answer.quote) {
      BrokerQuote quote;
      quote.instrument = {field(row->ExchangeID), field(row->InstrumentID)};
      quote.last = price(row->LastPrice);
      quote.pre_settlement = price(row->PreSettlementPrice);
      quote.upper_limit = price(row->UpperLimitPrice);
      quote.lower_limit = price(row->LowerLimitPrice);
      quote.trading_day = field(row->TradingDay);
      quote.update_time = field(row->UpdateTime);
      answer.quote = std::move(quote);
    }
    if (last || code) {
      answer.completed = true;
      if (code)
        answer.quote.reset();
      else if (answer.quote) {
        answer.quote->connection_generation = generation;
        answer.quote->completed_at = observed_at;
      }
      query_done();
    }
  }
  void OnRtnOrder(CThostFtdcOrderField* order) noexcept override {
    receive(RtnOrderEvent{copy_callback(order)}, true);
  }
  void apply(const RtnOrderEvent& event, std::chrono::steady_clock::time_point) {
    const auto& order = event.order;
    if (order)
      apply_order(*order);
  }
  void apply_order(const CThostFtdcOrderField& order) {
    if (reports.apply(order))
      refresh();
    if (field(order.TradingDay) != state.trading_day)
      return;
    const auto key = ReportReconciler::key(order.FrontID, order.SessionID, trimmed(order.OrderRef));
    if (reports.find(key)->status == BrokerOrderStatus::submitted)
      return;
    std::erase_if(order_requests,
                  [&](const auto& entry) { return entry.second.order.broker_key == key; });
  }
  void OnRtnTrade(CThostFtdcTradeField* trade) noexcept override {
    receive(RtnTradeEvent{copy_callback(trade)}, true);
  }
  void apply(const RtnTradeEvent& event, std::chrono::steady_clock::time_point) {
    const auto& trade = event.trade;
    if (trade && reports.apply(*trade))
      refresh();
  }
  void OnRspOrderInsert(CThostFtdcInputOrderField*, CThostFtdcRspInfoField* info, int request,
                        bool) noexcept override {
    receive(RspOrderInsertEvent{copy_callback(info), request}, true);
  }
  void apply(const RspOrderInsertEvent& event, std::chrono::steady_clock::time_point) {
    const auto& info = event.info;
    if (!info || !info->ErrorID)
      return;
    const auto request = order_requests.find(event.request);
    if (request == order_requests.end())
      return;
    reports.reject(request->second.order, info->ErrorID);
    order_requests.erase(request);
  }
  void OnRspOrderAction(CThostFtdcInputOrderActionField* input, CThostFtdcRspInfoField* info, int,
                        bool) noexcept override {
    receive(RspOrderActionEvent{copy_callback(input), copy_callback(info)}, true);
  }
  void apply(const RspOrderActionEvent& event, std::chrono::steady_clock::time_point) {
    const auto& input = event.input;
    const auto& info = event.info;
    if (input && info && info->ErrorID)
      reports.action_failed(
          ReportReconciler::key(input->FrontID, input->SessionID, trimmed(input->OrderRef)),
          info->ErrorID);
  }
  void OnErrRtnOrderAction(CThostFtdcOrderActionField* input,
                           CThostFtdcRspInfoField* info) noexcept override {
    receive(ErrRtnOrderActionEvent{copy_callback(input), copy_callback(info)}, true);
  }
  void apply(const ErrRtnOrderActionEvent& event, std::chrono::steady_clock::time_point) {
    const auto& input = event.input;
    const auto& info = event.info;
    if (input && info && info->ErrorID)
      reports.action_failed(
          ReportReconciler::key(input->FrontID, input->SessionID, trimmed(input->OrderRef)),
          info->ErrorID);
  }
  void OnRspError(CThostFtdcRspInfoField* info, int, bool) noexcept override {
    receive(RspErrorEvent{copy_callback(info)}, true);
  }
  void apply(const RspErrorEvent& event, std::chrono::steady_clock::time_point) {
    const auto& info = event.info;
    if (info && info->ErrorID)
      fail(info->ErrorID);
  }
  void OnErrRtnOrderInsert(CThostFtdcInputOrderField* input,
                           CThostFtdcRspInfoField* info) noexcept override {
    if (input)
      OnRspOrderInsert(input, info, input->RequestID, true);
  }

  // ---- worker ----
  // Session requests: authenticate, login and settlement confirmation.
  int send(const Command& command) {
    const int id = command.request_id;
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
    const int id = command.request_id;
    {
      std::lock_guard lock(mutex);
      if (closing || command.generation != generation)
        return -1003;
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
  // All scheduling, query bookkeeping and completion projection runs on the owner.
  std::optional<Command> next_command() {
    if (closing)
      return std::nullopt;
    const auto now = std::chrono::steady_clock::now();
    finish_quotes();
    auto next = std::find_if(queue.begin(), queue.end(),
                             [](const Command& c) { return !is_query(c.kind); });
    if (next == queue.end() && !query_outstanding && now >= next_query_at) {
      next = std::find_if(queue.begin(), queue.end(),
                          [](const Command& c) { return c.kind == Kind::query_quote; });
      if (next == queue.end())
        next = queue.begin();
    }
    if (next == queue.end())
      return std::nullopt;
    auto command = std::move(*next);
    queue.erase(next);
    if (command.generation != generation || now >= command.deadline ||
        (command.kind == Kind::query_quote && !quotes.contains(command.serial))) {
      if (command.result)
        complete(command, now >= command.deadline ? -1005 : -1003, false);
      return std::nullopt;
    }
    if (command.kind != Kind::insert)
      command.request_id = ++request_id;
    if (is_query(command.kind)) {
      query_outstanding = true;
      query_started = now;
      next_query_at = now + query_spacing;
      query_request_id = command.request_id;
      query_kind = command.kind;
      query_generation = command.generation;
    }
    if (command.kind == Kind::query_positions) {
      position_query_revision = state.exposure_revision;
      position_refresh_again = false;
      reports.abandon_positions();
    }
    if (is_rate_query(command.kind))
      rate_instrument = command.instrument;
    if (command.kind == Kind::query_quote)
      quote_outstanding = command.serial;
    return command;
  }
  void returned(Command& command, int code, bool invoked) {
    if (is_query(command.kind) && (code == -2 || code == -3) && !closing &&
        command.generation == generation) {
      query_outstanding = false;
      next_query_at = std::chrono::steady_clock::now() + query_spacing;
      queue.push_front(std::move(command));
      return;
    }
    if (command.kind == Kind::query_quote)
      quote_dispatched(command.serial, code);
    else if (command.result)
      complete(command, code, invoked);
    if (!code || closing || command.generation != generation)
      return;
    if (is_query(command.kind))
      query_outstanding = false;
    if (is_rate_query(command.kind))
      rate_answered(command.kind, code);
    else if (!command.result && command.kind != Kind::query_quote)
      fail(code);
  }
  void run(std::stop_token stop) {
    while (!stop.stop_requested()) {
      {
        std::unique_lock lock(mutex);
        wake.wait_for(lock, 50ms);
        if (closing)
          return;
      }
      auto next = on_owner([this] { return next_command(); });
      if (!next)
        continue;
      auto& command = *next;
      int code = 0;
      {
        std::lock_guard lock(mutex);
        const auto now = std::chrono::steady_clock::now();
        if (closing || command.generation != generation)
          code = -1003;
        else if (now >= command.deadline)
          code = -1005;
        else if (command.kind == Kind::insert &&
                 (command.exposure_revision != state.exposure_revision ||
                  (command.order.CombOffsetFlag[0] == THOST_FTDC_OF_Open &&
                   !state.positions_reconciled)))
          code = -1004;
        else if (command.kind == Kind::insert && !command.permit.consume(command.order_id))
          code = -1007;
      }
      const bool invoked = !code;
      // No owner handoff, wait or queue after consuming an order permit.
      if (invoked) {
        if (is_query(command.kind))
          code = send_query(command);
        else if (command.kind == Kind::insert)
          code = api->ReqOrderInsert(&command.order, command.request_id);
        else if (command.kind == Kind::cancel)
          code = api->ReqOrderAction(&command.action, command.request_id);
        else
          code = send(command);
      }
      on_owner([&] { returned(command, code, invoked); });
    }
  }
  // Called under the adapter mutex. SDK returns cannot replace broker facts.
  void complete(Command& command, int code, bool invoked) {
    if (command.kind == Kind::insert) {
      order_in_flight = false;
      command.permit = {};
      if (!invoked)
        order_requests.erase(command.request_id);
    }
    command.result->set_value({code, invoked});
  }
  std::future<BrokerDispatchResult> enqueue(Command command) {
    command.result = std::make_shared<std::promise<BrokerDispatchResult>>();
    auto future = command.result->get_future();
    queue.push_back(std::move(command));
    return future;
  }
  // The sole SDK thread owns creation, initialization, requests and Release.
  // A replacement connection waits in one slot while the old SDK retires;
  // disconnect and snapshot never wait for a blocked vendor Release call.
  void lifecycle(std::stop_token stop) {
    while (!stop.stop_requested()) {
      {
        std::unique_lock lock(mutex);
        wake.wait(lock, [&] { return stop.stop_requested() || connecting.has_value(); });
        if (stop.stop_requested())
          return;
      }
      const bool start = on_owner([this, stop] {
        if (stop.stop_requested() || !connecting)
          return false;
        {
          std::lock_guard ingress_lock(ingress_mutex);
          // Release has ended every producer of the previous connection, and
          // poll_owner applied its last callbacks before this owner operation.
          ingress_error = 0;
          send_gate.acknowledge(send_gate.revision());
        }
        config = std::move(connecting->config);
        reports.reset(std::move(connecting->known));
        connecting.reset();
        rates.clear();
        order_requests.clear();
        order_in_flight = false;
        state = {};
        state.phase = "connecting";
        closing = false;
        changed();
        return true;
      });
      if (!start)
        continue;
      try {
        std::filesystem::create_directories(flow);
        const auto raw = flow.u8string();
        std::string path(raw.begin(), raw.end());
        path += "/";
        api = factory(path.c_str());
        if (!api)
          throw Error(ErrorCode::unavailable, "CTP trader factory failed");
        api->RegisterSpi(this);
        api->RegisterFront(config.front.data());
        // QUICK: only new reports; orders and trades are queried after login.
        api->SubscribePrivateTopic(THOST_TERT_QUICK);
        api->SubscribePublicTopic(THOST_TERT_QUICK);
        api->Init();
        run(stop);
      } catch (const std::runtime_error&) {
        on_owner([this] {
          close_locked();
          fail(-1000);
        });
      }
      // No state lock is held across a vendor call, including Release. A
      // supervisor may terminate the whole process if this call never returns.
      if (api) {
        api->RegisterSpi(nullptr);
        api->Release();
        api = nullptr;
      }
    }
  }
  void close_locked() {
    erase(config.password);
    erase(config.auth_code);
    if (closing)
      return;
    closing = true;
    ++generation;
    for (auto& command : queue)
      if (command.result)
        complete(command, -1003, false);
    queue.clear();
    query_outstanding = refresh_queued = false;
    abandon_rates();
    state.phase = "disconnected";
    changed();
    finish_quotes();
  }
  void close(std::optional<std::pair<std::uint64_t, std::uint64_t>> expected = {}) {
    {
      std::lock_guard lock(mutex);
      std::unique_lock ingress(ingress_mutex, std::defer_lock);
      if (expected) {
        ingress.lock();
        if (callback_size || ingress_error || closing || state.phase != "ready" ||
            generation != expected->first || state.exposure_revision != expected->second ||
            !state.positions_reconciled)
          throw Error(ErrorCode::conflict, "broker state changed before account freeze");
      }
      if (connecting) {
        erase(connecting->config.password);
        erase(connecting->config.auth_code);
        connecting.reset();
        state.phase = "disconnected";
        changed();
      }
      close_locked();
    }
    wake.notify_all();
  }
};

Trader::Trader(const std::filesystem::path& library, const std::filesystem::path& flow,
               BrokerSendGate& gate, std::function<void()> events_ready)
    : impl_(std::make_unique<Impl>(library, flow, gate, std::move(events_ready))) {}
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
void Trader::connect(TraderConfiguration config, KnownOrders known) {
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
  {
    std::lock_guard lock(impl_->mutex);
    impl_->close_locked();
    if (impl_->connecting) {
      erase(impl_->connecting->config.password);
      erase(impl_->connecting->config.auth_code);
    }
    impl_->connecting = Impl::Connection{std::move(config), std::move(known)};
    impl_->state.phase = "connecting";
    impl_->state.error_code = 0;
    impl_->changed();
  }
  impl_->wake.notify_all();
}
bool Trader::restore_orders(std::string_view day, std::uint64_t generation,
                            const KnownOrders& known) {
  std::lock_guard lock(impl_->mutex);
  if (impl_->generation != generation || impl_->state.trading_day != day)
    return false;
  for (const auto& [identity, order] : known)
    impl_->reports.remember(identity, order);
  return true;
}
std::unique_ptr<PreparedBrokerOrder>
Trader::prepare(const LimitOrder& order, Offset offset, std::uint64_t connection_generation,
                std::uint64_t exposure_revision, std::chrono::steady_clock::time_point deadline) {
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
  command.deadline = deadline;
  if (std::chrono::steady_clock::now() >= deadline)
    throw Error(ErrorCode::unavailable, "order dispatch deadline expired; the order was not sent");
  BrokerOrder pending;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->closing || impl_->state.phase != "ready")
      throw Error(ErrorCode::unavailable, "CTP trading session is not ready");
    if (impl_->order_in_flight)
      throw Error(ErrorCode::resource_exhausted, "an order is awaiting SDK completion");
    if (connection_generation != impl_->generation)
      throw Error(ErrorCode::unavailable, "CTP trading session changed; the order was not sent");
    if (offset == Offset::open && !impl_->state.positions_reconciled)
      throw Error(ErrorCode::unavailable,
                  "broker fills are awaiting position reconciliation; opening orders are paused");
    if (exposure_revision != impl_->state.exposure_revision)
      throw Error(ErrorCode::unavailable, "broker exposure changed; evaluate pre-trade risk again");
    if (impl_->reports.contains(order.id))
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
    auto& cancel = command.action;
    copy(cancel.BrokerID, field(input.BrokerID));
    copy(cancel.InvestorID, field(input.InvestorID));
    copy(cancel.UserID, field(input.UserID));
    copy(cancel.InstrumentID, order.instrument.symbol);
    copy(cancel.ExchangeID, order.instrument.venue);
    copy(cancel.OrderRef, ref);
    cancel.FrontID = impl_->front_id;
    cancel.SessionID = impl_->session_id;
    cancel.ActionFlag = THOST_FTDC_AF_Delete;
    pending.order_id = order.id;
    pending.broker_key = ReportReconciler::key(impl_->front_id, impl_->session_id, ref);
    pending.instrument = order.instrument;
    pending.side = order.side;
    pending.offset = offset;
    pending.quantity = order.quantity;
    pending.limit_price = order.limit_price;
  }
  command.exposure_revision = exposure_revision;
  return std::make_unique<Impl::Prepared>(impl_.get(), std::move(command), std::move(pending));
}
std::future<BrokerDispatchResult> Trader::dispatch(std::unique_ptr<PreparedBrokerOrder> prepared,
                                                   BrokerSendPermit permit,
                                                   std::uint64_t journal_sequence) {
  auto* request = dynamic_cast<Impl::Prepared*>(prepared.get());
  if (!request || request->owner != impl_.get())
    throw std::logic_error("prepared order belongs to another execution port");
  auto command = std::move(request->command);
  const auto& pending = request->order();
  command.order_id = pending.order_id;
  command.permit = std::move(permit);
  const auto refused = [](int code) {
    std::promise<BrokerDispatchResult> result;
    result.set_value({code, false});
    return result.get_future();
  };
  std::future<BrokerDispatchResult> result;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->order_in_flight)
      return refused(-1006);
    if (impl_->closing || command.generation != impl_->generation) {
      // Journaled but never sent: do not insert a local rejection into the
      // newly synchronized broker snapshot as if the broker had confirmed it.
      return refused(-1003);
    }
    if (std::chrono::steady_clock::now() >= command.deadline) {
      return refused(-1005);
    }
    if (command.exposure_revision != impl_->state.exposure_revision ||
        (pending.offset == Offset::open && !impl_->state.positions_reconciled)) {
      return refused(-1004);
    }
    impl_->reports.remember({impl_->state.trading_day, pending.broker_key},
                            {pending.order_id, journal_sequence});
    command.request_id = ++impl_->request_id;
    command.order.RequestID = command.request_id;
    impl_->order_requests.emplace(command.request_id, Impl::OrderRequest{pending, command.action});
    result = impl_->enqueue(std::move(command));
    impl_->order_in_flight = true;
  }
  impl_->wake.notify_all();
  return result;
}

std::future<BrokerDispatchResult> Trader::cancel(const std::string& order_id) {
  Command command;
  command.kind = Kind::cancel;
  std::future<BrokerDispatchResult> result;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->closing || (impl_->state.phase != "ready" && impl_->state.error_code != -1008))
      throw Error(ErrorCode::unavailable, "CTP trading session is not ready");
    const auto* order = impl_->reports.find_order(order_id);
    if (order && (order->status == BrokerOrderStatus::filled ||
                  order->status == BrokerOrderStatus::cancelled ||
                  order->status == BrokerOrderStatus::rejected))
      throw Error(ErrorCode::conflict, "order is no longer working");
    auto& action = command.action;
    if (const auto pending = std::ranges::find_if(
            impl_->order_requests,
            [&](const auto& entry) { return entry.second.order.order_id == order_id; });
        pending != impl_->order_requests.end())
      action = pending->second.cancel;
    else {
      if (!order)
        throw Error(ErrorCode::not_found, "unknown order ID");
      copy(action.BrokerID, impl_->config.broker);
      copy(action.InvestorID, impl_->config.user);
      copy(action.UserID, impl_->config.user);
      action.ActionFlag = THOST_FTDC_AF_Delete;
      copy(action.InstrumentID, order->instrument.symbol);
      copy(action.ExchangeID, order->instrument.venue);
      const auto& text = order->broker_key;
      const auto first = text.find(':'), second = text.find(':', first + 1);
      action.FrontID = std::stoi(text.substr(0, first));
      action.SessionID = std::stoi(text.substr(first + 1, second - first - 1));
      copy(action.OrderRef, text.substr(second + 1));
    }
    if (order && !order->exchange_order_id.empty()) {
      const auto& raw = impl_->reports.raw_exchange_id(order->broker_key);
      std::copy(raw.begin(), raw.end(), std::begin(action.OrderSysID));
    }
    command.generation = impl_->generation;
    result = impl_->enqueue(std::move(command));
  }
  impl_->wake.notify_all();
  return result;
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
std::future<std::optional<BrokerQuote>> Trader::quote(const InstrumentId& instrument) {
  instrument.validate();
  std::future<std::optional<BrokerQuote>> result;
  {
    std::lock_guard lock(impl_->mutex);
    if (impl_->closing || impl_->state.phase != "ready")
      throw Error(ErrorCode::unavailable, "CTP trading session is not ready");
    Command command;
    command.kind = Kind::query_quote;
    command.generation = impl_->generation;
    command.instrument = instrument;
    command.serial = ++impl_->quote_serial;
    command.deadline = std::chrono::steady_clock::now() + query_timeout;
    auto& answer = impl_->quotes[command.serial];
    answer.instrument = instrument;
    answer.generation = command.generation;
    answer.deadline = command.deadline;
    result = answer.result.get_future();
    impl_->queue.push_back(std::move(command));
  }
  impl_->wake.notify_all();
  return result;
}
void Trader::poll() {
  std::lock_guard lock(impl_->mutex);
  impl_->poll_owner();
}
std::chrono::steady_clock::time_point Trader::next_deadline() const {
  std::lock_guard lock(impl_->mutex);
  auto deadline = impl_->query_outstanding ? impl_->query_started + query_timeout
                                           : std::chrono::steady_clock::time_point::max();
  for (const auto& [_, quote] : impl_->quotes)
    deadline = std::min(deadline, quote.deadline);
  return deadline;
}
bool Trader::ready() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->state.phase == "ready" && impl_->state.positions_reconciled;
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
void Trader::disconnect_checked(std::uint64_t generation, std::uint64_t exposure_revision) {
  impl_->close(std::pair{generation, exposure_revision});
}
} // namespace asterion::ctp
