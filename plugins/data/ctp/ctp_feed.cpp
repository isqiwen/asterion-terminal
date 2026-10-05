#include "ctp_feed.hpp"
#include "ctp_support.hpp"
#include <asterion/foundation/bounded_queue.hpp>
#include <atomic>
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
#include <unordered_map>
#include <stdexcept>
namespace asterion::ctp {
void validate_instruments(const std::vector<InstrumentId>& ids) {
  if (ids.size() > 20050)
    throw std::invalid_argument("market subscriptions exceed catalog capacity");
  static const std::set<std::string> venues{"SHFE", "DCE", "CZCE", "CFFEX", "INE", "GFEX"};
  static const std::regex contract("[A-Za-z]{1,3}[0-9]{3,4}");
  std::set<std::string> symbols;
  for (const auto& id : ids) {
    id.validate();
    if (!venues.contains(id.venue) || !std::regex_match(id.symbol, contract) ||
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
struct Feed::Impl final {
  // Only SDK command handoff uses this mutex. Normalized state belongs to poll().
  std::mutex mutex;
  struct Connected {};
  struct Disconnected {
    int reason;
  };
  struct Login {
    int code;
    bool last;
  };
  struct Subscription {
    CThostFtdcSpecificInstrumentField instrument;
    int code;
  };
  struct Failure {
    int code;
  };
  struct SubscriptionFailure {
    int code;
  };
  struct Tick {
    CThostFtdcDepthMarketDataField value;
  };
  using Observation = std::variant<Connected, Disconnected, Login, Subscription, Failure,
                                   SubscriptionFailure, Tick>;
  struct Incoming {
    std::uint64_t generation;
    std::int64_t received_ms;
    Observation value;
  };
  // Enough for the initial 20,000-contract acknowledgement/quote burst. Raw
  // callback storage is bounded independently from normalized event retention.
  BoundedQueue<Incoming> incoming{65536};
  std::atomic<bool> ingress_failed{false};
  std::uint64_t generation = 0;
  template <class T> void deliver(std::uint64_t entry, T value) noexcept {
    try {
      if (!incoming.try_push({entry, now_ms(), std::move(value)}))
        ingress_failed = true;
    } catch (...) {
      // No exception may escape into the vendor callback ABI.
      ingress_failed = true;
    }
  }
  const std::filesystem::path library_path, flow;
  Configuration config;
  using Factory = CThostFtdcMdApi* (*)(const char*, bool, bool);
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
  void record_status(std::int64_t received_ms = now_ms()) noexcept {
    std::fill(subscription_revisions.begin(), subscription_revisions.end(), state.sequence);
    try {
      auto status = state;
      for (auto& subscription : status.subscriptions)
        subscription.quote.reset();
      record({0, received_ms, std::move(status)});
    } catch (...) {
      event_failed = true;
    }
  }
  std::vector<InstrumentId> wanted;
  std::unordered_map<std::string, std::size_t> subscription_index;
  std::vector<std::uint64_t> subscription_revisions;
  std::uint64_t membership_sequence = 0;
  void select_subscriptions(const std::vector<InstrumentId>& ids, bool preserve_quotes) {
    auto next_wanted = ids;
    std::vector<MarketSubscription> next;
    std::unordered_map<std::string, std::size_t> index;
    next.reserve(ids.size());
    index.reserve(ids.size());
    for (const auto& id : ids) {
      MarketSubscription subscription{id, "pending", 0, {}};
      const auto old = subscription_index.find(id.symbol);
      if (preserve_quotes && old != subscription_index.end() &&
          state.subscriptions[old->second].instrument == id)
        subscription.quote = state.subscriptions[old->second].quote;
      index.emplace(id.symbol, next.size());
      next.push_back(std::move(subscription));
    }
    // The state owner publishes the vector and indices together.
    std::vector<std::uint64_t> revisions(ids.size(), state.sequence + 1);
    state.subscriptions = std::move(next);
    subscription_revisions = std::move(revisions);
    membership_sequence = state.sequence + 1;
    subscription_index = std::move(index);
    wanted = std::move(next_wanted);
  }
  std::map<InstrumentId, int> multipliers;
  bool closing = true, logged_in = false;
  bool login_pending = false, subscription_pending = false;
  struct Bridge final : CThostFtdcMdSpi {
    Impl& owner;
    const std::uint64_t generation;
    Bridge(Impl& owner, std::uint64_t generation) : owner(owner), generation(generation) {}
    void OnFrontConnected() override { owner.deliver(generation, Connected{}); }
    void OnFrontDisconnected(int reason) override {
      owner.deliver(generation, Disconnected{reason});
    }
    void OnRspUserLogin(CThostFtdcRspUserLoginField*, CThostFtdcRspInfoField* info, int,
                        bool last) override {
      owner.deliver(generation, Login{info ? info->ErrorID : 0, last});
    }
    void OnRspSubMarketData(CThostFtdcSpecificInstrumentField* instrument,
                            CThostFtdcRspInfoField* info, int, bool) override {
      if (instrument)
        owner.deliver(generation, Subscription{*instrument, info ? info->ErrorID : 0});
    }
    void OnRspUnSubMarketData(CThostFtdcSpecificInstrumentField*, CThostFtdcRspInfoField* info, int,
                              bool) override {
      if (info && info->ErrorID)
        owner.deliver(generation, Failure{info->ErrorID});
    }
    void OnRspError(CThostFtdcRspInfoField* info, int, bool) override {
      if (info && info->ErrorID)
        owner.deliver(generation, Failure{info->ErrorID});
    }
    void OnRtnDepthMarketData(CThostFtdcDepthMarketDataField* tick) override {
      if (tick)
        owner.deliver(generation, Tick{*tick});
    }
  };
  // Every vendor call, its SPI and the loaded library live on the SDK owner.
  struct Sdk {
    SharedLibrary library;
    Bridge bridge;
    CThostFtdcMdApi* api = nullptr;
    std::string front;
    Sdk(Impl& owner, std::uint64_t generation, std::string address)
        : library(owner.library_path, "?CreateFtdcMdApi@CThostFtdcMdApi@@SAPEAV1@PEBD_N1@Z",
                  "_ZN15CThostFtdcMdApi15CreateFtdcMdApiEPKcbb"),
          bridge(owner, generation), front(std::move(address)) {}
    ~Sdk() {
      if (api) {
        api->RegisterSpi(nullptr);
        api->Release();
      }
    }
    void start(const std::filesystem::path& flow) {
      const auto factory = library.symbol<Factory>();
      if (!factory)
        throw Error(ErrorCode::unavailable, "CTP 6.7.7 market SDK unavailable or incompatible");
      std::filesystem::create_directories(flow);
      const auto raw = flow.u8string();
      const std::string path = std::string(raw.begin(), raw.end()) + "/";
      api = factory(path.c_str(), false, false);
      if (!api)
        throw Error(ErrorCode::unavailable, "CTP market factory failed");
      api->RegisterSpi(&bridge);
      api->RegisterFront(front.data());
      api->Init();
    }
  };
  struct SdkState {
    std::unique_ptr<Sdk> sdk;
    std::uint64_t generation = 0;
    std::vector<InstrumentId> sent;
  };
  ThreadPool& sdk_owner;
  std::unique_ptr<SdkState> sdk_state = std::make_unique<SdkState>();
  std::future<void> pending;
  explicit Impl(ThreadPool& owner, const std::filesystem::path& path,
                const std::filesystem::path& directory, std::size_t capacity)
      : library_path(path), flow(directory), event_capacity(capacity), sdk_owner(owner) {
    if (!capacity || capacity > 65536)
      throw std::invalid_argument("market event capacity must be 1..65536");
  }
  ~Impl() {
    if (pending.valid())
      pending.get();
    sdk_owner.submit([this](std::stop_token) { sdk_state.reset(); }).get();
    erase(config.password);
  }
  void close() {
    std::lock_guard lock(mutex);
    erase(config.password);
    if (closing)
      return;
    closing = true;
    logged_in = login_pending = subscription_pending = false;
    state.phase = "disconnected";
    ++state.sequence;
    record_status();
  }
  bool current(std::uint64_t value) {
    std::lock_guard lock(mutex);
    return !closing && !ingress_failed && generation == value;
  }
  void send_commands(Sdk& sdk, std::uint64_t sdk_generation, std::vector<InstrumentId>& sent,
                     std::stop_token stop) {
    CThostFtdcReqUserLoginField request{};
    bool login = false, subscribe = false;
    std::vector<InstrumentId> desired;
    {
      std::lock_guard lock(mutex);
      if (closing || generation != sdk_generation)
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
      const int code = sdk.api->ReqUserLogin(&request, 1);
      volatile char* secret = request.Password;
      for (std::size_t i = 0; i < sizeof(request.Password); ++i)
        secret[i] = 0;
      if (code)
        deliver(sdk_generation, Failure{code});
    }
    if (subscribe) {
      std::vector<std::string> removed;
      const std::set<InstrumentId> desired_set(desired.begin(), desired.end());
      for (const auto& id : sent)
        if (!desired_set.contains(id))
          removed.push_back(id.symbol);
      std::vector<char*> names;
      for (auto& name : removed)
        names.push_back(name.data());
      int code = 0;
      for (std::size_t offset = 0; !code && offset < names.size(); offset += 50) {
        if (stop.stop_requested() || !current(sdk_generation))
          return;
        code = sdk.api->UnSubscribeMarketData(
            names.data() + offset,
            static_cast<int>(std::min<std::size_t>(50, names.size() - offset)));
      }
      names.clear();
      for (auto& id : desired)
        names.push_back(id.symbol.data());
      for (std::size_t offset = 0; !code && offset < names.size(); offset += 50) {
        if (stop.stop_requested() || !current(sdk_generation))
          return;
        code = sdk.api->SubscribeMarketData(
            names.data() + offset,
            static_cast<int>(std::min<std::size_t>(50, names.size() - offset)));
      }
      if (code)
        deliver(sdk_generation, SubscriptionFailure{code});
      sent = std::move(desired);
    }
  }
  void advance_sdk(std::stop_token stop) {
    auto& sdk = sdk_state->sdk;
    auto& sdk_generation = sdk_state->generation;
    auto& sent = sdk_state->sent;
    std::uint64_t desired;
    {
      std::lock_guard lock(mutex);
      desired = closing || ingress_failed ? 0 : generation;
    }
    try {
      if (desired != sdk_generation) {
        sdk.reset(); // Release completes before another API or SPI is created.
        if (stop.stop_requested())
          return;
        std::string front;
        {
          std::lock_guard lock(mutex);
          sdk_generation = closing || ingress_failed ? 0 : generation;
          front = config.front;
        }
        sent.clear();
        if (sdk_generation) {
          sdk = std::make_unique<Sdk>(*this, sdk_generation, std::move(front));
          sdk->start(flow);
        }
      }
      if (sdk)
        send_commands(*sdk, sdk_generation, sent, stop);
    } catch (const std::exception&) {
      // An explicit new connection is required after creation or SDK failure.
      deliver(sdk_generation, Failure{-1000});
      sdk.reset();
    }
  }
  void quote(const CThostFtdcDepthMarketDataField* tick, std::int64_t received_ms) {
    if (!logged_in)
      return;
    const auto found = subscription_index.find(field(tick->InstrumentID));
    if (found == subscription_index.end())
      return;
    auto& s = state.subscriptions[found->second];
    const auto venue = field(tick->ExchangeID);
    if (!venue.empty() && venue != s.instrument.venue)
      return;
    MarketQuote q;
    q.instrument = s.instrument;
    q.action_day = field(tick->ActionDay);
    q.trading_day = field(tick->TradingDay);
    q.update_time = field(tick->UpdateTime);
    q.source_ms = source_time(q.action_day, q.update_time, tick->UpdateMillisec);
    q.received_ms = received_ms;
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
    q.open_interest_change = open_interest_change(tick->OpenInterest, tick->PreOpenInterest);
    q.previous_close = price(tick->PreClosePrice);
    const auto multiplier = multipliers.find(s.instrument);
    q.average_price = average_price(
        tick->AveragePrice, s.instrument.venue,
        multiplier == multipliers.end() ? std::nullopt : std::optional<int>(multiplier->second));
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
    subscription_revisions[found->second] = state.sequence;
  }
  void apply(const Incoming& event) {
    std::visit(
        [&](const auto& value) {
          using T = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<T, Tick>) {
            quote(&value.value, event.received_ms);
          } else {
            std::lock_guard lock(mutex);
            const auto before = state.sequence;
            if constexpr (std::is_same_v<T, Connected>) {
              logged_in = false;
              state.phase = config.password.empty() ? "error" : "logging_in";
              state.error_code = 0;
              login_pending = !config.password.empty();
              ++state.sequence;
            } else if constexpr (std::is_same_v<T, Disconnected>) {
              logged_in = false;
              state.phase = "reconnecting";
              state.error_code = value.reason;
              for (auto& sub : state.subscriptions)
                sub.state = "pending";
              ++state.sequence;
            } else if constexpr (std::is_same_v<T, Login>) {
              if (value.code) {
                state.phase = "error";
                state.error_code = value.code;
                logged_in = false;
                erase(config.password);
                ++state.sequence;
              } else if (value.last) {
                logged_in = true;
                state.phase = "connected";
                state.error_code = 0;
                subscription_pending = true;
                ++state.sequence;
              }
            } else if constexpr (std::is_same_v<T, Subscription>) {
              const auto found = subscription_index.find(field(value.instrument.InstrumentID));
              if (found == subscription_index.end())
                return;
              auto& sub = state.subscriptions[found->second];
              sub.error_code = value.code;
              sub.state = value.code ? "error" : "subscribed";
              ++state.sequence;
              subscription_revisions[found->second] = state.sequence;
              record({0, event.received_ms,
                      MarketSubscription{sub.instrument, sub.state, sub.error_code, {}}});
              return;
            } else if constexpr (std::is_same_v<T, Failure>) {
              if (value.code) {
                state.phase = "error";
                state.error_code = value.code;
                ++state.sequence;
              }
            } else if constexpr (std::is_same_v<T, SubscriptionFailure>) {
              for (auto& sub : state.subscriptions) {
                sub.state = "error";
                sub.error_code = value.code;
              }
              ++state.sequence;
            }
            if (state.sequence != before)
              record_status(event.received_ms);
          }
        },
        event.value);
  }
  void poll() {
    if (!closing && (ingress_failed || event_failed)) {
      event_failed = true;
      if (state.phase != "error" || state.error_code != -1000) {
        state.phase = "error";
        state.error_code = -1000;
        ++state.sequence;
      }
      ingress_failed = true;
    }
    for (unsigned i = 0; i < 1024; ++i) {
      auto event = incoming.try_pop();
      if (!event)
        break;
      if (!closing && !event_failed && !ingress_failed && event->generation == generation)
        apply(*event);
    }
    if (pending.valid()) {
      if (pending.wait_for(std::chrono::milliseconds{0}) != std::future_status::ready)
        return;
      pending.get();
    }
    pending = sdk_owner.submit([this](std::stop_token stop) { advance_sdk(stop); });
  }
};
Feed::Feed(ThreadPool& sdk_owner, const std::filesystem::path& library,
           const std::filesystem::path& flow, std::size_t event_capacity)
    : impl_(std::make_unique<Impl>(sdk_owner, library, flow, event_capacity)) {}
Feed::~Feed() = default;
PluginDescriptor Feed::descriptor() const {
  return {"asterion.data.ctp", PluginKind::data, plugin_contract_version, {}};
}
void Feed::start() {
  if (impl_->library_path.empty())
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
  start();
  if (impl_->event_failed || impl_->ingress_failed)
    throw Error(ErrorCode::unavailable, "market event stream failed; restart market service");
  if (!impl_->closing && impl_->state.phase != "error")
    throw Error(ErrorCode::conflict, "disconnect current market session first");
  impl_->close();
  std::lock_guard lock(impl_->mutex);
  impl_->config = std::move(config);
  impl_->select_subscriptions(ids, false);
  ++impl_->generation;
  impl_->closing = false;
  impl_->state.phase = "connecting";
  impl_->state.error_code = 0;
  ++impl_->state.sequence;
  impl_->record_status();
}
void Feed::subscribe(const std::vector<InstrumentId>& ids) {
  validate_instruments(ids);
  std::lock_guard lock(impl_->mutex);
  if (impl_->closing)
    throw Error(ErrorCode::conflict, "connect market data first");
  impl_->select_subscriptions(ids, true);
  impl_->subscription_pending = true;
  ++impl_->state.sequence;
  impl_->record_status();
}
void Feed::set_multipliers(std::map<InstrumentId, int> values) {
  impl_->multipliers = std::move(values);
}
LiveMarketSnapshot Feed::snapshot(std::optional<std::uint64_t> after,
                                  std::span<const InstrumentId> forced) const {
  const auto& state = impl_->state;
  if (after && *after > state.sequence)
    throw std::invalid_argument("invalid market snapshot cursor");
  if (!after || *after < impl_->membership_sequence)
    return state;
  LiveMarketSnapshot out;
  out.phase = state.phase;
  out.error_code = state.error_code;
  out.sequence = state.sequence;
  out.out_of_order = state.out_of_order;
  out.subscriptions_delta = true;
  // Scanning compact revision counters does not copy unchanged quote trees.
  std::vector<std::size_t> selected;
  for (std::size_t i = 0; i < impl_->subscription_revisions.size(); ++i)
    if (impl_->subscription_revisions[i] > *after)
      selected.push_back(i);
  for (const auto& id : forced) {
    const auto found = impl_->subscription_index.find(id.symbol);
    if (found != impl_->subscription_index.end() &&
        state.subscriptions[found->second].instrument == id)
      selected.push_back(found->second);
  }
  std::sort(selected.begin(), selected.end());
  selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
  out.subscriptions.reserve(selected.size());
  for (const auto i : selected)
    out.subscriptions.push_back(state.subscriptions[i]);
  return out;
}
std::string Feed::phase() const {
  return impl_->state.phase;
}
MarketEventBatch Feed::events_after(const std::string& stream_id, std::uint64_t cursor,
                                    std::size_t limit) const {
  if (!limit || limit > 1024 || cursor > impl_->event_sequence ||
      (stream_id.empty() ? cursor != 0 : stream_id != impl_->stream_id))
    throw std::invalid_argument("invalid market event cursor or limit");
  MarketEventBatch batch;
  batch.stream_id = impl_->stream_id;
  batch.latest_sequence = impl_->event_sequence;
  batch.oldest_sequence = impl_->events.empty() ? 0 : impl_->events.front().sequence;
  batch.gap = batch.oldest_sequence > 0 && cursor < batch.oldest_sequence - 1;
  batch.failed = impl_->event_failed;
  // The deque retains a contiguous suffix of sequence numbers. Seeking by
  // offset avoids scanning every retained observation on each incremental read.
  const auto offset = batch.oldest_sequence && cursor >= batch.oldest_sequence
                          ? static_cast<std::size_t>(cursor - batch.oldest_sequence + 1)
                          : 0;
  const auto count = std::min(limit, impl_->events.size() - offset);
  batch.events.reserve(count);
  for (std::size_t i = 0; i < count; ++i)
    batch.events.push_back(impl_->events[offset + i]);
  return batch;
}
void Feed::poll() {
  impl_->poll();
}
void Feed::disconnect() {
  impl_->close();
}
} // namespace asterion::ctp
