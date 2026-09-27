#include "ctp_feed.hpp"
#include <ThostFtdcMdApi.h>
#include <asterion/foundation/error.hpp>
#include <asterion/kernel/process/child.hpp>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <thread>
#include <utility>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
namespace asterion::ctp {
namespace {
std::int64_t now() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
template <std::size_t N> std::string field(const char (&value)[N]) {
  return {value, std::find(value, value + N, '\0')};
}
template <std::size_t N> void copy(char (&dest)[N], const std::string& value) {
  if (value.empty() || value.size() >= N || value.find('\0') != std::string::npos)
    throw std::invalid_argument("invalid CTP credential length");
  std::memcpy(dest, value.data(), value.size());
}
void erase(std::string& value) {
  volatile char* p = value.data();
  for (std::size_t i = 0; i < value.size(); ++i)
    p[i] = 0;
  value.clear();
}
} // namespace
void validate_instruments(const std::vector<InstrumentId>& ids) {
  if (ids.size() > 50)
    throw std::invalid_argument("at most 50 market subscriptions");
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
  char text[96];
  const auto result = std::to_chars(text, text + sizeof(text), value, std::chars_format::fixed, 8);
  if (result.ec != std::errc{})
    return {};
  try {
    return Decimal::parse(std::string(text, result.ptr));
  } catch (...) {
    return {};
  }
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
#ifdef _WIN32
  HMODULE library = nullptr;
#else
  void* library = nullptr;
#endif
  using Factory = CThostFtdcMdApi* (*)(const char*, bool, bool);
  Factory factory = nullptr;
  LiveMarketSnapshot state;
  const std::string stream_id = unique_process_id();
  const std::size_t event_capacity;
  std::deque<MarketEvent> events;
  std::uint64_t event_sequence = 0;
  bool event_failed = false;
  void record(MarketEvent event) noexcept {
    if (event_failed)
      return;
    try {
      if (event_sequence == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("market event sequence exhausted");
      event.sequence = event_sequence + 1;
      events.push_back(std::move(event));
      ++event_sequence;
      if (events.size() > event_capacity)
        events.pop_front();
    } catch (...) {
      event_failed = true;
    }
  }
  void record_status() noexcept {
    try {
      auto status = state;
      for (auto& subscription : status.subscriptions)
        subscription.quote.reset();
      record({0, now(), std::move(status)});
    } catch (...) {
      event_failed = true;
    }
  }
  std::vector<InstrumentId> wanted;
  bool closing = false, logged_in = false;
  bool login_pending = false, subscription_pending = false;
  std::jthread commands;
  explicit Impl(const std::filesystem::path& path, const std::filesystem::path& directory,
                std::size_t capacity)
      : flow(directory), event_capacity(capacity) {
    if (!capacity || capacity > 65536)
      throw std::invalid_argument("market event capacity must be 1..65536");
#ifdef _WIN32
    library = LoadLibraryExW(path.c_str(), nullptr,
                             LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (library)
      factory = reinterpret_cast<Factory>(
          GetProcAddress(library, "?CreateFtdcMdApi@CThostFtdcMdApi@@SAPEAV1@PEBD_N1@Z"));
#else
    library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (library)
      factory =
          reinterpret_cast<Factory>(dlsym(library, "_ZN15CThostFtdcMdApi15CreateFtdcMdApiEPKcbb"));
#endif
    if (!factory) {
      unload();
      throw Error(ErrorCode::unavailable, "CTP 6.7.7 market SDK unavailable or incompatible");
    }
  }
  void unload() {
#ifdef _WIN32
    if (library)
      FreeLibrary(library);
#else
    if (library)
      dlclose(library);
#endif
    library = nullptr;
  }
  ~Impl() {
    close();
    unload();
  }
  void close() {
    CThostFtdcMdApi* old;
    {
      std::lock_guard lock(mutex);
      closing = true;
      logged_in = false;
      old = api;
      login_pending = subscription_pending = false;
      state.phase = "disconnected";
      ++state.sequence;
      record_status();
    }
    commands.request_stop();
    if (commands.joinable())
      commands.join();
    api = nullptr;
    if (old) {
      old->RegisterSpi(nullptr);
      old->Release();
    }
    std::lock_guard lock(mutex);
    erase(config.password);
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
        int code = names.empty()
                       ? 0
                       : api->UnSubscribeMarketData(names.data(), static_cast<int>(names.size()));
        names.clear();
        for (auto& id : desired)
          names.push_back(id.symbol.data());
        if (!code && !names.empty())
          code = api->SubscribeMarketData(names.data(), static_cast<int>(names.size()));
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
              q.received_ms = now();
              q.last = price(tick->LastPrice);
              q.bid = price(tick->BidPrice1);
              q.ask = price(tick->AskPrice1);
              q.previous_settlement = price(tick->PreSettlementPrice);
              q.high = price(tick->HighestPrice);
              q.low = price(tick->LowestPrice);
              q.open_interest = price(tick->OpenInterest);
              q.bid_quantity = std::max(0, tick->BidVolume1);
              q.ask_quantity = std::max(0, tick->AskVolume1);
              q.volume = std::max(0, tick->Volume);
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
