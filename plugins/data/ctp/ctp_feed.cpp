#include "ctp_feed.hpp"
#include "ctp_support.hpp"
#include <ThostFtdcMdApi.h>
#include <asterion/foundation/error.hpp>
#include <asterion/kernel/process/child.hpp>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <locale>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <thread>
#include <utility>
#include <stdexcept>
namespace asterion::ctp {
void validate_instruments(const std::vector<InstrumentId>& ids) {
  if (ids.size() > 20050)
    throw std::invalid_argument("market subscriptions exceed catalog capacity");
  std::set<std::string> symbols;
  for (const auto& id : ids) {
    id.validate();
    if (!std::set<std::string>{"SHFE", "DCE", "CZCE", "CFFEX", "INE", "GFEX"}.contains(id.venue) ||
        !std::regex_match(id.symbol, std::regex("[A-Za-z]{1,3}[0-9]{3,4}")) ||
        !symbols.insert(id.symbol).second)
      throw std::invalid_argument("use unique dated futures contracts and a supported venue");
  }
}
std::optional<Decimal> price(double value) {
  if (!std::isfinite(value) || std::abs(value) > 1e10)
    return {};
  try {
    // Floating-point to_chars requires macOS 13.3; the desktop supports 13.0.
    // Keep the vendor boundary's eight decimal places independent of UI locale.
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text.setf(std::ios::fixed, std::ios::floatfield);
    text.precision(8);
    text << value;
    if (!text)
      return {};
    return Decimal::parse(text.str());
  } catch (...) {
    return {};
  }
}
std::optional<Decimal> open_interest_change(double current, double previous) {
  const auto now = price(current), before = price(previous);
  if (!now || !before || current < 0 || previous < 0)
    return {};
  return *now - *before;
}
std::optional<Decimal> average_price(double value, const std::string& venue,
                                     std::optional<int> multiplier) {
  const auto raw = price(value);
  // Zero means no trade yet in this trading day.
  if (!raw || *raw <= Decimal{})
    return {};
  if (venue == "CZCE")
    return raw;
  if (!multiplier || *multiplier <= 0)
    return {};
  return divide(*raw, Decimal::parse(std::to_string(*multiplier)), Rounding::half_even);
}
MarketDepthLevel depth_level(double value, int quantity) {
  const auto converted = price(value);
  // SDK zero-initialized unused levels have neither a price nor a quantity.
  if (!converted || (converted->raw() == 0 && quantity <= 0))
    return {};
  return {converted, quantity >= 0 ? std::optional<std::int64_t>(quantity) : std::nullopt};
}
std::int64_t source_time(const std::string& day, const std::string& time, int millisecond) {
  if (day.size() != 8 || time.size() != 8 || time[2] != ':' || time[5] != ':' || millisecond < 0 ||
      millisecond > 999)
    return 0;
  auto number = [](const std::string& s, std::size_t pos, std::size_t count) {
    int n = -1;
    const auto [p, e] = std::from_chars(s.data() + pos, s.data() + pos + count, n);
    return e == std::errc{} && p == s.data() + pos + count ? n : -1;
  };
  const auto y = number(day, 0, 4), m = number(day, 4, 2), d = number(day, 6, 2),
             h = number(time, 0, 2), min = number(time, 3, 2), sec = number(time, 6, 2);
  const std::chrono::year_month_day date{std::chrono::year{y},
                                         std::chrono::month{static_cast<unsigned>(m)},
                                         std::chrono::day{static_cast<unsigned>(d)}};
  if (!date.ok() || y < 2000 || h < 0 || h > 23 || min < 0 || min > 59 || sec < 0 || sec > 59)
    return 0;
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::sys_days{date}.time_since_epoch())
             .count() +
         ((h - 8) * 3600LL + min * 60 + sec) * 1000 + millisecond;
}
struct Feed::Impl final : CThostFtdcMdSpi {
  mutable std::recursive_mutex mutex;
  std::filesystem::path flow;
  Configuration config;
  CThostFtdcMdApi* api = nullptr;
  std::unique_ptr<SharedLibrary> library;
  using Factory = CThostFtdcMdApi* (*)(const char*, bool, bool);
  Factory factory = nullptr;
  LiveMarketSnapshot state;
  const std::string stream_id = unique_process_id();
  const std::size_t event_capacity;
  std::deque<MarketEvent> events;
  std::size_t retained_rows = 0;
  static std::size_t event_rows(const MarketEvent& event) {
    const auto* status = std::get_if<LiveMarketSnapshot>(&event.value);
    return 1 + (status ? status->subscriptions.size() : 0);
  }
  std::uint64_t event_sequence = 0;
  bool event_failed = false;
  void record(MarketEvent event) noexcept {
    if (event_failed)
      return;
    try {
      if (event_sequence == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("market event sequence exhausted");
      event.sequence = event_sequence + 1;
      const auto rows = event_rows(event);
      events.push_back(std::move(event));
      retained_rows += rows;
      ++event_sequence;
      // Full-market status snapshots are larger than individual quotes.
      // Bound retained rows as well as event count; normal gap reporting applies.
      while (events.size() > event_capacity || retained_rows > 65536) {
        retained_rows -= event_rows(events.front());
        events.pop_front();
      }
    } catch (...) {
      event_failed = true;
    }
  }
  void record_status() noexcept {
    try {
      auto status = state;
      for (auto& subscription : status.subscriptions)
        subscription.quote.reset();
      record({0, now_ms(), std::move(status)});
    } catch (...) {
      event_failed = true;
    }
  }
  std::vector<InstrumentId> wanted;
  std::map<InstrumentId, int> multipliers;
  bool closing = false, logged_in = false;
  bool login_pending = false, subscription_pending = false;
  std::jthread commands;
  // Background release of the previous SDK instance; see close().
  std::jthread retiring;
  explicit Impl(const std::filesystem::path& path, const std::filesystem::path& directory,
                std::size_t capacity)
      : flow(directory), event_capacity(capacity) {
    if (!capacity || capacity > 65536)
      throw std::invalid_argument("market event capacity must be 1..65536");
    library =
        std::make_unique<SharedLibrary>(path, "?CreateFtdcMdApi@CThostFtdcMdApi@@SAPEAV1@PEBD_N1@Z",
                                        "_ZN15CThostFtdcMdApi15CreateFtdcMdApiEPKcbb");
    factory = library->symbol<Factory>();
    if (!factory) {
      library.reset();
      throw Error(ErrorCode::unavailable, "CTP 6.7.7 market SDK unavailable or incompatible");
    }
  }
  void unload() { library.reset(); }
  ~Impl() {
    close();
    // The SDK must be fully released before its library is unloaded.
    finish_release();
    unload();
  }
  // Waits for a previous close() to release its SDK instance.
  void finish_release() {
    if (retiring.joinable())
      retiring.join();
  }
  // Marks the session disconnected at once and releases the vendor SDK in the
  // background: Release() can block for seconds while the SDK is still
  // retrying an unreachable front (notably on Windows), which must not hold
  // the service's request path. Idempotent; connect() and the destructor
  // wait for the release before reusing or unloading the SDK.
  void close() {
    CThostFtdcMdApi* old;
    {
      std::lock_guard lock(mutex);
      erase(config.password);
      if (closing)
        return;
      closing = true;
      logged_in = false;
      old = api;
      login_pending = subscription_pending = false;
      state.phase = "disconnected";
      ++state.sequence;
      record_status();
    }
    finish_release();
    commands.request_stop();
    retiring = std::jthread([this, old, runner = std::move(commands)]() mutable {
      // The command thread reads `api` outside the lock after checking
      // `closing`, so it must stop before the instance is released.
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
  template <class F> void callback(F action, bool quote = false) noexcept {
    try {
      std::lock_guard lock(mutex);
      if (!closing) {
        const auto previous = state.sequence;
        action();
        if (!quote && state.sequence != previous)
          record_status();
      }
    } catch (...) {
      try {
        std::lock_guard lock(mutex);
        event_failed = true;
        state.phase = "error";
        state.error_code = -1000;
        ++state.sequence;
      } catch (...) {
      }
    }
  }
  // The SDK may invoke callbacks while holding its own locks. Never call it
  // while holding our state mutex; one worker owns all login/subscription
  // calls.
  void run(std::stop_token stop) {
    std::vector<InstrumentId> sent;
    while (!stop.stop_requested()) {
      CThostFtdcReqUserLoginField request{};
      bool login = false, subscribe = false;
      std::vector<InstrumentId> desired;
      {
        std::lock_guard lock(mutex);
        if (closing)
          return;
        login = std::exchange(login_pending, false);
        if (login) {
          copy(request.BrokerID, config.broker);
          copy(request.UserID, config.user);
          copy(request.Password, config.password);
          sent.clear();
        }
        subscribe = logged_in && std::exchange(subscription_pending, false);
        if (subscribe)
          desired = wanted;
      }
      if (login) {
        const int code = api->ReqUserLogin(&request, 1);
        volatile char* secret = request.Password;
        for (std::size_t i = 0; i < sizeof(request.Password); ++i)
          secret[i] = 0;
        if (code)
          callback([&] {
            state.phase = "error";
            state.error_code = code;
            ++state.sequence;
          });
      }
      if (subscribe) {
        std::vector<std::string> removed;
        for (const auto& id : sent)
          if (std::find(desired.begin(), desired.end(), id) == desired.end())
            removed.push_back(id.symbol);
        std::vector<char*> names;
        for (auto& name : removed)
          names.push_back(name.data());
        int code = 0;
        for (std::size_t offset = 0; !code && offset < names.size(); offset += 50) {
          if (stop.stop_requested())
            return;
          code = api->UnSubscribeMarketData(
              names.data() + offset,
              static_cast<int>(std::min<std::size_t>(50, names.size() - offset)));
        }
        names.clear();
        for (auto& id : desired)
          names.push_back(id.symbol.data());
        for (std::size_t offset = 0; !code && offset < names.size(); offset += 50) {
          if (stop.stop_requested())
            return;
          code = api->SubscribeMarketData(
              names.data() + offset,
              static_cast<int>(std::min<std::size_t>(50, names.size() - offset)));
        }
        if (code)
          callback([&] {
            for (auto& sub : state.subscriptions) {
              sub.state = "error";
              sub.error_code = code;
            }
            ++state.sequence;
          });
        sent = std::move(desired);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  void OnFrontConnected() override {
    callback([&] {
      logged_in = false;
      if (config.password.empty()) {
        state.phase = "error";
        ++state.sequence;
        return;
      }
      state.phase = "logging_in";
      state.error_code = 0;
      ++state.sequence;
      login_pending = !config.password.empty();
    });
  }
  void OnFrontDisconnected(int reason) override {
    callback([&] {
      logged_in = false;
      state.phase = "reconnecting";
      state.error_code = reason;
      for (auto& s : state.subscriptions)
        s.state = "pending";
      ++state.sequence;
    });
  }
  void OnRspUserLogin(CThostFtdcRspUserLoginField*, CThostFtdcRspInfoField* info, int,
                      bool last) override {
    callback([&] {
      if (info && info->ErrorID) {
        state.phase = "error";
        state.error_code = info->ErrorID;
        logged_in = false;
        erase(config.password);
        ++state.sequence;
        return;
      }
      if (last) {
        logged_in = true;
        state.phase = "connected";
        state.error_code = 0;
        subscription_pending = true;
        ++state.sequence;
      }
    });
  }
  void OnRspSubMarketData(CThostFtdcSpecificInstrumentField* instrument,
                          CThostFtdcRspInfoField* info, int, bool) override {
    callback([&] {
      if (!instrument)
        return;
      for (auto& s : state.subscriptions)
        if (s.instrument.symbol == field(instrument->InstrumentID)) {
          s.error_code = info ? info->ErrorID : 0;
          s.state = s.error_code ? "error" : "subscribed";
          ++state.sequence;
        }
    });
  }
  void OnRspUnSubMarketData(CThostFtdcSpecificInstrumentField*, CThostFtdcRspInfoField* info, int,
                            bool) override {
    callback([&] {
      if (info && info->ErrorID) {
        state.phase = "error";
        state.error_code = info->ErrorID;
        ++state.sequence;
      }
    });
  }
  void OnRspError(CThostFtdcRspInfoField* info, int, bool) override {
    callback([&] {
      if (info && info->ErrorID) {
        state.phase = "error";
        state.error_code = info->ErrorID;
        ++state.sequence;
      }
    });
  }
  void OnRtnDepthMarketData(CThostFtdcDepthMarketDataField* tick) override {
    callback(
        [&] {
          if (!tick || !logged_in)
            return;
          for (auto& s : state.subscriptions)
            if (s.instrument.symbol == field(tick->InstrumentID)) {
              const auto venue = field(tick->ExchangeID);
              if (!venue.empty() && venue != s.instrument.venue)
                return;
              MarketQuote q;
              q.instrument = s.instrument;
              q.action_day = field(tick->ActionDay);
              q.trading_day = field(tick->TradingDay);
              q.update_time = field(tick->UpdateTime);
              q.source_ms = source_time(q.action_day, q.update_time, tick->UpdateMillisec);
              q.received_ms = now_ms();
              q.last = price(tick->LastPrice);
              q.bid = price(tick->BidPrice1);
              q.ask = price(tick->AskPrice1);
              q.previous_settlement = price(tick->PreSettlementPrice);
              q.open = price(tick->OpenPrice);
              q.upper_limit = price(tick->UpperLimitPrice);
              q.lower_limit = price(tick->LowerLimitPrice);
              q.high = price(tick->HighestPrice);
              q.low = price(tick->LowestPrice);
              q.open_interest = tick->OpenInterest >= 0 ? price(tick->OpenInterest) : std::nullopt;
              q.open_interest_change =
                  open_interest_change(tick->OpenInterest, tick->PreOpenInterest);
              q.previous_close = price(tick->PreClosePrice);
              const auto multiplier = multipliers.find(s.instrument);
              q.average_price = average_price(tick->AveragePrice, s.instrument.venue,
                                              multiplier == multipliers.end()
                                                  ? std::nullopt
                                                  : std::optional<int>(multiplier->second));
              q.bid_quantity = std::max(0, tick->BidVolume1);
              q.ask_quantity = std::max(0, tick->AskVolume1);
              q.volume = std::max(0, tick->Volume);
              q.bid_levels = {depth_level(tick->BidPrice2, tick->BidVolume2),
                              depth_level(tick->BidPrice3, tick->BidVolume3),
                              depth_level(tick->BidPrice4, tick->BidVolume4),
                              depth_level(tick->BidPrice5, tick->BidVolume5)};
              q.ask_levels = {depth_level(tick->AskPrice2, tick->AskVolume2),
                              depth_level(tick->AskPrice3, tick->AskVolume3),
                              depth_level(tick->AskPrice4, tick->AskVolume4),
                              depth_level(tick->AskPrice5, tick->AskVolume5)};
              const bool out_of_order = s.quote && q.source_ms && s.quote->source_ms > q.source_ms;
              record({0, q.received_ms, MarketQuoteObservation{q, out_of_order}});
              if (out_of_order) {
                ++state.out_of_order;
                ++state.sequence;
                return;
              }
              s.quote = std::move(q);
              ++state.sequence;
              return;
            }
        },
        true);
  }
};
Feed::Feed(const std::filesystem::path& library, const std::filesystem::path& flow,
           std::size_t event_capacity)
    : impl_(std::make_unique<Impl>(library, flow, event_capacity)) {}
Feed::~Feed() = default;
PluginDescriptor Feed::descriptor() const {
  return {"asterion.data.ctp", PluginKind::data, plugin_contract_version, {}};
}
void Feed::start() {
  if (!impl_->factory)
    throw Error(ErrorCode::unavailable, "CTP SDK unavailable");
}
void Feed::stop() noexcept {
  try {
    disconnect();
  } catch (...) {
  }
}
void Feed::connect(Configuration config, const std::vector<InstrumentId>& ids) {
  validate_instruments(ids);
  if (!std::regex_match(config.front, std::regex("tcp://[A-Za-z0-9.-]+:[0-9]{1,5}")))
    throw std::invalid_argument("invalid CTP front address");
  const auto port = std::stoi(config.front.substr(config.front.rfind(':') + 1));
  if (port < 1 || port > 65535)
    throw std::invalid_argument("invalid CTP front port");
  CThostFtdcReqUserLoginField check{};
  copy(check.BrokerID, config.broker);
  copy(check.UserID, config.user);
  copy(check.Password, config.password);
  std::memset(check.Password, 0, sizeof(check.Password));
  impl_->close();
  impl_->finish_release();
  {
    std::lock_guard lock(impl_->mutex);
    impl_->config = std::move(config);
    impl_->wanted = ids;
    impl_->state.subscriptions.clear();
    for (const auto& id : ids)
      impl_->state.subscriptions.push_back({id, "pending", 0, {}});
    impl_->closing = false;
    impl_->state.phase = "connecting";
    impl_->state.error_code = 0;
    ++impl_->state.sequence;
    impl_->record_status();
  }
  std::filesystem::create_directories(impl_->flow);
  const auto raw = impl_->flow.u8string();
  std::string path(raw.begin(), raw.end());
  path += "/";
  impl_->api = impl_->factory(path.c_str(), false, false);
  if (!impl_->api)
    throw Error(ErrorCode::unavailable, "CTP market factory failed");
  impl_->api->RegisterSpi(impl_.get());
  impl_->api->RegisterFront(impl_->config.front.data());
  impl_->api->Init();
  impl_->commands = std::jthread([this](std::stop_token stop) {
    try {
      impl_->run(stop);
    } catch (...) {
      impl_->callback([&] {
        impl_->state.phase = "error";
        impl_->state.error_code = -1000;
        ++impl_->state.sequence;
      });
    }
  });
}
void Feed::subscribe(const std::vector<InstrumentId>& ids) {
  validate_instruments(ids);
  std::lock_guard lock(impl_->mutex);
  if (!impl_->api || impl_->closing)
    throw Error(ErrorCode::conflict, "connect market data first");
  auto previous = impl_->state.subscriptions;
  impl_->state.subscriptions.clear();
  impl_->wanted = ids;
  for (const auto& id : ids) {
    MarketSubscription s{id, "pending", 0, {}};
    for (const auto& old : previous)
      if (old.instrument == id)
        s.quote = old.quote;
    impl_->state.subscriptions.push_back(std::move(s));
  }
  impl_->subscription_pending = true;
  ++impl_->state.sequence;
  impl_->record_status();
}
void Feed::set_multipliers(std::map<InstrumentId, int> values) {
  std::lock_guard lock(impl_->mutex);
  impl_->multipliers = std::move(values);
}
LiveMarketSnapshot Feed::snapshot() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->state;
}
MarketEventBatch Feed::events_after(const std::string& stream_id, std::uint64_t cursor,
                                    std::size_t limit) const {
  std::lock_guard lock(impl_->mutex);
  if (!limit || limit > 1024 || cursor > impl_->event_sequence ||
      (stream_id.empty() ? cursor != 0 : stream_id != impl_->stream_id))
    throw std::invalid_argument("invalid market event cursor or limit");
  MarketEventBatch batch;
  batch.stream_id = impl_->stream_id;
  batch.latest_sequence = impl_->event_sequence;
  batch.oldest_sequence = impl_->events.empty() ? 0 : impl_->events.front().sequence;
  batch.gap = batch.oldest_sequence > 0 && cursor < batch.oldest_sequence - 1;
  batch.failed = impl_->event_failed;
  for (const auto& event : impl_->events) {
    if (event.sequence <= cursor)
      continue;
    batch.events.push_back(event);
    if (batch.events.size() == limit)
      break;
  }
  return batch;
}
void Feed::disconnect() {
  impl_->close();
}
} // namespace asterion::ctp
